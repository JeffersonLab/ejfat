//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file
 * <p>
 * Receive generated data sent by one or more packetBlaster.c programs.
 * This assumes there is an EJFAT load balancer between this and the sending program.
 * Note that this program uses fast, blocking, but non-locking ring buffers.
 * This program is intended to be a full-blown implementation of how to handle multiple incoming
 * data sources and having it all sorted and assembled into separate buffers which can then be
 * accessed by the user.
 * </p>
 * <p>
 * In this program the "MP" stands for multi port.
 * One thread will be started for each data source. Each thd will read data from a
 * different data source on the same IP address but on a different port. These threads
 * do the buffer reassembly and make that buffer available downstream.
 * Depending on command line parameters, additional threads can be started (one per source id) to
 * access the final buffers. Or even just one thread to access buffers from all sources.
 * Maximum of 16 data sources.
 * The ports used are just the base port (17750 default) plus the source id.
 * </p>
 * <p>
 * There is also a thread to calculate and print rates.
 * </p>
 * <p>
 * This program is like packetBlasteeFullNewMP.cc but with 2 improvements:<br>
 *   1) Like packetBlasterFullNewMP, it reads data in by means of N thds
 *   for each source which also reassembles. However, unlike that program,
 *   it doesn't use the complicated algorithm for dealing with every out-of-order
 *   and duplicate packet. It turns out that doing so uses so much compute time
 *   that at high input rates, it drops many more packets than the simple approach.
 *   That simple approach is just to call a function which returns the next
 *   buildable buffer. It only accounts for out-of-order within the boundaries of a
 *   single event.<br>
 *
 *   2) As part of keeping stats, it measures the latency of an event -
 *   that is the average time to reassemble in nanoseconds. Clock starts when the
 *   first packet of an event arrives and ends when the calling thread gets the full
 *   event. This number will only make sense if all incoming events are the same size.
 * </p>
 * <p>
 * Use packetBlaster.cc to produce events.
 * </p>
 */

#include <cstdlib>
#include <iostream>
#include <time.h>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>
#include <cstring>
#include <utility>
#include <limits>
#include <unordered_map>
#include <string.h>
#include <errno.h>
#include <random>

#include "BufferItem.h"
#include "PacketStoreItem.h"
#include "SupplyItem.h"
#include "Supplier.h"

#include "ByteBuffer.h"
#include "ByteOrder.h"

#include "ejfat.hpp"
#include "ejfat_assemble_ersap.hpp"

// GRPC stuff
#include "lb_cplane.h"


#ifdef __linux__
     #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif


using namespace ejfat;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------

// Max number of chars in a name
#define INPUT_LENGTH_MAX 256

// Max # of data input sources
#define MAX_SOURCES 16


template<class X>
X pid(                      // Proportional, Integrative, Derivative Controller
        const X& setPoint,  // Desired Operational Set Point
        const X& prcsVal,   // Measure Process Value
        const X& delta_t,   // Time delta between determination of last control value
        const X& Kp,        // Konstant for Proprtional Control
        const X& Ki,        // Konstant for Integrative Control
        const X& Kd,        // Konstant for Derivative Control
        const X& prev_err,  // previous error
        const X& prev_err_t // # of microseconds earlier that previous error was recorded
)
{
    static X integral_acc = 0;   // For Integral (Accumulated Error)
    X error = setPoint - prcsVal;
    integral_acc += error * delta_t;
    X derivative = (error - prev_err) * 1000000. / prev_err_t;
//    if (prev_err_t == 0 || prev_err_t != prev_err_t) {
//        derivative = 0;
//    }
    return Kp * error + Ki * integral_acc + Kd * derivative;  // control output
}



/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6] [-norestart] [-jointstats] [-direct]\n",

            "        -addr  <data receiving address to register w/ CP>",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <starting listening UDP port (increment for each source, 17750 default)>]\n",

            "        [-b <internal buffer byte size, 100kB default>]",
            "        [-r <UDP receive buffer byte size, system default>]\n",

            "        [-direct (sender bypasses LB, so no LB communication)]",
            "        [-uri  <URI containing info for sending to LB/CP (default "")>]",
            "        [-file <file with URI (default /tmp/ejfat_uri)>]",
            "        [-token <CP admin token (default udplbd_default_change_me)>]\n",

            "        [-minf <min factor for CP slot assignment (default 0>]",
            "        [-maxf <max factor for CP slot assignment (default 0)>]\n",

            "        [-qmean <report this as the mean Q fill level to CP (>= 0)>]",
            "        [-qdev  <std dev of Q fill level if -qmean defined (>= 0)>]\n",
            "        [-qcnt  <how many Q measurements to average over 1 sec (default 1k, must divide 1M evenly: 1,2,4,5,8,10, ... 1k max)>]\n",

            "        [-dump (no thd to get & merge buffers)]",
            "        [-lump (DISABLED at the moment -- 1 thd to get & merge buffers from all sources)]",
            "        [-ids <comma-separated list of incoming source ids>]\n",

            "        [-pinRead <starting core # for read thds>]",
            "        [-pinCnt <# of cores for read thd>]",
            "        [-pinBuf <starting core #, 1 for each buf collection thd>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]\n");

    fprintf(stderr, "        Normally connect to a CP. This can be done by specifying either -uri or -file.\n");
    fprintf(stderr, "          If neither specified, default is to find connection info in /tmp/ejat_uri.\n");
    fprintf(stderr, "          This client only reports 0%% fill and ready for data.\n\n");

    fprintf(stderr, "        To get data directly from a sender (packetBlaster -direct <addr:port>)");
    fprintf(stderr, "          and not interact with the CP, use the -direct cmd line option.\n\n");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with packetBlaster.\n");
    fprintf(stderr, "        It can receive from multiple data sources simultaneously.\n\n");

    fprintf(stderr, "        By default, one thread started to \"process\" reassembled buffers for each source,\n");
    fprintf(stderr, "        unless -dump or -lump arg used.\n");
    fprintf(stderr, "        The -norestart flag means sources that restart and have new, lower event numbers,\n");
    fprintf(stderr, "        out of sync with other sources, will NOT be allowed\n\n");

    fprintf(stderr, "        If -qmean not set, reports q fill level as 100, error as 0., set pt = 5\n");
    fprintf(stderr, "        It -qmean is set, set max q as 10 x mean, set pt = mean/10.\n\n");

}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param bufSize       filled with buffer size.
 * @param recvBufSize   filled with UDP receive buffer size.
 * @param tickPrescale  expected increase in tick with each incoming buffer.
 * @param core          starting core id on which to run pkt reading thread.
 * @param coreCnt       number of cores on which to run pkt reading thread.
 * @param coreBuf       starting core id on which to run buf assembly threads.
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param dump          don't have a thd which gets all buffers (for possible merging).
 * @param lump          add 1 thd to get & merge buffers from all sources.
 * @param noRestart     exit program if sender restarts.
 * @param jointStats    display stats of all sources joined together.
 * @param direct        get data directly from sender (not thru LB) and do NOT talk to CP.
 * @param listenAddr    IP address to listen on.
 * @param dataAddr      IP address to send to CP as data destination addr for this program.
 * @param uri           URI containing LB/CP connection info.
 * @param file          name of file in which to read URI.
 * @param token         CP admin token.
 * @param ids           vector to be filled with data source id numbers.
 * @param minFactor     factor for setting min # of slot assignments.
 * @param maxFactor     factor for setting max # of slot assignments.
 * @param qMean         set a fake Q fill level to report to CP (0 - 1).
 * @param qDev          set a std. dev. of Q level if fake Q fill level set (0 - .5).
 * @param qCnt          number of Q measurements to average over 1 sec (must divide 1M evenly: 1,2,4,10,200, ...).
 */
static void parseArgs(int argc, char **argv,
                      uint32_t* bufSize, int *recvBufSize, int *tickPrescale,
                      int *core, int *coreCnt, int *coreBuf,
                      uint16_t* port,
                      bool *debug, bool *useIPv6,
                      bool *dump, bool *lump, bool *noRestart,
                      bool *jointStats, bool *direct,
                      char *listenAddr, char *dataAddr,
                      char *uri, char *file, char *token,
                      std::vector<int>& ids,
                      float *minFactor, float *maxFactor,
                      float *qMean, float *qDev, int *qCnt) {

    int c, i_tmp;
    bool help = false;
    float sp = 0.;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",        1, nullptr, 1},
                          {"ip6",         0, nullptr, 2},
                          {"pinRead",     1, nullptr, 3},
                          {"pinBuf",      1, nullptr, 4},
                          {"ids",         1, nullptr, 5},

                          {"dump",        0, nullptr, 7},
                          {"lump",        0, nullptr, 8},
                          {"pinCnt",      1, nullptr, 9},
                          {"norestart",   0, nullptr, 10},
                          {"jointstats",  0, nullptr, 11},

                    // Control Plane

                          {"addr",        1, nullptr, 12},
                          {"uri",         1, nullptr, 13},
                          {"file",        1, nullptr, 14},
                          {"token",       1, nullptr, 15},

                          {"minf",        1, nullptr, 16},
                          {"maxf",        1, nullptr, 17},

                          {"direct",      0, nullptr, 18},

                          {"qmean",       1, nullptr, 19},
                          {"qdev",        1, nullptr, 20},
                          {"qcnt",        1, nullptr, 21},

                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65536) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p (%d), 1023 < port < 65536\n", i_tmp);
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
                    fprintf(stderr, "Invalid argument to -b, internal buf size >= 10kB\n");
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
                    fprintf(stderr, "Invalid argument to -r, UDP recv buf size >= 220kB\n");
                    exit(-1);
                }
                break;

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n");
                    exit(-1);
                }
                strcpy(listenAddr, optarg);
                break;

            case 1:
                // Tick prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *tickPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -tpre, tpre >= 1\n");
                    exit(-1);
                }
                break;

            case 2:
                // use IP version 6
                *useIPv6 = true;
                break;

            case 3:
                // Cores to run on for packet reading thds
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *core = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -pinRead, need starting core #\n");
                    exit(-1);
                }

                break;

            case 4:
                // Cores to run on for buf assembly thds
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *coreBuf = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -pinBuf, need starting core #\n");
                    exit(-1);
                }

                break;

            case 5:
                // Incoming source ids
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ids\n");
                    exit(-1);
                }

                {
                    ids.clear();
                    std::string s = optarg;
                    std::stringstream ss(s);
                    std::string token;

                    while (std::getline(ss, token, ',')) {
                        try {
                            int value = std::stoi(token);
                            ids.push_back(value);
                        }
                        catch (const std::exception& e) {
                            fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ints\n");
                            exit(-1);
                        }
                    }
                }

                break;

            case 9:
                // NUmber of cores to run on for packet reading thd
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *coreCnt = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -pinCnt, need # of read cores (min 1)\n");
                    exit(-1);
                }

                break;

            case 12:
                // data receiving IP ADDRESS to report to CP
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "data receiving IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(dataAddr, optarg);
                break;

            case 13:
                // URI
                if (strlen(optarg) >= 255) {
                    fprintf(stderr, "Invalid argument to -uri, uri name is too long\n");
                    exit(-1);
                }
                strcpy(uri, optarg);
                break;

            case 14:
                // FILE NAME
                if (strlen(optarg) >= 255) {
                    fprintf(stderr, "Invalid argument to -file, file name is too long\n");
                    exit(-1);
                }
                strcpy(file, optarg);
                break;

            case 15:
                // ADMIN TOKEN
                if (strlen(optarg) >= 511) {
                    fprintf(stderr, "Invalid argument to -token, too long\n");
                    exit(-1);
                }
                strcpy(token, optarg);
                break;

            case 16:
                // Set the min-factor parameter
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -minf\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *minFactor = sp;
                break;

            case 17:
                // Set the max-factor parameter
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -maxf\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *maxFactor = sp;
                break;

            case 19:
                // Set the fake mean queue fill level
                try {
                    sp = (float) std::stof(optarg, nullptr);
                    if (sp < 0.) {
                        fprintf(stderr, "Invalid argument to -qmean, must be >= 0\n\n");
                        exit(-1);
                    }
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -qmean\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }

                *qMean = sp;
                break;

            case 20:
                // Set the std. dev. pf the fake mean queue fill level
                try {
                    sp = (float) std::stof(optarg, nullptr);
                    if (sp < 0.) {
                        fprintf(stderr, "Invalid argument to -qdev, must be >= 0\n\n");
                        exit(-1);
                    }
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -qdev\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *qDev = sp;
                break;

            case 21:
                // # Q fill level measurements to avg together (1 to 1000, divide evenly into 1M)
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1 && i_tmp <= 1000 && 1000000 % i_tmp == 0) {
                    *qCnt = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -qcnt, # measurements to avg >= 1 and <= 1000, evenly into 1M\n");
                    exit(-1);
                }
                break;

            case 7:
                // dump buffers without gathering into 1 thread
                *dump = true;
                break;

            case 8:
                // all buffers from all sources gathering into 1 thread
                *lump = true;
                break;

            case 10:
                // do NOT allow senders to restart their event number sequence
                *noRestart = true;
                break;

            case 11:
                // print stats of all sources joined together
                *jointStats = true;
                break;

            case 18:
                // get data directly from sender
                *direct = true;
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

    if (*dump && *lump) {
        fprintf(stderr, "Only one of -dump or -lump may be specified\n");
        exit(-1);
    }

    if ((*direct == false) && (strlen(dataAddr) < 7)) {
        fprintf(stderr, "Must specify -addr\n");
        exit(-1);
    }
}




// Statistics


// If a source dies and restarts, get a handle on when it actually restarted
// so the stats/rates
static volatile struct timespec restartTime;


typedef struct threadStruct_t {
    bool jointStats;
    int  sourceCount;
    std::vector<int> sourceIds;

    // key = source id, val = stats for src
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats;
    char filename[101];
} threadStruct;


// Thread to send to print out rates
static void *rateThread(void *arg) {

    int64_t byteCount, pktCount, bufCount, readTime;
    int64_t discardByteCount, discardPktCount, discardBufCount;

    // Parse arg
    threadStruct *targ = static_cast<threadStruct *>(arg);
    bool jointStats = targ->jointStats;
    int sourceCount = targ->sourceCount;
    std::vector<int> sourceIds = targ->sourceIds;

    // key = source id, val = stats
    // std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats;
    auto stats = targ->stats;
    // std::unordered_map<int, std::shared_ptr<packetRecvStats>> mapp;
    auto & mapp = (*(stats.get()));

    std::shared_ptr<packetRecvStats> pstats;


    int64_t prevTotalPkts[sourceCount];
    int64_t prevTotalBytes[sourceCount];
    int64_t prevBuiltBufs[sourceCount];

    int64_t prevDiscardPkts[sourceCount];
    int64_t prevDiscardBytes[sourceCount];
    int64_t prevDiscardBufs[sourceCount];

    int64_t prevDropPkts[sourceCount];
    int64_t prevDropBytes[sourceCount];
    int64_t prevDropBufs[sourceCount];


    int64_t prevTotalBuildTime[sourceCount];
    int64_t currTotalBuildTime[sourceCount];


    int64_t currTotalPkts[sourceCount];
    int64_t currTotalBytes[sourceCount];
    int64_t currBuiltBufs[sourceCount];

    memset(currBuiltBufs, 0, sizeof(currBuiltBufs));

    int64_t currDiscardPkts[sourceCount];
    int64_t currDiscardBytes[sourceCount];
    int64_t currDiscardBufs[sourceCount];

    int64_t currDropPkts[sourceCount];
    int64_t currDropBytes[sourceCount];
    int64_t currDropBufs[sourceCount];

    bool dataArrived[sourceCount];
    for (int i=0; i < sourceCount; i++) {
        dataArrived[i] = false;
    }

    int skippedFirst[sourceCount];
    for (int i=0; i < sourceCount; i++) {
        skippedFirst[i] = 0;
    }

    // Total time is different for each source since they may all start
    // sending their data at different times.
    int64_t totalMicroSecs[sourceCount];
    struct timespec tStart[sourceCount];


    // File writing stuff
    bool writeToFile = false;
    char *filename = targ->filename;
    FILE *fp;
    if (strlen(filename) > 0) {
        // open file
        writeToFile = true;
        fp = fopen (filename, "w");
        // validate file open for writing
        if (!fp) {
            fprintf(stderr, "file open failed: %s\n", strerror(errno));
            return (NULL);
        }

        // Write column headers
        fprintf(fp, "Sec,Source,PacketRate(kHz),DataRate(MB/s),Missing(bytes),TotalMissing(bytes)\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate, evRate, evAvgRate, latencyInstAvg, latencyTotalAvg;
    int64_t microSec;
    struct timespec tEnd, t1;
    bool rollOver = false, allSrcsSending = false;
    int sendingSrcCount = 0;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    restartTime.tv_sec  = t1.tv_sec;
    restartTime.tv_nsec = t1.tv_nsec;

    // We've got to handle "sourceCount" number of data sources - each with their own stats
    while (true) {

        // Loop for zeroing stats when first starting - for accurate rate calc
        for (int i=0; i < sourceCount; i++) {
            if (dataArrived[i] && (skippedFirst[i] == 1)) {
                // Data is now coming in. To get an accurate rate, start w/ all stats = 0
                int src = sourceIds[i];
fprintf(stderr, "rateThd: data now coming in for src %d\n", src);
                pstats = mapp[src];
                currTotalBytes[i]   = pstats->acceptedBytes    = 0;
                currTotalPkts[i]    = pstats->acceptedPackets  = 0;
                currBuiltBufs[i]    = pstats->builtBuffers     = 0;

                currDiscardPkts[i]  = pstats->discardedPackets = 0;
                currDiscardBytes[i] = pstats->discardedBytes   = 0;
                currDiscardBufs[i]  = pstats->discardedBuffers = 0;

                currDropBytes[i]    = pstats->droppedBytes     = 0;

                currTotalBuildTime[i] = pstats->readTime = 0;

                // Start the clock for this source
                clock_gettime(CLOCK_MONOTONIC, &tStart[i]);
//fprintf(stderr, "started clock for src %d\n", src);

                // From now on we skip this zeroing step
                skippedFirst[i]++;
            }
        }

        for (int i=0; i < sourceCount; i++) {
            prevTotalPkts[i]   = currTotalPkts[i];
            prevTotalBytes[i]  = currTotalBytes[i];
            prevBuiltBufs[i]   = currBuiltBufs[i];

            prevDiscardPkts[i]  = currDiscardPkts[i];
            prevDiscardBytes[i] = currDiscardBytes[i];
            prevDiscardBufs[i]  = currDiscardBufs[i];

            prevTotalBuildTime[i] = currTotalBuildTime[i];

//            prevDropBytes[i]  = currDropBytes[i];
//            prevDropPkts[i]   = currDropPkts[i];
//            prevDropBufs[i]   = currDropBufs[i];
        }

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &tEnd);

        // Time taken by last loop
        microSec = (1000000L * (tEnd.tv_sec - t1.tv_sec)) + ((tEnd.tv_nsec - t1.tv_nsec)/1000L);

        for (int i=0; i < sourceCount; i++) {
            // Total time - can be different for each source
            totalMicroSecs[i] = (1000000L * (tEnd.tv_sec - tStart[i].tv_sec)) + ((tEnd.tv_nsec - tStart[i].tv_nsec)/1000L);

            int src = sourceIds[i];
            pstats = mapp[src];

            // If the total # of built bufs goes down, it's only because the reassembly thread
            // believes the source was restarted and zeroed out the stats. So, in that case,
            // clear everything for that source and start over.
            bool restarted = currBuiltBufs[i] > pstats->builtBuffers;
            if (restarted) {
                fprintf(stderr, "\nLooks like data source %d restarted, so clear stuff\n", src);
                prevTotalPkts[i]    = 0;
                prevTotalBytes[i]   = 0;
                prevBuiltBufs[i]    = 0;

                prevDiscardPkts[i]  = 0;
                prevDiscardBytes[i] = 0;
                prevDiscardBufs[i]  = 0;

                prevTotalBuildTime[i] = 0;

                // The reassembly thread records when a source is restarted, use that time!
                totalMicroSecs[i] = (1000000L * (tEnd.tv_sec - restartTime.tv_sec)) + ((tEnd.tv_nsec - restartTime.tv_nsec)/1000L);
                tStart[i].tv_sec  = restartTime.tv_sec;
                tStart[i].tv_nsec = restartTime.tv_nsec;
            }

            currTotalBytes[i]   = pstats->acceptedBytes;
            currBuiltBufs[i]    = pstats->builtBuffers;
            currTotalPkts[i]    = pstats->acceptedPackets;

            currDiscardPkts[i]  = pstats->discardedPackets;
            currDiscardBytes[i] = pstats->discardedBytes;
            currDiscardBufs[i]  = pstats->discardedBuffers;

            currTotalBuildTime[i] = pstats->readTime;

//            currDropBytes[i]    = pstats->droppedBytes;

            if (currTotalBytes[i] < 0) {
                rollOver = true;
            }
        }

        // Don't start calculating stats until data has come in for a full cycle.
        // Keep track of when that starts.
        if (!allSrcsSending) {
            for (int i = 0; i < sourceCount; i++) {
                if (!dataArrived[i] && currTotalPkts[i] > 0) {
                    dataArrived[i] = true;
                    sendingSrcCount++;

                    if (sendingSrcCount == sourceCount) {
                        allSrcsSending = true;
                    }
                }
            }
        }


        // Start over tracking bytes and packets if #s roll over
        if (rollOver) {
            for (int i=0; i < sourceCount; i++) {
                int src = sourceIds[i];
                pstats = mapp[src];

                currTotalBytes[i]   = pstats->acceptedBytes    = 0;
                currTotalPkts[i]    = pstats->acceptedPackets  = 0;
                currBuiltBufs[i]    = pstats->builtBuffers     = 0;

                currDiscardPkts[i]  = pstats->discardedPackets = 0;
                currDiscardBytes[i] = pstats->discardedBytes   = 0;
                currDiscardBufs[i]  = pstats->discardedBuffers = 0;

                currTotalBuildTime[i] = pstats->readTime = 0;

                prevTotalPkts[i]    = 0;
                prevTotalBytes[i]   = 0;
                prevBuiltBufs[i]    = 0;

                prevDiscardPkts[i]  = 0;
                prevDiscardBytes[i] = 0;
                prevDiscardBufs[i]  = 0;

                prevTotalBuildTime[i] = 0;

                totalMicroSecs[i]   = microSec;
printf("Stats ROLLING OVER\n");


//                currDropBytes[i]    = pstats->droppedBytes     = 0;
            }
            t1 = tEnd;
            rollOver = false;
            continue;
        }

        // Do all stats together?
        if (jointStats && sourceCount > 1) {

            int activeSources = 0;
            byteCount = pktCount = bufCount = 0, readTime = 0;
            discardByteCount = discardPktCount = discardBufCount = 0;
            int64_t totalDiscardBufs = 0L, totalDiscardPkts = 0L, totalDiscardBytes = 0L;
            int64_t totalBytes = 0L, totalBuilt = 0L, totalMicro = 0L, totalPkts = 0L, totalReadTime = 0L, avgMicroSec;

            for (int i = 0; i < sourceCount; i++) {
                // Data not coming in yet from this source so do NO calcs
                if (!dataArrived[i]) continue;

                // Skip first stat cycle as the rate calculations will be off
                if (skippedFirst[i] < 1) {
                    skippedFirst[i]++;
                    continue;
                }

                activeSources++;

                // Use for instantaneous rates/values
                readTime  += currTotalBuildTime[i] - prevTotalBuildTime[i];
                byteCount += currTotalBytes[i] - prevTotalBytes[i];
                pktCount  += currTotalPkts[i]  - prevTotalPkts[i];
                bufCount  += currBuiltBufs[i]  - prevBuiltBufs[i];

                // Can't tell how many bufs & packets are completely dropped unless we know exactly what's coming in
                discardByteCount += currDiscardBytes[i] - prevDiscardBytes[i];
                discardPktCount  += currDiscardPkts[i]  - prevDiscardPkts[i];
                discardBufCount  += currDiscardBufs[i]  - prevDiscardBufs[i];

                totalReadTime += currTotalBuildTime[i];
                totalBytes += currTotalBytes[i];
                totalBuilt += currBuiltBufs[i];
                totalMicro += totalMicroSecs[i];
                totalPkts  += currTotalPkts[i];
                totalDiscardBufs  += currDiscardBufs[i];
                totalDiscardPkts  += currDiscardPkts[i];
                totalDiscardBytes += currDiscardBytes[i];
            }

            if (activeSources > 0) {
                avgMicroSec = totalMicro/activeSources;

                latencyTotalAvg = ((double) totalReadTime) / totalBuilt;
                printf("Latency:  %3.4g nanosec,   %3.4g Avg\n", (double)readTime/bufCount, latencyTotalAvg);

                printf("Latency:  ");
                for (int j=0; j < sourceCount; j++) {
                    int src = sourceIds[j];
                    double latencyAvg = ((double) currTotalBuildTime[j]) / currBuiltBufs[j];
                    printf("(%d) %3.4g, ", src, latencyAvg);
                }
                printf("\n");

                pktRate    = 1000000.0 * ((double) pktCount) / microSec;
                pktAvgRate = 1000000.0 * ((double) totalPkts) / avgMicroSec;

                printf("Packets:  %3.4g Hz,    %3.4g Avg\n", pktRate, pktAvgRate);

                // Actual Data rates (no header info)
                dataRate    = ((double) byteCount) / microSec;
                dataAvgRate = ((double) totalBytes) / avgMicroSec;

                printf("   Data:  %3.4g MB/s,  %3.4g Avg\n", dataRate, dataAvgRate);

                // Event rates
                evRate    = 1000000.0 * ((double) bufCount) / microSec;
                evAvgRate = 1000000.0 * ((double) totalBuilt) / avgMicroSec;
                printf(" Events:  %3.4g Hz,    %3.4g Avg, total %" PRId64 "\n",
                        evRate, evAvgRate, totalBuilt);

                printf("Discard:  %" PRId64 ", (%" PRId64 " total) evts,   pkts: %" PRId64 ", %" PRId64 " total\n\n",
                        discardBufCount, totalDiscardBufs, discardPktCount, totalDiscardPkts);

                // Sec,Source,PacketRate(kHz),DataRate(MB/s),Missing(bytes),TotalMissing(bytes)
                if (writeToFile) {
                    fprintf(fp, "%" PRId64 ",all,%d,%d,%" PRId64 ",%" PRId64 "\n\n",
                            avgMicroSec / 1000000,
                            (int) (pktRate / 1000), (int) (dataRate),
                            discardByteCount, totalDiscardBytes);
                    fflush(fp);
                }
            }
        }
        else {

            // Do individual stat printouts for each source
            for (int i = 0; i < sourceCount; i++) {
                // Data not coming in yet from this source so do NO calcs
                if (!dataArrived[i]) continue;

                // Skip first stat cycle as the rate calculations will be off
                if (skippedFirst[i] < 1) {
                    skippedFirst[i]++;
                    continue;
                }

                int src = sourceIds[i];
                pstats = mapp[src];

                // Use for instantaneous rates/values
                readTime  = currTotalBuildTime[i] - prevTotalBuildTime[i];
                byteCount = currTotalBytes[i] - prevTotalBytes[i];
                pktCount  = currTotalPkts[i]  - prevTotalPkts[i];
                bufCount  = currBuiltBufs[i]  - prevBuiltBufs[i];

                // Can't tell how many bufs & packets are completely dropped unless we know exactly what's coming in
                discardByteCount = currDiscardBytes[i] - prevDiscardBytes[i];
                discardPktCount  = currDiscardPkts[i]  - prevDiscardPkts[i];
                discardBufCount  = currDiscardBufs[i]  - prevDiscardBufs[i];

                latencyInstAvg  = ((double) readTime) / bufCount;
                latencyTotalAvg = ((double) currTotalBuildTime[i]) / currBuiltBufs[i];
                printf("%d Latency:  %3.4g nanosec,    %3.4g Avg\n", sourceIds[i], latencyInstAvg, latencyTotalAvg);

                pktRate = 1000000.0 * ((double) pktCount) / microSec;
                pktAvgRate = 1000000.0 * ((double) currTotalPkts[i]) / totalMicroSecs[i];

                printf("  Packets:  %3.4g Hz,    %3.4g Avg\n", pktRate, pktAvgRate);

                // Actual Data rates (no header info)
                dataRate = ((double) byteCount) / microSec;
                dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSecs[i];

                printf("     Data:  %3.4g MB/s,  %3.4g Avg, bufs %" PRId64 "\n",
                        dataRate, dataAvgRate, pstats->builtBuffers);

                // Event rates
                evRate = 1000000.0 * ((double) bufCount) / microSec;
                evAvgRate = 1000000.0 * ((double) currBuiltBufs[i]) / totalMicroSecs[i];
                printf("   Events:  %3.4g Hz,    %3.4g Avg, total %" PRIu64 "\n",
                        evRate, evAvgRate, currBuiltBufs[i]);

                printf("  Discard:    %" PRId64 ", (%" PRId64 " total) evts,   pkts: %" PRId64 ", %" PRId64 " total\n\n",
                        discardBufCount, currDiscardBufs[i], discardPktCount, currDiscardPkts[i]);

                // Sec,Source,PacketRate(kHz),DataRate(MB/s),Missing(bytes),TotalMissing(bytes)
                if (writeToFile) {
                    fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 "\n\n",
                            totalMicroSecs[i] / 1000000, src,
                            (int) (pktRate / 1000), (int) (dataRate),
                            discardByteCount, currDiscardBytes[i]);
                    fflush(fp);
                }
            }
        }

        t1 = tEnd;
        restartTime.tv_sec  = t1.tv_sec;
        restartTime.tv_nsec = t1.tv_nsec;
    }

    fclose(fp);
    return (nullptr);
}



// Arg to pass to buffer reassembly thread
typedef struct threadArg_t {
    /** One buffer supply for each reassembly thread. Used in threadReadBuffers thread. */
    std::shared_ptr<Supplier<BufferItem>> supplyMap;

    /** Byte size of buffers contained in supply. */
    int bufferSize;
    int mtu;
    int sourceCount;
    int pktConsumerId;
    int sourceId;
    int tickPrescale;
    int port;
    int recvBufSize;
    char *listeningAddr;

    bool debug;
    bool dump;
    bool noRestart;
    bool useIPv6;

    // shared ptr of stats
    std::shared_ptr<packetRecvStats> stats;

    int core;
    int coreCount;

} threadArg;



/**
 * <p>
 * Thread to assemble incoming packets into their original buffers.
 *
 * There is a fast-ring-buffer-based supply which contains empty buffers designed to hold built events.
 * An empty buffer is taken from the buffer supply. Then packets are read from the socket and
 * reconstructed into an event in that empty buffer.
 * When the event is fully constructed, the buffer is released back to the ring
 * which is then accessed by another thread which is looking for the built events downstream
 * (or it can simply be dumped straight away).
 * </p>
 * <p>
 * Reassembly of event is done by calling getCompletePacketizedBuffer which handles
 * out of order packets but only within one event. None of the fancy error recovery
 * of the threadAssemble thread is done. Ironically, even tho this thread uses less
 * completel error recovery, it's about 40% faster because of that and hence
 * drops many fewer event.
 * </p>
 * <p>
 * What happens if a data source dies and restarts?
 * This causes the tick/event-number sequence to restart. In order to avoid causing problems
 * calculating various stats, like rates, the "biggest tick" from a source is reset if 1000
 * packets with smaller ticks than the current largest.
 * </p>
 *
 *
 * @param arg   struct to be passed to thread.
 */
static void *threadAssembleFast(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int bufSize         = tArg->bufferSize;
    int id              = tArg->pktConsumerId;
    int sourceId        = tArg->sourceId;
    //std::cerr << "thd receiving sourceId = " << sourceId << std::endl;
    int port            = tArg->port;
    int recvBufSize     = tArg->recvBufSize;
    int tickPrescale    = tArg->tickPrescale;
    char *listeningAddr = tArg->listeningAddr;

    auto bufSupply = tArg->supplyMap;

    /** Byte size of buffers contained in supply. */

    bool dumpBufs  = tArg->dump;
    bool debug     = tArg->debug;
    bool noRestart = tArg->noRestart;
    bool useIPv6   = tArg->useIPv6;

    auto stats     = tArg->stats;
    bool takeStats = (stats != nullptr);

    // expected max size of packets
    int mtu = tArg->mtu;
    if (mtu < 1) {
        mtu = 9000;
    }
    else if (mtu < 1400) {
        mtu = 1400;
    }


    //---------------------------------------------------
    // Create socket to read data from source ID
    //---------------------------------------------------
    int udpSocket;

    if (useIPv6) {
        struct sockaddr_in6 serverAddr6{};

        // Create IPv6 UDP socket
        if ((udpSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
            perror("creating IPv6 client socket");
            exit(1);
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);

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
            exit(1);
        }
    }
    else {
        // Create UDP socket
        if ((udpSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("creating IPv4 client socket");
            exit(1);
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);

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
            exit(1);
        }
    }

    fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes, source id = %d\n",
            port, recvBufSize, sourceId);


#ifdef __linux__

    int core = tArg->core;
    if (core > -1) {
       cpu_set_t cpuset;
       CPU_ZERO(&cpuset);

       for (int i=0; i < tArg->coreCount; i++) {
            std::cerr << "Run assemble thd for source " << sourceId << " on core " << (core + i) << "\n";
            CPU_SET(core+i, &cpuset);
        }

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }

#endif


    // Need to figure out if source was killed and restarted.
    // If this is the case, then the tick # will be much smaller than the previous value.
    uint64_t tick = 0, prevTick, largestTick = 0;
    uint32_t offset, length;
    bool restarted = false, firstLoop = true;
    ssize_t nBytes;
    char *dataBuf;
    uint16_t dataId;

    std::shared_ptr<BufferItem>  bufItem, prevBufItem;
    std::shared_ptr<ByteBuffer>  buf;

    struct timespec end;
    int64_t now, nanoSecs;

    bool useLocalBuf = true;

    char eventBuf[bufSize];
    dataBuf = eventBuf;

//int loops = 100, loopCount = 0;

    while (true) {

        //-------------------------------------------------------------
        // Get reassembled buffer
        //-------------------------------------------------------------

        // With the load balancer, ticks can only increase. This new, largest tick
        // (or restarted tick) will not have an associated buffer yet, so create it,
        // store it, etc.

        if (!useLocalBuf) {
            bufItem = bufSupply->get();
            buf = bufItem->getBuffer();
            dataBuf = (char *) (buf->array());
        }

        prevTick = tick;

        // Fill with data
        nBytes = getCompletePacketizedBufferTime(dataBuf, bufSize, udpSocket,
                                                 debug, &tick, &dataId, stats, tickPrescale, true);

        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in receiving data, %ld\n", nBytes);
            }
            return (0);
        }


        // The first tick received may be any value depending on # of backends receiving
        // packets from load balancer. Use the first tick received and subsequent ticks
        // to check the prescale.
        if (firstLoop) {
            prevTick = tick;
            firstLoop = false;
        }

        if (takeStats) {
            clock_gettime(CLOCK_MONOTONIC_RAW, &end);
            nanoSecs = ts_to_nano(end) - stats->startTime;
            stats->readTime += nanoSecs;
            stats->builtBuffers++;
//
//            if (++loopCount >= loops) {
//                fprintf(stderr, "time: start %" PRId64 ", end %" PRId64 ", diff %" PRId64 "\n", stats->startTime, ts_to_nano(end), nanoSecs);
//                loopCount = 0;
//            }
        }

        if (debug) {
            if (tick - prevTick != 0) {
                fprintf(stderr, "Expect %" PRIu64 ", got %" PRIu64 "\n", prevTick, tick);
            }
        }

        // See if data source was restarted with new, lower starting event number.
        if (tick >= prevTick) {
            largestTick = tick;
        }

        // How do we tell if a data source has been restarted? Hopefully this is good enough.
        bool restarted = (largestTick - tick > 1000);

        if (restarted) {
            if (noRestart) {
                fprintf(stderr, "\nRestarted data source %d so exit\n", dataId);
                fflush(stderr);
                exit(1);
            }

            //fprintf(stderr, "\nRestarted data source %d\n", sourceId);

            // Tell stat thread when restart happened so that rates can be calculated within reason
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            restartTime.tv_sec = now.tv_sec;
            restartTime.tv_nsec = now.tv_nsec;


            if (takeStats) {
                clearStats(stats);
            }
        }


        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale;

        if (!useLocalBuf) {
            // Done with this buffer, so get it reading for reading
            buf->limit(nBytes).position(0);


            if (dumpBufs) {
                bufSupply->release(bufItem);
            } else {
                bufSupply->publish(bufItem);
            }
        }
    }

    return nullptr;
}



// Arg to pass to buffer reassembly thread
typedef struct threadReadBufArg_t {
    /** One buffer supply for each reassembly thread. Used in threadReadBuffers thread. */
    std::shared_ptr<Supplier<BufferItem>> supplyMap;

    // shared ptr of stats
    std::shared_ptr<packetRecvStats> stats;

    int sourceCount;
    int sourceId;
    int core;

    bool debug;

} threadReadBufArg;




/**
 * Thread to read all buffers from a single, particular data source.
 * There is only one buffer supply for each data source.
 * Not all buffers have valid data, so ignore those with no data.
 * @param arg
 */
static void *threadReadBuffers(void *arg) {

    threadReadBufArg *tArg = (threadReadBufArg *) arg;
    int id = tArg->sourceId;
    auto & supply = tArg->supplyMap;

    //std::cerr << "2 id = " << id <<std::endl;

#ifdef __linux__

    int core = tArg->core;
    if (core > -1) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run read buffer thd for source " << id << " on core " << core << "\n";
        CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }

#endif


    std::cout << "   Started cleanup thread for source " << id << std::endl;

    std::shared_ptr<BufferItem> bufItem;

    // If bufs are not already dumped by the reassembly thread,
    // we need to put them back into the supply now.
    while (true) {
            // Grab a fully reassembled buffer from Supplier
            bufItem = supply->consumerGet();

//            if (bufItem->validData()) {
//                // Do something with buffer here.
//                // Data can be accessed thru the (shared pointer to) ByteBuffer object:
//                std::shared_ptr<ByteBuffer> buffer = bufItem->getBuffer();
//
//                // or more directly thru its byte array:
//                uint8_t *buf = bufItem->getBuffer()->array();
//                size_t dataBytes = bufItem->getBuffer()->limit();
//
//                // Get access to meta data from its packet RE header
//                // if data source id or tick value is needed.
//                reHeader hdr = bufItem->getHeader();
//            }

            // Release item for reuse
            supply->release(bufItem);
    }

    // Thread not needed and can exit.
    return nullptr;
}



///**
// * Thread to read all buffers from ALL data sources.
// * There is one buffer supply for each data source.
// * Not all buffers have valid data, so ignore those with no data.
// * @param arg
// */
//static void *threadReadAllBuffers(void *arg) {
//
//    threadArg *tArg = (threadArg *) arg;
//    int id = tArg->sourceId;
//    auto & supply = tArg->supplyMap;
//
//
//#ifdef __linux__
//
//    int core = tArg->core;
//    if (core > -1) {
//        cpu_set_t cpuset;
//        CPU_ZERO(&cpuset);
//
//        std::cerr << "Run read buffer thd for source " << id << " on core " << core << "\n";
//        CPU_SET(core, &cpuset);
//
//        pthread_t current_thread = pthread_self();
//        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
//        if (rc != 0) {
//            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
//        }
//    }
//
//#endif
//
//
//    std::cout << "   Started cleanup thread for source " << id << std::endl;
//
//    std::shared_ptr<BufferItem> bufItem;
//
//    // If bufs are not already dumped by the reassembly thread,
//    // we need to put them back into the supply now.
//    while (true) {
//        // Grab a fully reassembled buffer from Supplier
//        bufItem = supply->consumerGet();
//
////            if (bufItem->validData()) {
////                // Do something with buffer here.
////                // Data can be accessed thru the (shared pointer to) ByteBuffer object:
////                std::shared_ptr<ByteBuffer> buffer = bufItem->getBuffer();
////
////                // or more directly thru its byte array:
////                uint8_t *buf = bufItem->getBuffer()->array();
////                size_t dataBytes = bufItem->getBuffer()->limit();
////
////                // Get access to meta data from its packet RE header
////                // if data source id or tick value is needed.
////                reHeader hdr = bufItem->getHeader();
////            }
//
//        // Release item for reuse
//        supply->release(bufItem);
//    }
//
//    // Thread not needed and can exit.
//    return nullptr;
//}



/**
 * Method to produce a random value with given mean (level) and standard deviation.
 * It ensures the value is between 0 and 1.
 * @param mean
 * @param stddev
 * @return
 */
static float getRandomLevel(float mean, float stddev) {
    // Create a random number generator
    static std::random_device rd; // Non-deterministic random device
    static std::mt19937 generator(rd()); // Mersenne Twister random number generator

    // Create a normal distribution with specified mean and standard deviation
    std::normal_distribution<float> distribution(mean, stddev);

    // Generate a random value from the distribution
    float ran = distribution(generator);

    // Clamp the result between 0 and 1 to ensure it stays within bounds
    if (ran < 0.0f) {
        //std::cerr << "Clipping bottom val of " << ran << " to 0" << std::endl;
        ran = 0.f;
    }
    else if (ran > 1.0f) {
        //std::cerr << "Clipping top val of " << ran << " to 1" << std::endl;
        ran = 1.f;
    }

    return ran;
}



/**
 * Method to produce a fake PID error signal proportional to a given fill level (0 to 1)
 * while taking a set point into account.
 * It ensures the signal is between -1 and 1, inclusive.
 * @param fill   fill level of some queue (0 to 1).
 * @param setPt  set point of fill level for a PID loop.
 * @return fake PID error signal.
 */
static float getPidErrSig(float fill, float setPt) {

    // Set some limits & avoid dividing by 0 below
    if (setPt < 0.01f) setPt = 0.01f;
    else if (setPt > 1.0f) setPt = 1.f;

    if (fill < 0.0f) fill = 0.f;
    else if (fill > 0.99f) fill = 0.99f;
    //----------------------------------

    float err = 0.F;
    float maxSig =  1.F;
    float minSig = -1.F;

    if (fill <= setPt) {
        err = (-1.F/setPt)*fill + 1.F;
    }
    else {
        err = (1.F/(setPt - 1.F))*fill + (setPt/(1.F - setPt));
    }

    return err;
}




int main(int argc, char **argv) {

    int status;
    // Set this to max expected data size
    uint32_t bufSize = 100000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    uint16_t startingPort = 17750;
    int startingCore = -1;
    int coreCount = 1;
    int startingBufCore = -1;

    // # of fill levels to average together
    int fcount = 1000;

    std::vector<int> ids;

    bool debug = false;
    bool useIPv6 = false;
    bool pinCores = false;
    bool pinBufCores = false;
    bool dumpBufs = false;
    bool lumpBufs = false;
    bool noRestart = false;
    bool jointStats = false;
    bool direct = false;
    bool useFakeQueuelevel = false;

    float minFactor = 0.F;
    float maxFactor = 0.F;

    // Use if reporting fake queue levels to CP
    float qFakeFillMean = -1.F;
    float qFakeFillStdDev = 0.F;


    // CP stuff
    int portRange = 0; // translates to PORT_RANGE_1 in proto enum

    char dataAddr[16];
    memset(dataAddr, 0, 16);

    char uri[256];
    memset(uri, 0, 256);

    char fileName[256];
    memset(fileName, 0, 256);

    char beName[256];
    memset(beName, 0, 256);

    char adminToken[512];
    memset(adminToken, 0, 512);
    std::string token;

    //----------------------

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);


    parseArgs(argc, argv, &bufSize, &recvBufSize,
              &tickPrescale, &startingCore, &coreCount,
              &startingBufCore,
              &startingPort, &debug, &useIPv6,
              &dumpBufs, &lumpBufs,
              &noRestart, &jointStats, &direct,
              listeningAddr, dataAddr,
              uri, fileName, adminToken, ids,
              &minFactor, &maxFactor,
              &qFakeFillMean, &qFakeFillStdDev, &fcount);

    pinCores = startingCore >= 0;
    pinBufCores = startingBufCore >= 0;

    if (strlen(adminToken) < 1) {
        token = "udplbd_default_change_me";
    }
    else {
        token = adminToken;
    }

    // If set by caller
    if (qFakeFillMean >= 0.) {
        useFakeQueuelevel = true;
        std::cerr << "Using FAKE fill levels" << std::endl;
    }


#ifdef __linux__

    if (pinCores) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run main thd on core " << startingCore << "\n";
        // First cpu is at 0 for CPU_SET
        // (80 - 87) inclusive is best for receiving over network for ejfat nodes.
        // This app performs better if this main thread is pinned to core 80-87
        // event tho it doesn't use the network. That way it's on same chip as the
        // threads pinned to chip close to NIC.
        CPU_SET(startingCore, &cpuset);

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }

#endif

    int sourceCount = ids.size();

    if (sourceCount < 1) {
        ids.push_back(0);
        sourceCount = 1;
        std::cerr << "Defaulting to (single) source id = 0" << std::endl;
    }

    if (ids.empty()) {
        std::cerr << "ids is empty!" << "\n";
    }
    else {
        for (int i = 0; i < sourceCount; i++) {
            std::cerr << "ids[" << i << "] = " << ids[i] <<std::endl;
        }
    }


    std::string cpAddr;
    uint16_t cpPort;
    std::string lbId;
    std::string instanceToken;


    if (!direct) {
        //----------------------------------------------
        // Parse the URI (directly given or in file().
        // This gives CP connection info.
        //----------------------------------------------

        // Set default file name
        if (strlen(fileName) < 1) {
            strcpy(fileName, "/tmp/ejfat_uri");
        }

        ejfatURI uriInfo;
        bool haveEverything = false;

        // First see if the uri arg is defined, if so, parse it
        if (strlen(uri) > 0) {
            bool parsed = parseURI(uri, uriInfo);
            if (parsed) {
                // URI is in correct format
                if (!uriInfo.haveInstanceToken) {
                    std::cerr << "no instance token in URI, substitute admin token" << std::endl;
                    uriInfo.instanceToken = token;
                }
                haveEverything = true;
            }
        }

        // If no luck with URI, look into file
        if (!haveEverything && strlen(fileName) > 0) {

            std::ifstream file(fileName);
            if (file.is_open()) {
                std::string uriLine;
                if (std::getline(file, uriLine)) {
                    bool parsed = parseURI(uriLine, uriInfo);
                    if (parsed) {
                        if (!uriInfo.haveInstanceToken) {
                            std::cerr << "no instance token in file, substitute admin token" << std::endl;
                            uriInfo.instanceToken = token;
                        }
                        haveEverything = true;
                    }
                }

                file.close();
            }
        }

        //printUri(std::cerr, uriInfo);

        if (!haveEverything) {
            std::cerr << "no LB/CP info in uri or file" << std::endl;
            return 1;
        }

        cpAddr = uriInfo.cpAddr;
        cpPort = uriInfo.cpPort;
        lbId = uriInfo.lbId;
        instanceToken = uriInfo.instanceToken;
    }

    // Need to give this back end a name (no, not "horse's"),
    // base part of it on least significant 6 digits of current time in microsec
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int time = now.tv_nsec/1000L;
    snprintf(beName, 256, "be_%06d/lb/%s", (time % 1000000), lbId.c_str());


#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

    int TIMEOUT = 1;

    // Have one stats object for each reassembly thread and one for the UDP reading thread
    std::shared_ptr <packetRecvStats> buildStats[sourceCount];

    // Store each in a map, key = source id, val = stats
    auto allBuildStats = std::make_shared<std::unordered_map<int, std::shared_ptr<packetRecvStats>>>();

    for (int i = 0; i < sourceCount; i++) {
        buildStats[i] = std::make_shared<packetRecvStats>();
        clearStats(buildStats[i]);
        allBuildStats->insert({ids[i], buildStats[i]});
    }


    // Arrays for holding threads & their args
    pthread_t thds[sourceCount];

    //---------------------------------------------------
    // Each reassembly thd will have a buffer supply in which to hold reconstructed buffers.
    // Make these buffers sized as given on command line (100kB default) and expand as necessary.
    // For really small buffers (1 or 2 pkts), they may be created out of order.
    // In that case, orderedRelease flag should be false. In general, however, we cannot do that
    // since we may need to toss a buffer and we need all buffers to be explicitly released.
    //---------------------------------------------------

    // Note that 256 buffers will work for large incoming data rates.
    // If there are multiple sources, this can be divided amongst them.
    int ringSize = 256;
    BufferItem::setEventFactorySettings(ByteOrder::ENDIAN_LOCAL, bufSize);

    // Array of buffer supplies
    std::shared_ptr <Supplier<BufferItem>> supplyMaps[sourceCount];
    for (int i = 0; i < sourceCount; i++) {
        supplyMaps[i] = std::make_shared < Supplier < BufferItem >> (ringSize, false);
    }

    // For each data source ...
    for (int i = 0; i < sourceCount; i++) {
        // Start thread to reassemble buffers of packets from 1 source
        auto arg = (threadArg *) calloc(1, sizeof(threadArg));
        if (arg == nullptr) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        arg->supplyMap = supplyMaps[i];

        arg->stats = buildStats[i];
        arg->dump  = dumpBufs;
        arg->debug = debug;
        arg->sourceCount   = sourceCount;
        arg->pktConsumerId = i;
        arg->sourceId      = ids[i];
//fprintf(stderr, "assemble thd #%d: src id = %d\n", i, ids[i]);
        arg->bufferSize    = (int) bufSize;
        arg->tickPrescale  = (int) tickPrescale;

        arg->port = startingPort + i;
        arg->useIPv6 = useIPv6;
        arg->recvBufSize = recvBufSize;
        arg->listeningAddr = listeningAddr;

        if (pinCores) {
            arg->core = startingCore + coreCount * i;
            arg->coreCount = coreCount;
        } else {
            arg->core = -1;
        }

        status = pthread_create(&thds[i], NULL, threadAssembleFast, (void *) arg);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    //---------------------------------------------------
    // Thread(s) to read fully reassembled buffers
    //---------------------------------------------------
    if (!dumpBufs) {
        for (int i = 0; i < sourceCount; i++) {
            threadReadBufArg *targ = (threadReadBufArg *) calloc(1, sizeof(threadReadBufArg));
            if (targ == nullptr) {
                fprintf(stderr, "out of mem\n");
                return -1;
            }

            // Supply of buffers for holding reassembled data from a single source.
            // One from each reassembly thread.
            targ->supplyMap = supplyMaps[i];
            targ->debug = debug;
            targ->sourceId = ids[i];
            //std::cerr << "2 ids[" << i << "] = " << ids[i] <<std::endl;
            if (pinBufCores) {
                targ->core = startingBufCore + i;
            } else {
                targ->core = -1;
            }

            pthread_t thd;
            status = pthread_create(&thd, NULL, threadReadBuffers, (void *) targ);
            if (status != 0) {
                fprintf(stderr, "Error creating thread for reading pkts\n");
                return -1;
            }
        }
    }

    //---------------------------------------------------
    // Start thread to do rate printout
    //---------------------------------------------------
    threadStruct *targ = (threadStruct *) calloc(1, sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");

        return -1;
    }

    targ->jointStats = jointStats;
    targ->sourceCount = sourceCount;
    targ->stats = allBuildStats;
    targ->sourceIds = ids;

    pthread_t thd2;
    status = pthread_create(&thd2, NULL, rateThread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    //--------------------------------------------------------

    if (!direct) {

        // Fake things since there is no internal fifo ...

        int portRangeValue = getPortRange(sourceCount);
        auto range = PortRange(portRangeValue);
        std::cout << "GRPC client port range = " << portRangeValue << std::endl;


        float weight = 1.F;

        LbControlPlaneClient client(cpAddr, cpPort,
                                    dataAddr, startingPort, range,
                                    beName, instanceToken, lbId, weight,
                                    minFactor, maxFactor);

        // Register this client with the grpc server
        int32_t err = client.Register();
        if (err == 1) {
            printf("GRPC client %s communication error with server when registering, exit\n", beName);
            exit(1);
        }

        printf("GRPC client %s registered!\n", beName);


        // # of fill levels to average together, fcount, set above (default 1000)


        // stat sampling time in microsec (1 millisec)
        //int sampleTime = 1000;
        int sampleTime = 1000000/fcount; // 1 sec
        int adjustedSampleTime = sampleTime;


        // time period in millisec for reporting to CP
        uint32_t reportTime = 1000;

        // Since fill level gets sent directly, this no longer gets factored out
        int fifoCapacity = 1000;
        if (useFakeQueuelevel) {
            // just make sure this is > qFakeFillMean
            fifoCapacity = 10*qFakeFillMean;
        }

        // # of loops (samples) to comprise one reporting period =
        int loopMax   = 1000*reportTime / sampleTime; // reportTime in millisec, sampleTime in microsec
        int loopCount = loopMax;    // use to track # loops made

        // PID loop constants
        float Kp = 1.F;
        float Kd = 0.F;
        float Ki = 0.F;

        float setPoint = 5.F > fifoCapacity ? 1.F : fifoCapacity/100.F;
        float pidError = 0.F;

        // Keep fcount sample times worth (1 sec) of errors so we can use error from 1 sec ago
        // for PID derivative term. For now, fill w/ 0's.
        float oldestPidError, oldPidErrors[fcount];
        memset(oldPidErrors, 0, fcount*sizeof(float));

        // Keep a sliding window avg of fifo fill over fcount samples
        float runningFillTotal = 0., fillAvg;
        float fillValues[fcount];
        memset(fillValues, 0, fcount*sizeof(float));

        // Keep circulating thru array. Highest index is fcount - 1.
        float prevFill, curFill, fillPercent;
        // set first and last indexes right here
        int currentIndex = 0, earliestIndex = 1;

        // Change to floats for later computations
        float fifoCapacityFlt = static_cast<float>(fifoCapacity);
        float fcountFlt = static_cast<float>(fcount);

        // time stuff
        struct timespec t1, t2;
        int64_t totalTime, time; // microsecs
        int64_t totalTimeGoal = sampleTime * fcount;
        int64_t times[fcount];
        float deltaT; // "time" in secs
        int64_t absTime, prevAbsTime, prevFifoTime;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        prevFifoTime = prevAbsTime = 1000000L*(t1.tv_sec) + (t1.tv_nsec)/1000L; // microsec epoch time
        // Load times with current time for more accurate first round of rates
        for (int i=0; i < fcount; i++) {
            times[i] = prevAbsTime;
        }


        while (true) {

            // Use fake fifo fill level & PID error
            if (!useFakeQueuelevel) {
                std::this_thread::sleep_for(std::chrono::seconds(1));

                float fillPercent_ = 100.F;
                float pidError_ = 0.F;

                // Update the changing variables
                client.update(fillPercent_, pidError_);

                // Send to server
                err = client.SendState();
                if (err == 1) {
                    printf("GRPC client %s communication error with server during sending of data, exit\n", beName);
                    exit(1);
                }
            }
            else {
                // Delay between data points
                // sampleTime is adjusted below to get close to the actual desired sampling rate.
                std::this_thread::sleep_for(std::chrono::microseconds(adjustedSampleTime));

                // Read time
                clock_gettime(CLOCK_MONOTONIC, &t2);
                // time diff in microsec
                time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec) / 1000L);
                // convert to sec
                deltaT = static_cast<float>(time) / 1000000.F;
                // Get the current epoch time in microsec
                absTime = 1000000L * t2.tv_sec + t2.tv_nsec / 1000L;
                t1 = t2;


                // Keep count of total time taken for last fcount periods.

                // Record current time
                times[currentIndex] = absTime;
                // Subtract from that the earliest time to get the total time in microsec
                totalTime = absTime - times[earliestIndex];
                // Keep things from blowing up if we've goofed somewhere
                if (totalTime < totalTimeGoal) totalTime = totalTimeGoal;
                // Get oldest pid error for calculating PID derivative term
                oldestPidError = oldPidErrors[earliestIndex];


                // Read current fifo fill level
                // Random output from 0 to 1 with given mean & stddev, scaled up to fifoCapacity
                curFill = fifoCapacityFlt * getRandomLevel(qFakeFillMean, qFakeFillStdDev);
//printf("fill %f\n", curFill);

                // Previous value at this index
                prevFill = fillValues[currentIndex];
                // Store current val at this index
                fillValues[currentIndex] = curFill;
//printf("curFill %f, prevFill %f, index = %d, fillValues %f || mean %f / std %f\n",
//        curFill, prevFill, currentIndex, fillValues[currentIndex], qFakeFillMean, qFakeFillStdDev);
                // Add current val and remove previous val at this index from the running total.
                // That way we have added loopMax number of most recent entries at ony one time.
                runningFillTotal += curFill - prevFill;
//printf("total %f\n", runningFillTotal);

                // Under crazy circumstances, runningFillTotal could be < 0 !
                // Would have to have high fill, then IMMEDIATELY drop to 0 for about a second.
                // This would happen at very small input rate as otherwise it takes too much
                // time for events in q to be processed and q level won't drop as quickly
                // as necessary to see this effect.
                // If this happens, set runningFillTotal to 0 as the best approximation.
                if (runningFillTotal < 0.) {
                    printf("\nNEG runningFillTotal (%f), set to 0!!\n\n", runningFillTotal);
                    runningFillTotal = 0.;
                }

                fillAvg = runningFillTotal / fcountFlt;
                fillPercent = fillAvg / fifoCapacityFlt;
                // No longer send %fill, just send fill level
                //pidError = pid<float>(setPoint, fillPercent, deltaT, Kp, Ki, Kd, oldestPidError, totalTime);
                pidError = pid<float>(setPoint, fillAvg, deltaT, Kp, Ki, Kd, oldestPidError, totalTime);

                // Track pid error
                oldPidErrors[currentIndex] = pidError;

                // Set indexes for next round
                earliestIndex++;
                earliestIndex = (earliestIndex == fcount) ? 0 : earliestIndex;

                currentIndex++;
                currentIndex = (currentIndex == fcount) ? 0 : currentIndex;

                if (currentIndex == 0) {
                    // Use totalTime to adjust the effective sampleTime so that we really do sample
                    // at the desired rate set on command line. This accounts for all the computations
                    // that this code does which slows down the actual sample rate.
                    // Do this adjustment once every fcount samples.
                    float factr = totalTimeGoal / totalTime;
                    adjustedSampleTime = sampleTime * factr;

                    // If totalTime, for some reason, is really big, we don't want the adjusted time to be 0
                    // since a sleep_for(0) is very short. However, sleep_for(1) is pretty much the same as
                    // sleep_for(500).
                    if (adjustedSampleTime == 0) {
                        adjustedSampleTime = 500;
                    }

                    //fprintf(fp, "sampleTime = %d, totalT = %.0f\n", adjustedSampleTime, totalTime);
                }


                // Every "loopMax" loops
                if (--loopCount <= 0) {

                    // Update the changing variables
                    //client.update(fillPercent, pidError);
                    client.update(fillAvg, pidError);

                    // Send to server
                    err = client.SendState();
                    if (err == 1) {
                        printf("GRPC client %s communication error with server, exit\n", beName);
                        exit(1);
                    }

                    if (absTime - prevAbsTime >= 4000000) {
                        prevAbsTime = absTime;
                        printf("     Fifo level %d  Avg:  %.2f,  %.2f%%,  pid err %f\n\n",
                               ((int) curFill), fillAvg, (100.F * fillPercent), pidError);
                    }

                    loopCount = loopMax;
                }
            }
        }
    }
    else {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }


    return 0;
}

