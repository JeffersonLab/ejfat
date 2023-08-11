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
 * Receive generated data sent by clasBlaster.c program.
 * Take the reassembled buffers and send to an ET system so ERSAP can grab them.
 * Interact with an EJFAT control plane (register, send info, etc).
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
#include <vector>

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
#include "lb_cplane.h"


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


static bool haveRecvAddr = false;
static bool haveEtName = false;


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        -f <ET file>",
            "        [-h] [-v] [-ip6]",
            "        [-p <data receiving port, 17750 default>]",
            "        [-a <data receiving address>]",
            "        [-token <authentication token (for CP registration, default token_<time>)>]",
            "        [-range <data receiving port range, entropy of sender>]\n\n",

            "        [-sfile <file name for stats>]",
            "        [-stime <stat sample millisec (t >= 1 msec, default = 1)]\n\n",

            "        [-gaddr <CP IP address (default = none & no CP comm)>]",
            "        [-gport <CP port (default 18347)>]",
            "        [-gname <name of this backend (default myBackEnd)>]\n\n",

            "        [-kp <PID proportional constant, default 0.8>]",
            "        [-ki <PID integral constant, default 0.02>]",
            "        [-kd <PID differential constant, default 0.001>]\n",

            "        [-count <# of most recent fill values averaged, default 1000>]",
            "        [-rtime <millisec for reporting fill to CP, default 1000>]\n\n",

            "        [-s <PID fifo set point (default 0)>]",
            "        [-b <internal buffer byte size (default 150kB)>]",
            "        [-r <UDP receive buffer byte size (default 25MB)>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with clasBlaster and send data to an ET system.\n");
    fprintf(stderr, "        It interacts with either a real or simulated control plane.\n");
    fprintf(stderr, "        Specifying a stat file name will create 1 files with:\n");
    fprintf(stderr, "          1) String header naming columns,\n");
    fprintf(stderr, "          2) Each row: time (usec), instant fill, avg fill, avg fill %%, pid err\n");
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
 * @param setPt         filled with the set point of PID loop used with fifo fill level.
 * @param port          filled with UDP receiving data port to listen on.
 * @param cpPort        filled wit hmain control plane port.
 * @param range         filled with range of ports in powers of 2 (entropy).
 * @param fillCount     filled with # of fill level measurements to average together before sending.
 * @param reportTime    filled with millisec between reports to CP.
 * @param stime         filled with time in millisec to sample stats.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param listenAddr    filled with IP address to listen on for LB data.
 * @param cpAddr        filled with control plane IP address.
 * @param etFilename    filled with name of ET file in which to write data.
 * @param beName        filled with name of this backend CP client.
 * @param statBaseName  filled with base name of files used to store 100 sec of ET fill level data.
 * @param kp            filled with PID proportional constant.
 * @param ki            filled with PID integral constant.
 * @param kd            filled with PID differential constant.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      int *cores, float *setPt,
                      uint16_t* port, uint16_t* cpPort, int *range,
                      uint32_t *fillCount, uint32_t *reportTime, uint32_t *stime,
                      bool *debug, bool *useIPv6,
                      char *listenAddr, char *cpAddr, char *cpToken,
                      char *etFilename, char *beName, std::string &statFileName,
                      float *kp, float *ki, float *kd) {

    int c, i_tmp;
    bool help = false;
    float sp = 0.;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",      1, nullptr, 1},
                          {"ip6",       0, nullptr, 2},
                          {"cores",     1, nullptr, 3},
                          {"gaddr",     1, nullptr, 4},
                          {"gport",     1, nullptr, 5},
                          {"gname",     1, nullptr, 6},
                          {"token",     1, nullptr, 7},
                          {"range",     1, nullptr, 8},
                          {"sfile",     1, nullptr, 9},
                          {"stime",     1, nullptr, 10},
                          {"kp",        1, nullptr, 11},
                          {"ki",        1, nullptr, 12},
                          {"kd",        1, nullptr, 13},
                          {"count",     1, nullptr, 14},
                          {"rtime",     1, nullptr, 15},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:f:s:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // Data PORT
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

            case 5:
                // control plane PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *cpPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -gport, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 8:
                // LB port range
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0 && i_tmp <= 14) {
                    *range = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -range, 0 <= port <= 14\n\n");
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

            case 7:
                // backend authentication token with control plane
                if (strlen(optarg) > 32 || strlen(optarg) < 1) {
                    fprintf(stderr, "authentication token length must be > 1 and < 33\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cpToken, optarg);
                break;

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                haveRecvAddr = true;
                strcpy(listenAddr, optarg);
                break;

            case 4:
                // control plane IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "grpc server IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cpAddr, optarg);
                break;

            case 6:
                // this client name to control plane
                if (strlen(optarg) > 32 || strlen(optarg) < 1) {
                    fprintf(stderr, "backend client name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(beName, optarg);
                break;

            case 'f':
                // ET file
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "ET file name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                haveEtName = true;
                strcpy(etFilename, optarg);
                break;

            case 9:
                // statistics file name
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "stat file name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                statFileName = optarg;
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

            case 10:
                // Set the stat sample time in msec
                i_tmp = (int) strtol(optarg, nullptr, 0);
                i_tmp =  i_tmp < 1 ? 1 : i_tmp;
                *stime = i_tmp;
                break;

            case 11:
                // Set the Kp PID loop parameter
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -kp\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *kp = sp;
                break;

            case 12:
                // Set the Ki PID loop parameter
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -ki\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *ki = sp;
                break;

            case 13:
                // Set the Kd PID loop parameter
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -kd\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *kd = sp;
                break;

            case 14:
                // count = # of fill level values averaged together before reporting
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *fillCount = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -count, must be > 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 15:
                // reporting interval in millisec
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *reportTime = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -rtime, must be >= 1 ms\n\n");
                    printHelp(argv[0]);
                    exit(-1);
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

    if (!haveEtName || !haveRecvAddr) {
        fprintf(stderr, "Need to define ET system and receiving addr on cmd line\n\n");
        printHelp(argv[0]);
        exit(2);
    }
}


/**
 * Convert timespec to unsigned long in microseconds.
 * @param time time to convert.
 * @return microseconds.
 */
static uint64_t timespecToMicroSec(timespec *time) {
    return 1000000UL * time->tv_sec + time->tv_nsec/1000UL;
}


// structure for passing args to thread
typedef struct threadStruct_t {
    et_sys_id etId;
    et_fifo_id fid;

    uint16_t cpServerPort;
    std::string cpServerIpAddr;

    uint16_t dataPort;
    std::string dataIpAddr;
    int dataPortRange;

    std::string myName;
    std::string token;
    float setPoint;
    bool report;

    // fill stats
    bool keepFillStats;
    std::string statFile;

    float Kp;
    float Ki;
    float Kd;

    uint32_t fcount;
    uint32_t reportTime;
    uint32_t sampleTime;

} threadStruct;


// Thread to monitor the ET system, run PID loop and report back to control plane
static void *pidThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);

    et_sys_id etId = targ->etId;
    et_fifo_id fid = targ->fid;
    bool reportToCp = targ->report;

    int status, numEvents, usedFifoEntries = 0;
    size_t eventSize;
    status = et_system_getnumevents(etId, &numEvents);
    status = et_system_geteventsize(etId, &eventSize);
    // Max # of fifo entries available to consumer
    int fifoCapacity = et_fifo_getEntryCount(fid);
    if (fifoCapacity < 0) {
        // error
        // ET system error
        printf("Bad ET fifo id, exit\n");
        exit(1);
    }

    float pidError;
    float pidSetPoint = targ->setPoint;
    const float Kp = targ->Kp;
    const float Ki = targ->Ki;
    const float Kd = targ->Kd;

    // # of fill level to average together
    uint32_t fcount = targ->fcount;
    // time period in millisec for reporting to CP
    uint32_t reportTime = targ->reportTime;

    // Vectors to store statistics values until reporting time, then written to file
    std::vector<uint64_t> timeVec;        // epoch time in millisec
    std::vector<float> percentVec;     // % of fifo filled (0-1)
    std::vector<float> avgVec;         // running avg fill level
    std::vector<float> instVec;        // instantaneous fill level
    std::vector<float> pidErrVec;      // PID loop error


    FILE *fp = nullptr;
    bool keepFillStats = targ->keepFillStats;
    if (keepFillStats) {
        // Open file and write header
        FILE *fp = fopen (targ->statFile.c_str(), "w");
        if (fp == nullptr) {
            printf("Failed to open statistics file %s, so don't keep any\n", targ->statFile.c_str());
            keepFillStats = false;
        }
        else {
            fprintf(fp, "time(usec), instant_fill, avg_fill, percent_avg_fill, pidErr\n");
        }
    }


//    /**
//     * Constructor.
//     * @param cIp          grpc IP address of control plane (dotted decimal format).
//     * @param cPort        grpc port of control plane.
//     * @param bIp          data-receiving IP address of this backend client.
//     * @param bPort        data-receiving port of this backend client.
//     * @param bPortRange   range of data-receiving ports for this backend client.
//     * @param cliName      name of this backend.
//     * @param bufferSize   byte size of each buffer (fifo entry) in this backend.
//     * @param bufferCount  number of buffers in fifo.
//     * @param setPoint     PID loop set point (% of fifo).
//     *
//     */
//    LbControlPlaneClient::LbControlPlaneClient(
//            const std::string& cIP, uint16_t cPort,
//            const std::string& bIP, uint16_t bPort,
//            PortRange bPortRange, const std::string& cliName,
//            uint32_t bufferSize, uint32_t bufferCount, float setPoint)  {
//    }

    // convert integer range in PortRange enum
    auto range = PortRange(targ->dataPortRange);

    LbControlPlaneClient client(targ->cpServerIpAddr, targ->cpServerPort,
                                targ->dataIpAddr, targ->dataPort, range,
                                targ->myName, targ->token,
                                (int32_t)eventSize, numEvents, targ->setPoint);

    // Register this client with the grpc server &
    // wait for server to send session token in return.
    // Token stored internally in client.
    int32_t err = client.Register();
    if (err == 1) {
        printf("GRPC client %s communication error with server when registering, exit\n", targ->myName.c_str());
        exit(1);
    }


    // Add stuff to prevent anti-aliasing.
    // If sampling fifo level every millisec but that level is changing much more quickly,
    // the sent value will NOT be an accurate representation. It will include a lot of noise.
    // To prevent this,
    // keep a running average of the fill %, so its reported value is a more accurate portrayal
    // of what's really going on. In this case a running avg is taken over the reporting time.

    float deltaT; // sec
    int sampleMilliSec = targ->sampleTime; // Sample data every x millisec
    // # of loops (samples) to comprise one reporting period =
    int loopMax   = reportTime / sampleMilliSec; // remember, report time is also in millisec
    int loopCount = loopMax;    // use to track # loops made

    float runningFillTotal = 0., fillAvg;
    int fillValues[fcount];
    memset(fillValues, 0, fcount*sizeof(float));

    // Keep circulating thru array. Highest index is fcount - 1.
    // The first time thru, we don't want to over-weight with (fcount - 1) zero entries.
    // So we read fcount entries first, before we start keeping stats.
    float prevFill, curFill, fillPercent;
    bool startingUp = true;
    int fillIndex = 0, firstLoopCounter = 1;

    // time stuff
    struct timespec t1, t2;
    int64_t timeDiff;
    uint64_t absTime, prevAbsTime;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    prevAbsTime = 1000L*(t1.tv_sec) + (t1.tv_nsec)/1000000L;


    while (true) {

        // Delay between data points
        std::this_thread::sleep_for(std::chrono::milliseconds(sampleMilliSec));

        // Read current fifo fill level
        curFill = static_cast<float>(et_fifo_getFillLevel(fid));
        if (curFill < 0) {
            // ET system error
            printf("ET closed or communication error, exit\n");
            exit(1);
        }

        // Previous value at this index
        prevFill = fillValues[fillIndex];
        // Store current val at this index
        fillValues[fillIndex++] = curFill;
        // Add current val and remove previous val at this index from the running total.
        // That way we have added loopMax number of most recent entries at ony one time.
        runningFillTotal += curFill - prevFill;
        // Find index for the next round
        fillIndex = (fillIndex == fcount) ? 0 : fillIndex;

        if (startingUp) {
            if (firstLoopCounter++ >= fcount) {
                startingUp = false;
            }
            else {
                if (firstLoopCounter == fcount) {
                    // Start the clock NOW
                    clock_gettime(CLOCK_MONOTONIC, &t1);
                }
                // Don't start sending data or recording values to be written
                // until the startup time (fcount loops) is over.
                continue;
            }
            fillAvg = runningFillTotal / static_cast<float>(fcount);
        }
        else {
            fillAvg = runningFillTotal / static_cast<float>(fcount);
        }

        fillPercent = fillAvg / static_cast<float>(fifoCapacity);

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        // time diff in microsec
        timeDiff = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        // convert to sec
        deltaT = static_cast<float>(timeDiff)/1000000.F;
        // Get the current epoch time in millisec
        absTime = 1000L*t2.tv_sec + t2.tv_nsec/1000000L;
        t1 = t2;


        // PID error, to make this more accurate make a time measurement for deltaT
        pidError = pid<float>(pidSetPoint, fillPercent, deltaT, Kp, Ki, Kd);


        // Store stats
        if (keepFillStats) {
            timeVec.push_back(absTime);
            percentVec.push_back(fillPercent);
            avgVec.push_back(fillAvg);
            instVec.push_back(curFill);
            pidErrVec.push_back(pidError);
        }


        // Every "loopMax" loops
        if (--loopCount <= 0) {

            if (reportToCp) {
                // Update the changing variables
                client.update(fillPercent, pidError);

                // Send to server
                err = client.SendState();
                if (err == 1) {
                    printf("GRPC client %s communication error with server during sending of data, exit\n",
                           targ->myName.c_str());
                    exit(1);
                }
            }


            // Write out stats
            if (keepFillStats) {
                for (int i=0; i < timeVec.size(); i++) {
                    fprintf(fp, "%" PRIu64 " %f %f %f %f\n", timeVec[i], instVec[i], avgVec[i], percentVec[i], pidErrVec[i]);
                }
                fflush(fp);

                timeVec.clear();
                percentVec.clear();
                avgVec.clear();
                instVec.clear();
                pidErrVec.clear();
            }

            if (absTime - prevAbsTime >= 4000) {
                prevAbsTime = absTime;
                printf("     Fifo level %f, %f%%, avg %f, pid err %f\n\n", curFill, fillPercent, fillAvg, pidError);
            }

            loopCount = loopMax;
        }
    }

    // Unregister this client with the grpc server
    err = client.Deregister();
    if (err == 1) {
        printf("GRPC client %s communication error with server when unregistering, exit\n", targ->myName.c_str());
    }
    exit(1);

    return (nullptr);
}



// Statistics
static volatile uint64_t totalBytes=0, totalPackets=0, totalEvents=0;
static volatile int cpu=-1;
static std::atomic<uint32_t> droppedPackets {0};
static std::atomic<uint32_t> droppedTicks {0};

// Thread to send to print out rates
static void *rateThread(void *arg) {

    uint64_t packetCount, byteCount, eventCount;
    uint64_t prevTotalPackets, prevTotalBytes, prevTotalEvents;
    uint64_t currTotalPackets, currTotalBytes, currTotalEvents;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    double pktRate, pktAvgRate, dataRate, dataAvgRate, totalRate, totalAvgRate, evRate, avgEvRate;
    int64_t totalT = 0, time, droppedPkts, totalDroppedPkts = 0, droppedTiks, totalDroppedTiks = 0;
    uint64_t absTime;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        prevTotalBytes   = totalBytes;
        prevTotalPackets = totalPackets;
        prevTotalEvents  = totalEvents;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        // Epoch time in milliseconds
        absTime = 1000L*(t2.tv_sec) + (t2.tv_nsec)/1000000L;
        // time diff in microseconds
        time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalT = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        currTotalBytes   = totalBytes;
        currTotalPackets = totalPackets;
        currTotalEvents  = totalEvents;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalPackets > 0) {
                skipFirst = false;
            }
            firstT = t1 = t2;
            totalT = totalBytes = totalPackets = totalEvents = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount   = currTotalBytes   - prevTotalBytes;
        packetCount = currTotalPackets - prevTotalPackets;
        eventCount  = currTotalEvents  - prevTotalEvents;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = totalPackets = totalEvents = 0;
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
        printf("Packets:       %3.4g Hz,    %3.4g Avg, time: diff = %" PRId64 " usec, abs = %" PRIu64 " epoch msec",
               pktRate, pktAvgRate, time, absTime);
        // Tack on cpu info
        if (cpu > -1) {
            printf(", cpu = %d\n", cpu);
        }
        else {
            printf("\n");
        }

        // Data rates (with NO header info)
        dataRate = ((double) byteCount) / time;
        dataAvgRate = ((double) currTotalBytes) / totalT;
        // Data rates (with RE header info)
        totalRate = ((double) (byteCount + HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + HEADER_BYTES*currTotalPackets)) / totalT;
        printf("Data (+hdrs):  %3.4g (%3.4g) MB/s,  %3.4g (%3.4g) Avg\n", dataRate, totalRate, dataAvgRate, totalAvgRate);

        // Event rates
        evRate = 1000000.0 * ((double) eventCount) / time;
        avgEvRate = 1000000.0 * ((double) currTotalEvents) / totalT;
        printf("Events:        %3.4g Hz,  %3.4g Avg, total %" PRIu64 "\n", evRate, avgEvRate, totalEvents);

        // Drop info
        printf("Dropped: evts: %" PRId64 ", %" PRId64 " total, pkts: %" PRId64 ", %" PRId64 " total\n\n",
               droppedTiks, totalDroppedTiks, droppedPkts, totalDroppedPkts);

        t1 = t2;
    }

    return (nullptr);
}





int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    int range;
    int cores[10];

    // Set some defaults
    int bufSize = 150000;
    int recvBufSize = 25000000;
    int tickPrescale = 1;
    float cpSetPoint = 0.f;
    uint16_t cpPort = 18347;
    uint16_t port = 17750;

    bool debug = false;
    bool useIPv6 = false;
    bool sendToEt = false;
    bool keepLevelStats = false;

    // PID loop variables
    float Kp = 0.52;
    float Ki = 0.005;
    float Kd = 0.000;

    // # of fill values to average when reporting to grpc
    uint32_t fcount = 1000;
    // time period in millisec for reporting to CP
    uint32_t reportTime = 1000;
    // stat sampling time in msec
    uint32_t stime = 1;


    std::string stat_file_name;

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);

    char et_filename[101];
    memset(et_filename, 0, 101);

    char cpAddr[16];
    memset(cpAddr, 0, 16);

    char beName[33];
    memset(beName, 0, 33);

    char cpToken[33];
    memset(cpToken, 0, 33);

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv,
              &bufSize, &recvBufSize, &tickPrescale,
              cores, &cpSetPoint,
              &port, &cpPort, &range,
              &fcount, &reportTime, &stime,
              &debug, &useIPv6,
              listeningAddr, cpAddr, cpToken,
              et_filename, beName, stat_file_name,
              &Kp, &Ki, &Kd);

    std::cerr << "Tick prescale = " << tickPrescale << "\n";

    // Do we report to control plane?
    bool reportToCP = true;
    if (strlen(cpAddr) < 1) {
        reportToCP = false;
    }


    // Find a few bits of local time to set default names if necessary
    time_t localT = time(nullptr) & 0xffff;

    // Make sure this backend has a name
    if (strlen(beName) < 1) {
        std::string name = "backend_" + std::to_string(localT);
        std::strcpy(beName, name.c_str());
    }

    // Make sure we have a token
    if (strlen(cpToken) < 1) {
        std::string name = "token_" + std::to_string(localT);
        std::strcpy(cpToken, name.c_str());
    }

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
    // By default, set recv buf size to 7.4 MB which is the highest
    // it wants to go before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

    // Do we connect to a TCP server and send the data there?
    if (strlen(et_filename) > 0) {
        sendToEt = true;
    }

    // Do we write out ET fifo fill level data files?
    if (stat_file_name.length() > 0) {
        keepLevelStats = true;
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

    // Start thread to printout incoming data rate
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
    //LoadBalancerServiceImpl service;
    //LoadBalancerServiceImpl *pGrpcService = &service;


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
        if (et_open(&id, et_filename, openconfig) != ET_OK) {
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
        targ->fid = fid;
        targ->cpServerPort = cpPort;
        targ->cpServerIpAddr = cpAddr;

        targ->dataPort = port;
        targ->dataIpAddr = listeningAddr;
        targ->dataPortRange = range;

        targ->myName = beName;
        targ->token = cpToken;
        targ->setPoint = cpSetPoint;
        targ->report = reportToCP;

        targ->Kp = Kp;
        targ->Ki = Ki;
        targ->Kd = Kd;

        targ->fcount = fcount;
        targ->reportTime = reportTime;
        targ->sampleTime = stime;

        targ->keepFillStats = keepLevelStats;
        if (keepLevelStats) {
            targ->statFile = stat_file_name;
        }

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
                                             tickPrescale);
        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d), exit\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBufferNew (%ld), exit\n", nBytes);
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

//        diff = tick - prevTick;
//        if (diff != 0) {
//            fprintf(stderr, "Error in tick increment, %" PRIu64 "\n", diff);
//        }

        totalBytes   += nBytes;
        totalPackets += stats->acceptedPackets;
        totalEvents++;

        // atomic (ticks = buffers)
        droppedTicks   += stats->droppedBuffers;
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



