//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive generated data sent by clasBlaster.c program.
 * This assumes there is an emulator or FPGA between this and the sending program.
 * Take the reassembled buffers and send to an ET system so ERSAP can grab them.
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
#include "lbcontrol.h"


using namespace ejfat;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------


#define INPUT_LENGTH_MAX 256


template<class X>
X pid(          // Proportional, Integrative, Derivative Controller
        const X& setPoint, // Desired Operational Set Point
        const X& prcsVal,  // Measure Process Value
        const X& delta_t,  // Time delta between determination of last control value
        const X& Kp,       // Konstant for Proprtional Control
        const X& Ki,       // Konstant for Integrative Control
        const X& Kd        // Konstant for Derivative Control
)
{
    static X previous_error = 0; // for Derivative
    static X integral_acc = 0;   // For Integral (Accumulated Error)
    X error = setPoint - prcsVal;
    integral_acc += error * delta_t;
    X derivative = (error - previous_error) / delta_t;
    previous_error = error;
    return Kp * error + Ki * integral_acc + Kd * derivative;  // control output
}



/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        -f <ET file>",
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer byte size>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with clasBlaster and send data to an ET system.\n");
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
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param listenAddr    filled with IP address to listen on.
 * @param filename      filled with name of file in which to write stats.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      int *cores, uint16_t* port,
                      bool *debug, bool *useIPv6,
                      char *listenAddr, char *filename) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, NULL, 1},
                          {"ip6",  0, NULL, 2},
                          {"cores",  1, NULL, 3},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:f:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'b':
                // BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 10000) {
                    *bufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -b, internal buf size >= 10kB\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'r':
                // UDP RECEIVE BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 220000) {
                    *recvBufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -r, UDP recv buf size >= 220kB\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(listenAddr, optarg);
                break;

            case 'f':
                // ET file
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "ET file name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(filename, optarg);
                break;

            case 1:
                // Tick prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *tickPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -tpre, tpre >= 1\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 2:
                // use IP version 6
                fprintf(stderr, "SETTING TO IP version 6\n");
                *useIPv6 = true;
                break;

            case 3:
                // Cores to run on
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }


                {
                    // split into ints
                    std::string s = optarg;
                    std::string delimiter = ",";

                    size_t pos = 0;
                    std::string token;
                    char *endptr;
                    int index = 0;
                    bool oneMore = true;

                    while ((pos = s.find(delimiter)) != std::string::npos) {
                        //fprintf(stderr, "pos = %llu\n", pos);
                        token = s.substr(0, pos);
                        errno = 0;
                        cores[index] = (int) strtol(token.c_str(), &endptr, 0);

                        if ((token.c_str() - endptr) == 0) {
                            //fprintf(stderr, "two commas next to eachother\n");
                            oneMore = false;
                            break;
                        }
                        index++;
                        //std::cout << token << std::endl;
                        s.erase(0, pos + delimiter.length());
                        if (s.length() == 0) {
                            //fprintf(stderr, "break on zero len string\n");
                            oneMore = false;
                            break;
                        }
                    }

                    if (oneMore) {
                        errno = 0;
                        cores[index] = (int) strtol(s.c_str(), nullptr, 0);
                        if (errno == EINVAL || errno == ERANGE) {
                            fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n\n");
                            printHelp(argv[0]);
                            exit(-1);
                        }
                        index++;
                        //std::cout << s << std::endl;
                    }
                }
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
    et_sys_id id;
    uint16_t grpcServicePort;
    BackendStateServiceImpl *pGrpcService;
} threadStruct;


// Thread to monitor the ET system, run PID loop and report back to control plane
static void *pidThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);
    BackendStateServiceImpl *pGrpcService = targ->pGrpcService;
    et_sys_id id = targ->id;
    int status, numEvents, inputListCount, pidError = 5, fillPercent;
    size_t eventSize;
    status = et_system_getnumevents(id, &numEvents);
    status = et_system_geteventsize(id, &eventSize);

    int64_t totalT = 0, time;
    struct timespec t1, t2, firstT;

    pGrpcService->setName("ERSAP_backend");

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        // Delay 1 milliseconds between data points
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Get the number of available events (# sitting in Grandcentral's input list)
        status = et_station_getinputcount_rt(id, ET_GRANDCENTRAL, &inputListCount);

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);

        // Microseconds
        time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        if (time >= 1000000) {
            fillPercent = (numEvents-inputListCount)*100/numEvents;
            printf("Total cnt %d, GC in list cnt %d, %d%% filled\n",
                   numEvents, inputListCount, fillPercent);
            pGrpcService->setState(numEvents, (int32_t)eventSize, fillPercent, pidError);
            t1 = t2;
        }
    }

    return (nullptr);
}


// Thread to listen to requests from the control plane and respond
static void *grpcServerThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);
    BackendStateServiceImpl *pGrpcService = targ->pGrpcService;

    std::cout << "About to run GRPC server in its own thread on port " << targ->grpcServicePort << std::endl;
    pGrpcService->runServer(targ->grpcServicePort, pGrpcService);
    std::cout << "Should never print this message!!!" << std::endl;

    return (nullptr);
}



// Statistics
static volatile uint64_t totalBytes=0, totalPackets=0;
static volatile int cpu=-1;
static std::atomic<uint32_t> droppedPackets;
static std::atomic<uint32_t> droppedTicks;

// Thread to send to print out rates
static void *rateThread(void *arg) {

    uint64_t packetCount, byteCount;
    uint64_t prevTotalPackets, prevTotalBytes;
    uint64_t currTotalPackets, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    double pktRate, pktAvgRate, dataRate, dataAvgRate, totalRate, totalAvgRate;
    int64_t totalT = 0, time, droppedPkts, totalDroppedPkts = 0, droppedTiks, totalDroppedTiks = 0;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        prevTotalBytes   = totalBytes;
        prevTotalPackets = totalPackets;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalT = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        currTotalBytes   = totalBytes;
        currTotalPackets = totalPackets;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalPackets > 0) {
                skipFirst = false;
            }
            firstT = t1 = t2;
            totalT = totalBytes = totalPackets = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount   = currTotalBytes   - prevTotalBytes;
        packetCount = currTotalPackets - prevTotalPackets;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = totalPackets = 0;
            firstT = t1 = t2;
            continue;
        }

        // Dropped stuff rates
        droppedPkts = droppedPackets;
        droppedPackets.store(0);
        totalDroppedPkts += droppedPkts;

        droppedTiks = droppedTicks;
        droppedTicks.store(0);
        totalDroppedTiks += droppedTiks;

        pktRate = 1000000.0 * ((double) packetCount) / time;
        pktAvgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        if (packetCount == 0 && droppedPkts == 0) {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = 0?/everything? ", pktRate, pktAvgRate);
        }
        else {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = %" PRId64 " ", pktRate, pktAvgRate, droppedPkts);
        }
        printf(": Dropped Ticks = %" PRId64 ", total = %" PRId64 "\n", droppedTiks, totalDroppedTiks);

        // Actual Data rates (no header info)
        dataRate = ((double) byteCount) / time;
        dataAvgRate = ((double) currTotalBytes) / totalT;
        printf(" Data:     %3.4g MB/s,  %3.4g Avg, cpu %d, dropped pkts %" PRId64 ", total %" PRId64 "\n",
               dataRate, dataAvgRate, cpu, droppedPkts, totalDroppedPkts);

        totalRate = ((double) (byteCount + HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + HEADER_BYTES*currTotalPackets)) / totalT;
        printf(" Total:    %3.4g MB/s,  %3.4g Avg\n\n", totalRate, totalAvgRate);

        t1 = t2;
    }

    return (nullptr);
}





int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    // Set this to max expected data size
    int bufSize = 1020000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    uint16_t port = 7777;
    uint16_t serverPort = 8888;
    int cores[10];
    bool debug = false;
    bool useIPv6 = false;
    bool sendToEt = false;

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, cores, &port, &debug,
              &useIPv6, listeningAddr, filename);

    std::cerr << "Tick prescale = " << tickPrescale << "\n";

#ifdef __linux__

    if (cores[0] > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        if (debug) {
            for (int i=0; i < 10; i++) {
                     std::cerr << "core[" << i << "] = " << cores[i] << "\n";
            }
        }

        for (int i=0; i < 10; i++) {
            if (cores[i] >= 0) {
                std::cerr << "Run reassembly thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
            }
            else {
                break;
            }
        }
        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << std::endl;
        }
    }

#endif


#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

    // Do we connect to a TCP server and send the data there?
    if (strlen(filename) > 0) {
        sendToEt = true;
    }

    if (useIPv6) {
        struct sockaddr_in6 serverAddr6{};

        // Create IPv6 UDP socket
        if ((udpSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
            perror("creating IPv6 client socket");
            return -1;
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);
        if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

        // Configure settings in address struct
        // Clear it out
        memset(&serverAddr6, 0, sizeof(serverAddr6));
        // it is an INET address
        serverAddr6.sin6_family = AF_INET6;
        // the port we are going to receiver from, in network byte order
        serverAddr6.sin6_port = htons(port);
        if (strlen(listeningAddr) > 0) {
            inet_pton(AF_INET6, listeningAddr, &serverAddr6.sin6_addr);
        }
        else {
            serverAddr6.sin6_addr = in6addr_any;
        }

        // Bind socket with address struct
        int err = bind(udpSocket, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
        if (err != 0) {
            if (debug) fprintf(stderr, "bind socket error\n");
            return -1;
        }
    }
    else {
        // Create UDP socket
        if ((udpSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("creating IPv4 client socket");
            return -1;
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);
        fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

        // Configure settings in address struct
        struct sockaddr_in serverAddr{};
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        if (strlen(listeningAddr) > 0) {
            serverAddr.sin_addr.s_addr = inet_addr(listeningAddr);
        }
        else {
            serverAddr.sin_addr.s_addr = INADDR_ANY;
        }
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

        // Bind socket with address struct
        int err = bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
        if (err != 0) {
            fprintf(stderr, "bind socket error\n");
            return -1;
        }
    }

    pthread_t thd;
    int status = pthread_create(&thd, NULL, rateThread, (void *) nullptr);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating RATE thread\n\n");
        return -1;
    }

     // If bufSize gets too big, it exceeds stack limits, so lets malloc it!
    char *dataBuf = (char *) malloc(bufSize);
    if (dataBuf == NULL) {
        fprintf(stderr, "cannot allocate internal buffer memory of %d bytes\n", bufSize);
        return -1;
    }

    fprintf(stderr, "Internal buffer size = %d bytes\n", bufSize);

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;

    // Track cpu by calling sched_getcpu roughly once per sec
    int cpuLoops = 50000;
    int loopCount = cpuLoops;

    // Statistics
    std::shared_ptr<packetRecvStats> stats = std::make_shared<packetRecvStats>();
    droppedTicks.store(0);
    droppedPackets.store(0);

    /////////////////
    /// ET  Stuff ///
    /////////////////
    et_sys_id id;
    et_fifo_id fid;
    et_fifo_entry *entry;
    et_openconfig openconfig;
    et_event **pe;

    // This ET fifo is only 1 event wide
    int idCount = 1;
    int ids[idCount];
    int debugLevel = ET_DEBUG_INFO;
    char host[256];
    ////////////////////////////
    /// Control Plane  Stuff ///
    ////////////////////////////
    bool reportToCP = true && sendToEt;
    BackendStateServiceImpl service;
    BackendStateServiceImpl *pGrpcService = &service;


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


        // Start thread to do rate printout
        threadStruct *targ = (threadStruct *)malloc(sizeof(threadStruct));
        if (targ == nullptr) {
            fprintf(stderr, "out of mem\n");
            return -1;
        }
        std::memset(targ, 0, sizeof(threadStruct));
        targ->id = id;
        targ->grpcServicePort = 50051;
        targ->pGrpcService = pGrpcService;

        pthread_t thd1;
        status = pthread_create(&thd1, NULL, pidThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating PID thread ********\n\n");
            return -1;
        }

        if (reportToCP) {
            // Start up grpc server listening thread
            threadStruct *targ2 = (threadStruct *)calloc(1, sizeof(threadStruct));
            if (targ2 == nullptr) {
                fprintf(stderr, "out of mem\n");
                return -1;
            }

            pthread_t thd2;
            status = pthread_create(&thd2, NULL, grpcServerThread, (void *) targ);
            if (status != 0) {
                fprintf(stderr, "\n ******* error creating GRPC server thread ********\n\n");
                return -1;
            }
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
                fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
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



