//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file
 * Send simulated requests/data to the control_plane_server program.
 * Behaves like an ERSAP backend.
 */

#include <cstdlib>
#include <iostream>
#include <time.h>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <errno.h>
#include <cinttypes>
#include <memory>
#include <string>
#include <stdexcept>

#include "ejfat_assemble_ersap.hpp"
#include "ejfat_network.hpp"
#include "et.h"
#include "et_fifo.h"

#ifdef __linux__
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif

// GRPC stuff
#include "lb_cplane_esnet.h"


using namespace ejfat;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------


#define INPUT_LENGTH_MAX 256

//
//template<class X>
//X pid(          // Proportional, Integrative, Derivative Controller
//        const X& setPoint, // Desired Operational Set Point
//        const X& prcsVal,  // Measure Process Value
//        const X& delta_t,  // Time delta between determination of last control value
//        const X& Kp,       // Konstant for Proprtional Control
//        const X& Ki,       // Konstant for Integrative Control
//        const X& Kd        // Konstant for Derivative Control
//)
//{
//    static X previous_error = 0; // for Derivative
//    static X integral_acc = 0;   // For Integral (Accumulated Error)
//    X error = setPoint - prcsVal;
//    integral_acc += error * delta_t;
//    X derivative = (error - previous_error) / delta_t;
//    previous_error = error;
//    return Kp * error + Ki * integral_acc + Kd * derivative;  // control output
//}



/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        -f <ET file>",
            "        [-h] [-v] [-ip6]",
            "        [-gaddr <grpc server IP address>]",
            "        [-gport <grpc server port>]",
            "        [-gname <grpc name>]",
            "        [-gid <grpc id#>]",
            "        [-s <PID fifo set point>]");

    fprintf(stderr, "        This is a gRPC program that simulates an ERSAP backend by sending messages to a simulated control plane.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param bufSize       filled with buffer size.
 * @param recvBufSize   filled with UDP receive buffer size.
 * @param tickPrescale  expected increase in tick with each incoming buffer.
 * @param cores         array of core ids on which to run assembly thread.
 * @param grpcId        filled with id# of this grpc client (backend) to send to control plane.
 * @param setPt         filled with the set point of PID loop used with fifo fill level.
 * @param port          filled with UDP receiving data port to listen on.
 * @param grpcPort      filled with grpc server port to send control plane info to.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param listenAddr    filled with IP address to listen on for LB data.
 * @param grpcAddr      filled with grpc server IP address to send control plane info to.
 * @param etFilename    filled with name of ET file in which to write data.
 * @param grpcName      filled with name of this grpc client (backend) to send to control plane.
 */
static void parseArgs(int argc, char **argv,
                      int *grpcId, float *setPt, uint16_t* grpcPort,
                      bool *debug, char *grpcAddr, char *grpcName) {

    int c, i_tmp;
    bool help = false;
    float sp = 0.;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"ip6",  0, NULL, 2},
                          {"cores",  1, NULL, 3},
                          {"gaddr",  1, NULL, 4},
                          {"gport",  1, NULL, 5},
                          {"gname",  1, NULL, 6},
                          {"gid",  1, NULL, 7},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhs:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 5:
                // grpc server PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *grpcPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -gport, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 's':
                // PID set point for fifo fill
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -s, 0.0 <= PID set point <= 100.0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }

                if (sp >= 0. && sp <= 100.) {
                    *setPt = sp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -s, 0 <= PID set point <= 100\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 7:
                // grpc client id
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *grpcId = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -gid, grpc client id must be >= 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 4:
                // GRPC server IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "grpc server IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(grpcAddr, optarg);
                break;

            case 6:
                // grpc client name
                if (strlen(optarg) > 30 || strlen(optarg) < 1) {
                    fprintf(stderr, "grpc client name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(grpcName, optarg);
                break;

            case 'v':
                // VERBOSE
                *debug = true;
                break;

            case 'h':
                help = true;
                break;

            default:
                printHelp(argv[0]);
                exit(2);
        }

    }

    if (help) {
        printHelp(argv[0]);
        exit(2);
    }
}




// structure for passing args to thread
typedef struct threadStruct_t {
    et_sys_id etId;
    uint16_t grpcServerPort;
    std::string grpcServerIpAddr;
    std::string myName;
    int32_t myId;
    float setPoint;
    bool report;
} threadStruct;


// Thread to monitor the ET system, run PID loop and report back to control plane
static void *pidThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);

    et_sys_id etId = targ->etId;
    bool reportToCp = targ->report;
    int status, numEvents, inputListCount = 0, fillPercent = 0;
    size_t eventSize;
    status = et_system_getnumevents(etId, &numEvents);
    status = et_system_geteventsize(etId, &eventSize);

    float pidError;
    float pidSetPoint = targ->setPoint;
    const float Kp = 0.5;
    const float Ki = 0.0;
    const float Kd = 0.00;
    const float deltaT = 1.0; // 1 millisec

    int loopMax   = 1000;
    int loopCount = loopMax; // 1000 loops of 1 millisec = 1 sec

    LbControlPlaneClient client(targ->grpcServerIpAddr, targ->grpcServerPort, targ->myName,
                                targ->myId, (int32_t)eventSize, numEvents,targ->setPoint);

    // Register this client with the grpc server
    int32_t err = client.Register();
    if (err == -1) {
        printf("GRPC client %s is already registered!\n", targ->myName.c_str());
    }
    else if (err == 1) {
        printf("GRPC client %s communication error with server when registering, exit!\n", targ->myName.c_str());
        exit(1);
    }

    while (true) {

        // Delay 1 milliseconds between data points
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Get the number of available events (# sitting in Grandcentral's input list)
        status = et_station_getinputcount_rt(etId, ET_GRANDCENTRAL, &inputListCount);

        fillPercent = (numEvents-inputListCount)*100/numEvents;
        pidError = pid(pidSetPoint, (float)fillPercent, deltaT, Kp, Ki, Kd);

        // Every "loopMax" loops
        if (reportToCp && --loopCount <= 0) {
            // Update the changing variables
            client.update(fillPercent, pidError);
            // Send to server
            err = client.SendState();
            if (err == -2) {
                printf("GRPC client %s cannot send data since it is not registered with server!\n", targ->myName.c_str());
                break;
            }
            else if (err == 1) {
                printf("GRPC client %s communication error with server during sending of data!\n", targ->myName.c_str());
                break;
            }

            printf("Total cnt %d, GC in list cnt %d, %d%% filled, error %f\n",
                   numEvents, inputListCount, fillPercent, pidError);

            loopCount = loopMax;
        }
    }

    // Unregister this client with the grpc server
    err = client.UnRegister();
    if (err == 1) {
        printf("GRPC client %s communication error with server when unregistering, exit!\n", targ->myName.c_str());
    }
    exit(1);

    return (nullptr);
}



int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    // Set this to max expected data size
    int grpcId = 0;
    float grpcSetPoint = 0;
    uint16_t grpcPort = 50051;
    uint16_t serverPort = 8888;
    bool debug = false;

    char grpcAddr[16];
    memset(grpcAddr, 0, 16);
    char grpcName[31];
    memset(grpcName, 0, 31);


    parseArgs(argc, argv,
              &grpcId, &grpcSetPoint, &grpcPort,
              &debug, grpcAddr,  grpcName);


    ////////////////////////////
    /// Control Plane  Stuff ///
    ////////////////////////////
    BackendReportServiceImpl service;
    BackendReportServiceImpl *pGrpcService = &service;


    if (sendToEt) {

        /******************/
        /* open ET system */
        /******************/
        et_open_config_init(&openconfig);

        et_open_config_setcast(openconfig, ET_DIRECT);
        et_open_config_sethost(openconfig, ET_HOST_LOCAL);
        et_open_config_gethost(openconfig, host);
        fprintf(stderr, "Direct ET connection to %s\n", host);

        /* debug level */
        et_open_config_setdebugdefault(openconfig, debugLevel);

        et_open_config_setwait(openconfig, ET_OPEN_WAIT);
        if (et_open(&id, filename, openconfig) != ET_OK) {
            fprintf(stderr, "et_open problems\n");
            exit(1);
        }
        et_open_config_destroy(openconfig);

        /*-------------------------------------------------------*/

        /* set level of debug output (everything) */
        et_system_setdebug(id, debugLevel);

        /***********************/
        /* Use FIFO interface  */
        /***********************/
        status = et_fifo_openProducer(id, &fid, ids, idCount);
        if (status != ET_OK) {
            fprintf(stderr, "et_fifo_open problems\n");
            exit(1);
        }

        /* no error here */
        int numRead = et_fifo_getEntryCapacity(fid);

        fprintf(stderr, "Et fifo capacity = %d, idCount = %d\n", numRead, idCount);

        entry = et_fifo_entryCreate(fid);
        if (entry == NULL) {
            fprintf(stderr, "et_fifo_entryCreate: out of mem\n");
            exit(1);
        }

        /**************************/
        /* Start a couple threads */
        /**************************/

        // Start thread to do PID "control"
        threadStruct *targ = (threadStruct *)calloc(1, sizeof(threadStruct));
        if (targ == nullptr) {
            fprintf(stderr, "out of mem\n");
            return -1;
        }

        targ->etId = id;
        targ->grpcServerPort = grpcPort;
        targ->grpcServerIpAddr = grpcAddr;
        targ->myName = grpcName;
        targ->myId = grpcId;
        targ->setPoint = grpcSetPoint;
        targ->report = reportToCP;

        pthread_t thd1;
        status = pthread_create(&thd1, NULL, pidThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating PID thread ********\n\n");
            return -1;
        }
    }

    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint16_t dataId;
    bool firstLoop = true;

    while (true) {

        clearStats(stats);
        uint64_t diff, prevTick = tick;

        // Fill with data
        nBytes = getCompletePacketizedBuffer(dataBuf, bufSize, udpSocket,
                                             debug, &tick, &dataId, stats,
                                             tickPrescale, outOfOrderPackets);
        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d), exit\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBuffer (%ld), exit\n", nBytes);
            }
            et_close(id);
            return -1;
        }

        // The first tick received may be any value depending on # of backends receiving
        // packets from load balancer. Use the first tick received and subsequent ticks
        // to check the prescale. Took prescale-checking logic out of ejfat_assemble_ersap.hpp
        // code. Checking it here makes more sense.
        if (firstLoop) {
            prevTick = tick;
            firstLoop = false;
        }

        //fprintf(stderr, "Received buffer of %d bytes, tpre %d\n", (int)nBytes, tickPrescale);

        diff = tick - prevTick;
        if (diff != 0) {
            fprintf(stderr, "Error in tick increment, %" PRIu64 "\n", diff);
        }

        totalBytes   += nBytes;
        totalPackets += stats->acceptedPackets;

        // atomic
        droppedTicks   += stats->droppedTicks;
        droppedPackets += stats->droppedPackets;

        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale;

#ifdef __linux__

        if (loopCount-- < 1) {
            cpu = sched_getcpu();
            loopCount = cpuLoops;
        }
#endif

        // Send to ET
        if (sendToEt) {

            /* Grab new/empty buffers */
            status = et_fifo_newEntry(fid, entry);
            if (status != ET_OK) {
                fprintf(stderr, "et_fifo_newEntry error\n");
                return -1;
            }

            /* Access the new buffers */
            pe = et_fifo_getBufs(entry);

            /* write data, set control values here */
            char *pdata;
            et_event_getdata(pe[0], (void **) &pdata);
            memcpy((void *)pdata, (const void *) dataBuf, nBytes);

            /* Send all data */
            et_event_setlength(pe[0], nBytes);
            et_fifo_setId(pe[0], ids[0]);
            et_fifo_setHasData(pe[0], 1);

            /* put events back into the ET system */
            status = et_fifo_putEntry(entry);
            if (status != ET_OK) {
                fprintf(stderr, "et_fifo_putEntry error\n");
                return -1;
            }
        }
    }

    return 0;
}



