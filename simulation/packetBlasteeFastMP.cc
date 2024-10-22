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
 * Each reassembly thread accounts for out-of-order and duplicate packets.
 * It will only hold a limited number of partially constructed events in an effort to wait for
 * straggling packets with the oldest events eventually being discarded.
 * There is a 2-tiered solution for this problem.
 * The solution is to set a small "hold" time defined in FAST_STORE_MICROSEC as 2000 in order
 * to handle high event input rates. Also, the max size of the holding map is limited
 * by FAST_MAP_MAX to allow for fast searches.
 * Once this fast hold time or map size is exceeded, the partial event is copied and stored in
 * the "slow" map. The max # of items in the slow map and the max time to store it are
 * given by SLOW_MAP_MAX and SLOW_STORE_MILLISEC. If size is exceeded, the oldest item
 * is deleted to make room. Anything stored over the time limit is deleted.
 * </p>
 * <p>
 * Use packetBlaster.cc to produce events.
 * On an EFJAT node, to produce events at roughly 3 GB/s, use arg "-cores 60" where 60 can just as
 * easily be 0-63. To produce at roughly 3.4 GB/s, use cores 64-79, 88-127.
 * To produce at 4.7 GB/s, use cores 80-87. (Notices cores # start at 0).
 *
 * On an EJFAT node, this receiver will be able to receive data on a single thread of over
 * 3.3 GB/s dropping about 0.03%. It can receive at
 * 3.4 GB/s with dropping about 0.2% with 3 cores, but can't seem to be pushed beyond that.
 * To receive at high rates, the args "-pinRead 80 -pinCnt 2" must be used to specify the fastest
 * network cores for packet-reading-reassembling threads.
 * There needs to be 2 or 3 cores/thd to distribute the work, 2 is fine except when pushing the limit
 * in which case 3 helps a little.
 * Cores 80-87 have fastest access to the NIC.
 * Also, the main core needs to be pinned to the same chip to get least drops.
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
#include <errno.h>

#include "BufferItem.h"
#include "PacketStoreItem.h"
#include "SupplyItem.h"
#include "Supplier.h"

#include "ByteBuffer.h"
#include "ByteOrder.h"

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

// Limit the number of time that an incomplete buffer is kept,
// hoping for late packets to arrive. If this time is too large and the incoming
// data rate is high, then this program will run out of available buffers in which
// to construct events.
// This is due to the nature of the fast-ring-buffer-based buffer supply.
// It is set to account for non-sequential return of buffers to the supply.
// In practical terms, if a buffer is incomplete and therefore not returned back to
// the supply to be further processed downstream, the supply will not return all buffers
// which were obtained from the supply after the one being held back. So holding onto it
// for too long will mean running out of buffers.
//
// There is a 2-tiered solution for this problem.
// The solution is to decrease the "hold" time defined in FAST_STORE_MICROSEC.
// Suggestion: don't go above 2000 microsec for high rates. Also, the max size of this map
// is limited by FAST_MAP_MAX to allow for fast map searches.
// Once this fast hold time or map size is exceeded, the buffer is copied and stored in
// the "slow" map. The max # of items in the slow map and the max time to store it are
// given by SLOW_MAP_MAX and SLOW_STORE_MILLISEC. If size is exceeded, the oldest map item
// is deleted to make room. Anything stored over the time limit is deleted.

#define FAST_STORE_MICROSEC 2000
#define SLOW_STORE_MILLISEC 100
#define FAST_MAP_MAX 4
#define SLOW_MAP_MAX 20


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n",
            programName,
            "        [-h] [-v] [-ip6] [-norestart] [-jointstats]\n",

            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <starting listening UDP port (increment for each source, 17750 default)>]",
            "        [-b <internal buffer byte size, 100kB default>]",
            "        [-r <UDP receive buffer byte size, system default>]\n",

            "        [-addr  <data receiving address to register w/ CP>]",
            "        [-token <authentication token (for CP registration, default token_<time>)>]",
            "        [-range <data receiving port range, entropy of sender, default 0>]",
            "        [-gaddr <CP IP address (default = none & no CP comm)>]",
            "        [-gport <CP port (default 18347)>]",
            "        [-gname <name of this backend (default backend_<time>)>]\n",

            "        [-f <file for stats>]",
            "        [-dump (no thd to get & merge buffers)]",
            "        [-lump (1 thd to get & merge buffers from all sources)]",
            "        [-stats (keep stats)]",
            "        [-ids <comma-separated list of incoming source ids>]\n",

            "        [-pinRead <starting core # for read thds>]",
            "        [-pinCnt <# of cores for read thd>]",
            "        [-pinBuf <starting core #, 1 for each buf collection thd>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]\n");

    fprintf(stderr, "        If connecting to a CP by specifying -gaddr,\n");
    fprintf(stderr, "        this client only reports 0%% fill and ready for data.\n\n");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with packetBlaster.\n");
    fprintf(stderr, "        It can receive from multiple data sources simultaneously.\n\n");

    fprintf(stderr, "        By default, one thread started to \"process\" reassembled buffers for each source,\n");
    fprintf(stderr, "        unless -dump or -lump arg used.\n");
    fprintf(stderr, "        The -norestart flag means sources that restart and have new, lower event numbers,\n");
    fprintf(stderr, "        out of sync with other sources, will NOT be allowed\n");
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
 * @param sourceIds     array of incoming source ids.
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param keepStats     keep and printout stats.
 * @param dump          don't have a thd which gets all buffers (for possible merging).
 * @param noRestart     exit program if sender restarts.
 * @param jointStats    display stats of all sources joined together.
 * @param listenAddr    IP address to listen on.
 * @param filename      name of file in which to write stats.
 * @param dataAddr      IP address to send to CP as data destination addr for this program.
 * @param cpAddr        control plane IP address.
 * @param cpToken       foken string used into connecting to CP.
 * @param beName        name of this backend CP client.
 * @param cpPort        main control plane port.
 * @param range         range of ports in powers of 2 (entropy) for CP connection.
 */
static void parseArgs(int argc, char **argv,
                      uint32_t* bufSize, int *recvBufSize, int *tickPrescale,
                      int *core, int *coreCnt, int *coreBuf, int *sourceIds,
                      uint16_t* port,
                      bool *debug, bool *useIPv6, bool *keepStats,
                      bool *dump, bool *lump, bool *noRestart, bool *jointStats,
                      char *listenAddr, char *filename,
                      char *dataAddr, char *cpAddr, char *cpToken, char *beName,
                      uint16_t* cpPort, int *range) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",        1, nullptr, 1},
                          {"ip6",         0, nullptr, 2},
                          {"pinRead",     1, nullptr, 3},
                          {"pinBuf",      1, nullptr, 4},
                          {"ids",         1, nullptr, 5},
                          {"stats",       0, nullptr, 6},
                          {"dump",        0, nullptr, 7},
                          {"lump",        0, nullptr, 8},
                          {"pinCnt",      1, nullptr, 9},
                          {"norestart",   0, nullptr, 10},
                          {"jointstats",  0, nullptr, 11},

                          // Control Plane

                          {"addr",        1, nullptr, 12},
                          {"gaddr",       1, nullptr, 13},
                          {"gport",       1, nullptr, 14},
                          {"gname",       1, nullptr, 15},
                          {"token",       1, nullptr, 16},
                          {"range",       1, nullptr, 17},

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

            case 'f':
                // output stat file
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "Output file name too long/short, %s\n", optarg);
                    exit(-1);
                }
                strcpy(filename, optarg);
                *keepStats = true;
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
                        sourceIds[index] = (int) strtol(token.c_str(), &endptr, 0);

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
                        sourceIds[index] = (int) strtol(s.c_str(), nullptr, 0);
                        if (errno == EINVAL || errno == ERANGE) {
                            fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ids\n");
                            exit(-1);
                        }
                        index++;
                        //std::cout << s << std::endl;
                    }

                    if (index > MAX_SOURCES) {
                        fprintf(stderr, "Too many sources specified in -ids, max %d\n", MAX_SOURCES);
                        exit(-1);
                    }
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
                // control plane IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "grpc server IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cpAddr, optarg);
                break;


            case 14:
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

            case 15:
                // this client name to control plane
                if (strlen(optarg) > 32 || strlen(optarg) < 1) {
                    fprintf(stderr, "backend client name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(beName, optarg);
                break;

            case 16:
                // backend authentication token with control plane
                if (strlen(optarg) > 32 || strlen(optarg) < 1) {
                    fprintf(stderr, "authentication token length must be > 1 and < 33\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cpToken, optarg);
                break;

            case 17:
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

            case 6:
                // Keep stats
                *keepStats = true;
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

    // If wanting to register with a control plane ...
    if (std::strlen(cpAddr) > 0) {
        if (std::strlen(dataAddr) < 1) {
            fprintf(stderr, "If connecting to CP, must also specify -addr\n");
            exit(-1);
        }
    }
}




// Statistics


// If a source dies and restarts, get a handle on when it actually restarted
// so the stats/rates
static volatile struct timespec restartTime;


typedef struct threadStruct_t {
    bool jointStats;
    int  sourceCount;
    int  sourceIds[MAX_SOURCES];
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
    int sourceIds[MAX_SOURCES];
    memcpy(sourceIds, targ->sourceIds, sizeof(sourceIds));

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


/**
 * This method prints out the desired number of data bytes starting from the given index
 * without regard to the limit.
 *
 * @param buf     data to pring
 * @param bytes   number of bytes to print in hex
 * @param label   a label to print as header
 */
void printPktData(char *buf, size_t bytes, std::string const & label) {

    std::cout << label <<  ":" << std::endl;

    for (size_t i = 0; i < bytes; i++) {
        if (i%20 == 0) {
            std::cout << "\n  array[" << (i + 1) << "-" << (i + 20) << "] =  ";
        }
        else if (i%4 == 0) {
            std::cout << "  ";
        }

        printf("%02x ", (char)(buf[i]));
    }
    std::cout << std::endl << std::endl;
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
    int port            = tArg->port + sourceId;
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

    fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes, source count = %d\n",
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
 * How is the decision made to discard packets?
 * If packets come out-of-order, then more than one buffer will be constructed at one time.
 * This thread will only keep a limited number of partially constructed events in an effort to wait
 * for straggling packets with the oldest event being discarded if more storage is needed.
 * Also, old events will have their memory released if not fully constructed before a given
 * time limit.
 * After each packet is consumed, a scan is made to see if anything needs to be discarded.
 * All out-of-order and duplicate packets are handled.
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
static void *threadAssemble(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int id          = tArg->pktConsumerId;
    int sourceId    = tArg->sourceId;
    int port        = tArg->port + sourceId;
    int recvBufSize = tArg->recvBufSize;

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

    fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes, source count = %d\n",
            port, recvBufSize, sourceId);



    std::shared_ptr<BufferItem>  bufItem, prevBufItem;
    std::shared_ptr<ByteBuffer>  buf;

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

    // We implement a 2-tiered storage system.
    //
    // First, fastest tier #1:
    // Map #1 keeps BufferItems currently being worked on: key = tick, val = buffer .
    // Limit the number of ticks/events being worked on simultaneously to "FAST_MAP_MAX".
    // The map has buffers that are part of the buffer supply and thus must be
    // released quickly or it backs up the circular buffer.
    // If a buffer is there longer than FAST_STORE_MICROSEC microseconds, then it is
    // moved to longer term storage in tier 2.
    //
    // Slower tier #2:
    // Buffer items are COPIED into separate memory and placed into a separate map
    // so the supply's circular buffer can continue to function. Up to "SLOW_MAP_MAX"
    // items may stay there up to SLOW_STORE_MILLISEC milliseconds, whichever comes first.
    //
    // Both maps are sorted by tick with lowest value being the "first" and also the
    // first to be discarded.
    //
    std::map<uint64_t, std::shared_ptr<BufferItem>> fastTickMap;
    std::map<uint64_t, std::shared_ptr<BufferItem>> slowTickMap;

    // Keep a corresponding map of time - when each tick started being assembled.
    // key = tick, val = start time in microsec
    // This will allow us to dump old, partially assembled bufs/pkts
    std::map<uint64_t, int64_t> fastTimeMap;
    std::map<uint64_t, int64_t> slowTimeMap;

    // For reading time
    struct timespec now;
    int64_t microSec = 0;

    // Need to figure out if source was killed and restarted.
    // If this is the case, then the tick # will be much smaller than the previous value.
    int64_t tick, prevTick = -1, largestTick = -1, largestSlowTick = -1;
    uint32_t offset, length;
    int portIndex;
    bool restarted = false, fromSlowMap = false;
    size_t count;


    char pkt[PacketStoreItem::size];
    reHeader hdr;


    while (true) {

        //-------------------------------------------------------------
        // Get packet
        //-------------------------------------------------------------

        ssize_t bytesRead = recv(udpSocket, pkt, PacketStoreItem::size, 0);
        if (bytesRead < 0) {
            if (debug) fprintf(stderr, "recvmsg failed: %s\n", strerror(errno));
            exit(1);
        }
        else if (bytesRead < HEADER_BYTES) {
            if (debug) fprintf(stderr, "packet does not contain not enough data\n");
            exit(1);
        }

        // Parse header & store in item
        parseReHeader(pkt, &hdr);

        tick    = (int64_t)hdr.tick;
        offset  = hdr.offset;
        length  = hdr.length;
        portIndex = hdr.reserved;
        fromSlowMap = false;

        assert(hdr.dataId == sourceId);

        // If NOT the same tick as previous ...
        if (tick != prevTick) {

            bufItem = nullptr;

            // If tick is larger than largest, we'll need to get a new bufItem from supply

            // Also, if tick is <= largest tick received thus far,
            // it may be stored in one of the maps ...
            if (tick <= largestTick) {

                // If VERY OLD tick which will only be found in slow storage ...
                if (tick <= largestSlowTick) {
                    if (slowTickMap.count(tick)) {
                        bufItem = slowTickMap[tick];
                        fromSlowMap = true;
                    }
                }
                else {
                    // Look for it in fast storage
                    if (fastTickMap.count(tick)) {
                        bufItem = fastTickMap[tick];
                    }
                }

                if (bufItem == nullptr) {
                    // If there is no buffer associated with this tick available,
                    //    1) tick may have been reassembled and this is a late, duplicate packet, or
                    //    2) it is a very late packet and the incomplete buffer was deleted/released, or
                    //    3) data source was restarted with new, lower starting event number.
                    // In the first 2 cases, it can be released and ignored. So go to the next packet.
                    // In the last case, it's ok only if noRestart is false.

                    // How do we tell if a data source has been restarted?
                    // Hopefully this is good enough.
                    bool restarted = (largestTick - tick > 1000);

                    if (restarted) {
                        if (noRestart) {
                            fprintf(stderr, "\nRestarted data source %d so exit\n", sourceId);
                            fflush(stderr);
                            exit(1);
                        }

                        //fprintf(stderr, "\nRestarted data source %d\n", sourceId);

                        // Tell stat thread when restart happened so that rates can be calculated within reason
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        restartTime.tv_sec  = now.tv_sec;
                        restartTime.tv_nsec = now.tv_nsec;

                        // If source restarted, remove old stuff and start over. Release anything in maps.
                        if (fastTimeMap.size() > 0) {
                            // Iterate through the map
                            auto it = fastTimeMap.begin();
                            while (it != fastTimeMap.end()) {
                                // Release buffer resources
                                uint64_t tik = it->first;
                                auto bItem = fastTickMap[tik];

                                if (dumpBufs) {
                                    bufSupply->release(bItem);

                                } else {
                                    bItem->setValidData(false);
                                    bufSupply->publish(bItem);
                                }

                                // Erase the entry in fastTickMap associated with this tick
                                fastTickMap.erase(tik);

                                // Erase the current element, erase() will return the
                                // next iterator. So, don't need to increment.
                                it = fastTimeMap.erase(it);
                            }
                        }

                        slowTickMap.clear();
                        slowTimeMap.clear();

                        if (takeStats) {
                            clearStats(stats);
                        }
                    }
                    else  {
                        // std::cout << "  ignore old pkt from tick " << tick << std::endl;

                        // If we're here:
                        //    1) tick may have been reassembled and this is a duplicate packet, or
                        //    2) it is a very late packet
                        //
                        // In both cases, the incomplete buffer was deleted/released.
                        // So just ignore this packet.
                        // This actually happens quite often when packetBlaster has its "-sock" flag set to > 1.
                        // There seems to be a race condition between packets sent by one app but on different
                        // sockets!
                        //
                        // Unfortuneately, we just set bufItem = nullptr since tick changed. But, presumably,
                        // it will change back to the tick we were most recently working on.
                        // If we leave bufItem = nullptr, then a new buffer will be obtained from the supply
                        // and overwrite the previous causing many problems.

                        // So, go back and restore things back to they way they were before the really
                        // old packets showed up.
                        bufItem = prevBufItem;
                        continue;
                    }
                }
            }


            // Could not find it stored anywhere. This will only happen if it's the largest tick
            // received so far or the source was restarted and thus it is now the largest tick.
            if (bufItem == nullptr) {
                // With the load balancer, ticks can only increase. This new, largest tick
                // (or restarted tick) will not have an associated buffer yet, so create it,
                // store it, etc.

                fastTickMap[tick] = bufItem = bufSupply->get();
                bufItem->setEventNum(tick);
                bufItem->setDataLen(length);

                // store current time in microsec
                clock_gettime(CLOCK_MONOTONIC, &now);
                microSec = ((1000000L * now.tv_sec) + (now.tv_nsec / 1000L));
                fastTimeMap[tick] = microSec;

                largestTick = tick;
            }


            // At limit of # of bufs that can be worked on concurrently?
            // If we pulled bufItem out of the fastTickMap above, then this
            // should not be true.
            if (fastTickMap.size() > FAST_MAP_MAX) {

                // Find oldest entry in the fast map (still part of the supply)
                auto oldestEntry = *(fastTickMap.begin());
                uint64_t oldestTick = oldestEntry.first;
                std::shared_ptr<BufferItem> oldestBufItem = oldestEntry.second;

                // Copy entry
                auto slowBufItem = std::make_shared<BufferItem>(oldestBufItem);

                // Release oldestBufItem, that was just copied, back to the supply
                if (dumpBufs) {
                    bufSupply->release(oldestBufItem);
                }
                else {
                    oldestBufItem->setValidData(false);
                    bufSupply->publish(oldestBufItem);
                }

                // Place copy into slowTickMap but keep size of map under slowTickMapMax
                if (slowTickMap.size() >= SLOW_MAP_MAX) {

                    // Too many entries in slowTickMap, so remove oldest
                    auto oldestSlowEntry = *(slowTickMap.begin());
                    uint64_t oldestSlowTick = oldestSlowEntry.first;
                    auto oldestSlowItem = oldestSlowEntry.second;

                    if (takeStats) {
                        stats->discardedBuffers++;
                        stats->discardedBytes += oldestSlowItem->getDataLen();
                        // This will under count pkts discarded since not all have arrived yet
                        stats->discardedPackets  += oldestSlowItem->getUserInt();
                    }

                    // Remove oldest entry from slow map. NOTE:
                    // BufferItems in slowTickMap are not part of the supply and so can
                    // be deleted without having to publish or release back to the supply.
                    slowTickMap.erase(oldestSlowTick);
                    slowTimeMap.erase(oldestSlowTick);
//std::cout << "D " << " ";
                }

                // Put copied bufItem into slow map
                slowTickMap[oldestTick] = slowBufItem;
                slowTimeMap.insert({oldestTick, microSec});
                largestSlowTick = oldestTick;
//std::cout << "SL_" << slowTickMap.size() << " ";

                // Remove oldest entry from fast map
                fastTickMap.erase(oldestTick);
                fastTimeMap.erase(oldestTick);
//std::cout << "  remove partial tick " << oldestTick << ", fastTickMap size = " << fastTickMap.size() << std::endl;
            }
        }


        // Check for duplicate packets, ie if its offset has been stored already
        auto &offsets = bufItem->getOffsets();
//std::cout << portIndex << " " ;
        if (offsets.find(offset) != offsets.end()) {
            // There was already a packet with this offset, so ignore this duplicate packet!!
std::cout << "Got duplicate packet for event " << tick << ", offset " << offset << std::endl;
            continue;
        }
        else {
            // Record this packet's presence in buffer by storing the unique offset in a set
            offsets.insert(offset);
        }

        // Keep track so if next packet is same source/tick, we don't have to look it up
        prevTick = tick;
        prevBufItem = bufItem;

        // We are using the ability of the BufferItem to store a user's long.
        // Use it store how many bytes we've copied into it so far.
        // This will enable us to tell if we're done w/ reassembly.
        // Likewise, store # packets copied in user int.
        int64_t bytesSoFar = bufItem->getUserLong();
        int32_t pktsSoFar  = bufItem->getUserInt();
        int64_t dataLen    = bytesRead - HEADER_BYTES;

        //std::cout << "pkt len = " << dataLen << std::endl;

        // Do we have memory to store entire buf? If not, expand.
        if (length > bufItem->getBuffer()->capacity()) {
std::cout << "EXPAND BUF! to " << (length + 27000) << std::endl;
            // Preserves all existing data while increasing underlying array
            bufItem->expandBuffer(length + 27000); // also fit in 3 extra jumbo packets
        }

        // The neat thing about doing things this way is we don't have to track out-of-order packets.
        // Copy things into the buffer into its final spot.
        auto data = (uint8_t *)pkt + HEADER_BYTES;
        memcpy(bufItem->getBuffer()->array() + offset, data, dataLen);

        // Keep track of how much we've written so far
        bytesSoFar += dataLen;
        bufItem->setUserLong(bytesSoFar);

        // Track # of packets written so far
        pktsSoFar++;
        bufItem->setUserInt(pktsSoFar);

        // If we've written all data to this buf ...
        if (bytesSoFar >= length) {
            // Done with this buffer, so get it reading for reading
            bufItem->getBuffer()->limit(bytesSoFar).position(0);

            if (takeStats) {
                stats->acceptedBytes += bytesSoFar;
                stats->builtBuffers++;
                stats->acceptedPackets += bufItem->getUserInt();
            }

            if (fromSlowMap) {
                // It should be a VERY SELDOM thing that a very old partially-constructed buffer
                // gets a late packet, gets completely assembled, and is ready to be passed on.

                // Get it back into the supply by getting a free buffer from supply, copying this
                // item into the new buffer, and placing it back into the supply.
                auto bItem = bufSupply->get();
                bItem->copy(bufItem);
//std::cout << "\nResurrect" << std::endl;

                // Pass buffer to waiting consumer or just dump it
                if (dumpBufs) {
                    bufSupply->release(bItem);
                }
                else {
                    bufSupply->publish(bItem);
                }

                // Clear buffer from local map
                slowTickMap.erase(tick);
                slowTimeMap.erase(tick);
            }
            else {
                if (dumpBufs) {
                    bufSupply->release(bufItem);
                }
                else {
                    bufSupply->publish(bufItem);
                }

                // Clear buffer from local map
                fastTickMap.erase(tick);
                fastTimeMap.erase(tick);
            }
        }

        // See if items in fastTickMap have stayed over the limit
        if (fastTimeMap.size() > 0) {

            // Iterate through the map
            auto it = fastTimeMap.begin();

            while (it != fastTimeMap.end()) {
                // Check if "now" is more than FAST_STORE_MICROSEC microsec ahead of this entry
                if ((microSec - it->second) >= FAST_STORE_MICROSEC) {
                    // Move tick to slow map.
                    // It's too old, so copy & release buffer resources
                    uint64_t tik = it->first;
                    auto bItem = fastTickMap[tik];

                    // Copy buffer item
                    auto slowItem(bItem);

                    // Put orig item back into supply
                    if (dumpBufs) {
                        bufSupply->release(bItem);
                    }
                    else {
                        bItem->setValidData(false);
                        bufSupply->publish(bItem);
                    }

                    // Make space if needed
                    if (slowTickMap.size() >= SLOW_MAP_MAX) {
                        auto oldestSlowEntry = *(slowTickMap.begin());
                        uint64_t oldestSlowTick = oldestSlowEntry.first;
                        auto oldestSlowItem = oldestSlowEntry.second;

                        if (takeStats) {
                            stats->discardedBuffers++;
                            stats->discardedBytes   += oldestSlowItem->getDataLen();
                            stats->discardedPackets += oldestSlowItem->getUserInt();
                        }

                        slowTickMap.erase(oldestSlowTick);
                        slowTimeMap.erase(oldestSlowTick);
//std::cout << "D-" << " ";
                    }

                    // Put copied bItem into slow map
                    slowTickMap[tik] = slowItem;
                    slowTimeMap[tik] = microSec;
                    largestSlowTick  = tik;
//std::cout << "S-" << slowTickMap.size() << " ";

                    // Erase the entry in fastTickMap associated with this tick
                    fastTickMap.erase(tik);

                    // Erase the current element, erase() will return the
                    // next iterator. So, don't need to increment.
                    it = fastTimeMap.erase(it);
                }
                else {
                    // Reached newer entries within acceptable time limit, so end loop
                    break;
                }
            }
        }

        // Check map of stuff that's been hanging around for a while
        if (slowTimeMap.size() > 0) {
            auto   it  = slowTimeMap.begin();
            while (it != slowTimeMap.end()) {
                if ((microSec - it->second)/1000 >= SLOW_STORE_MILLISEC) {
                    // Dump too old tick
//std::cout << "D--" << slowTimeMap.size() << " ";
                    uint64_t tik = it->first;
                    auto bItem = slowTickMap[tik];

                    if (takeStats) {
                        stats->discardedBuffers++;
                        stats->discardedBytes   += bItem->getDataLen();
                        stats->discardedPackets += bItem->getUserInt();
                    }

                    slowTickMap.erase(tik);
                    it = slowTimeMap.erase(it);
                }
                else {
                    break;
                }
            }
        }

    }

    return nullptr;
}




/**
 * This reassembly thread is similar to {@link #threadAssemble()},
 * but it does not implement the 2 tiered system allowing for more extended waiting
 * on an event to be fully constructed. It's more like a single tiered algorithm,
 * but it does perform a little bit better. Currently this is unused.
 * @param arg
 * @return
 */

static void *threadAssembleGood(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int id = tArg->pktConsumerId;
    int sourceId = tArg->sourceId;
    int port = tArg->port + sourceId;
    int recvBufSize = tArg->recvBufSize;

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

        int optval = 1;
        setsockopt(udpSocket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

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

        int optval = 1;
        setsockopt(udpSocket, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

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

    fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes, source count = %d\n",
            port, recvBufSize, sourceId);



    std::shared_ptr<BufferItem>      bufItem, prevBufItem;
    std::shared_ptr<ByteBuffer>      buf;

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

    // Map keeps buffers being worked on: key = tick, val = buffer .
    // Limit the number of ticks/events being worked on simultaneously to 5.
    // Ticks from before that are delared to be "unassembleable" and are discarded.
    // Map is sorted by tick with lowest value being the "first" and also the first
    // to be discarded if need be.
    std::map<uint64_t, std::shared_ptr<BufferItem>> tickMap;
    size_t tickMapSize = 0;

    // Keep a corresponding map of time - when each tick started being assembled.
    // key = tick, val = start time in microsec
    // This will allow us to dump old, partially assembled bufs/pkts
    std::map<uint64_t, int64_t> timeMap;
    // For reading time
    struct timespec now;
    int64_t microSec = 0;

    // Need to figure out if source was killed and restarted.
    // If this is the case, then the tick # will be much smaller than the previous value.
    int64_t tick, prevTick = -1, largestTick = -1;
    uint32_t offset, length;
    bool restarted = false;

    int pktSizeMax = 9100;
    char pkt[pktSizeMax];
    reHeader hdr;


    while (true) {

        //-------------------------------------------------------------
        // Get item containing packet previously read in by main thd
        //-------------------------------------------------------------

        //ssize_t bytesRead = recvfrom(udpSocket, pkt, PacketStoreItem::size, 0, nullptr, nullptr);
        // We can use this with packetBlaster and is faster than recvfrom()
        ssize_t bytesRead = recv(udpSocket, pkt, PacketStoreItem::size, 0);
        if (bytesRead < 0) {
            if (debug) fprintf(stderr, "recvmsg failed: %s\n", strerror(errno));
            exit(1);
        }
        else if (bytesRead < HEADER_BYTES) {
            if (debug) fprintf(stderr, "packet does not contain not enough data\n");
            exit(1);
        }

        // Parse header & store in item
        parseReHeader(pkt, &hdr);

        tick    = (int64_t)hdr.tick;
        offset  = hdr.offset;
        length  = hdr.length;

        assert(hdr.dataId == sourceId);

        // If NOT the same tick as previous ...
        if (tick != prevTick) {

            bufItem = nullptr;

            // If tick is larger than largest, we'll need to get a new bufItem from supply.

            // Also, if tick is <= largest tick received thus far, may be stored in the map ...
            if (tick <= largestTick) {

                if (tickMap.count(tick)) {
                    bufItem = tickMap[tick];
                }
                else {
                    // If there is no buffer associated with this tick available,
                    //    1) tick may have been reassembled and this is a late, duplicate packet, or
                    //    2) it is a very late packet and the incomplete buffer was deleted/released, or
                    //    3) data source was restarted with new, lower starting event number.
                    // In the first 2 cases, it can be released and ignored. So go to the next packet.
                    // In the last case, it's ok only if noRestart is false.

                    // How do we tell if a data source has been restarted?
                    // Hopefully this is good enough.
                    bool restarted = (largestTick - tick > 1000);
//fprintf(stderr, "Smaller tick %" PRId64 "\n", tick);

                    if (restarted) {
                        if (noRestart) {
                            fprintf(stderr, "\nRestarted data source %d so exit\n", sourceId);
                            fflush(stderr);
                            exit(1);
                        }

                        // Tell stat thread when restart happened so that rates can be calculated within reason
                        struct timespec now;
                        clock_gettime(CLOCK_MONOTONIC, &now);
                        restartTime.tv_sec  = now.tv_sec;
                        restartTime.tv_nsec = now.tv_nsec;

                        // If source restarted, remove old stuff and start over. Release anything in maps.
                        if (timeMap.size() > 0) {
                            // Iterate through the map
                            auto it = timeMap.begin();
                            while (it != timeMap.end()) {
                                // Release buffer resources
                                uint64_t tik = it->first;
                                auto bItem = tickMap[tik];

                                if (dumpBufs) {
                                    bufSupply->release(bItem);

                                } else {
                                    bItem->setValidData(false);
                                    bufSupply->publish(bItem);
                                }

                                // Erase the entry in tickMap associated with this tick
                                tickMap.erase(tik);

                                // Erase the current element, erase() will return the
                                // next iterator. So, don't need to increment.
                                it = timeMap.erase(it);
                            }
                        }

                        if (takeStats) {
                            clearStats(stats);
                        }
                    }
                    else  {
                        //std::cout << "  ignore old pkt from tick " << tick << std::endl;
                        bufItem = prevBufItem;
                        continue;
                    }
                }
            }

            // Could not find it stored anywhere. This will only happen if it's the largest tick
            // received so far or the source was restarted and thus it is now the largest tick.
            if (bufItem == nullptr) {
                // With the load balancer, ticks can only increase. This new, largest tick
                // (or restarted tick) will not have an associated buffer yet, so create it,
                // store it, etc.

                tickMap[tick] = bufItem = bufSupply->get();
                bufItem->setEventNum(tick);
                bufItem->setDataLen(length);

                // store current time in msec
                clock_gettime(CLOCK_MONOTONIC, &now);
                microSec = ((1000000L * now.tv_sec) + (now.tv_nsec / 1000L));
                timeMap.insert({tick, microSec});

                largestTick = tick;
            }

            // Are we at limit of # of buffers being worked on? If so, discard oldest
            if (tickMap.size() > 4) {
                auto oldestEntry = *(tickMap.begin());
                uint64_t oldestTick = oldestEntry.first;
                std::shared_ptr<BufferItem> oldestBufItem = oldestEntry.second;
                tickMap.erase(oldestTick);
                timeMap.erase(oldestTick);
//std::cout << "  remove partial tick " << oldestTick << ", tickMap size = " << tickMap.size() << std::endl;

                if (takeStats) {
                    stats->discardedBuffers++;
                    stats->discardedBytes += oldestBufItem->getDataLen();
                    // This will under count pkts discarded since not all have arrived yet
                    stats->discardedPackets  += oldestBufItem->getUserInt();
                }

                // Do something with the buffer about to be discarded
                if (dumpBufs) {
                    bufSupply->release(oldestBufItem);
                }
                else {
                    oldestBufItem->setValidData(false);
                    bufSupply->publish(oldestBufItem);
                }
            }
        }

        // Check for duplicate packets, ie if its offset has been stored already
        auto &offsets = bufItem->getOffsets();
        if (offsets.find(offset) != offsets.end()) {
            // There was already a packet with this offset, so ignore this duplicate packet!!
            std::cout << "Got duplicate packet for event " << tick << ", offset " << offset << std::endl;
            continue;
        }
        else {
            // Record this packet's presence in buffer by storing the unique offset in a set
            offsets.insert(offset);
        }

        // Keep track so if next packet is same source/tick, we don't have to look it up
        prevTick = tick;
        prevBufItem = bufItem;

        // We are using the ability of the BufferItem to store a user's long.
        // Use it store how many bytes we've copied into it so far.
        // This will enable us to tell if we're done w/ reassembly.
        // Likewise, store # packets copied in user int.
        int64_t bytesSoFar = bufItem->getUserLong();
        int32_t pktsSoFar  = bufItem->getUserInt();
        int64_t dataLen    = bytesRead - HEADER_BYTES;

        //std::cout << "pkt len = " << dataLen << std::endl;

        // Do we have memory to store entire buf? If not, expand.
        if (length > bufItem->getBuffer()->capacity()) {
            std::cout << "EXPAND BUF! to " << (length + 27000) << std::endl;
            // Preserves all existing data while increasing underlying array
            bufItem->expandBuffer(length + 27000); // also fit in 3 extra jumbo packets
        }

        // The neat thing about doing things this way is we don't have to track out-of-order packets.
        // Copy things into the buffer into its final spot.
        auto data = (uint8_t *)pkt + HEADER_BYTES;
        memcpy(bufItem->getBuffer()->array() + offset, data, dataLen);

        // Keep track of how much we've written so far
        bytesSoFar += dataLen;
        bufItem->setUserLong(bytesSoFar);

        // Track # of packets written so far
        pktsSoFar++;
        bufItem->setUserInt(pktsSoFar);

        // If we've written all data to this buf ...
        if (bytesSoFar >= length) {
            // Done with this buffer, so get it reading for reading
            bufItem->getBuffer()->limit(bytesSoFar).position(0);

            // Clear buffer from local map
            tickMap.erase(tick);
            timeMap.erase(tick);

//std::cout << "Built tick " << tick << ", src " << sourceId << ", remove, bytes = " << bytesSoFar << std::endl;

            if (takeStats) {
                stats->acceptedBytes += bytesSoFar;
                stats->builtBuffers++;
                stats->acceptedPackets += bufItem->getUserInt();
            }

            // Pass buffer to waiting consumer or just dump it
            if (dumpBufs) {
                bufSupply->release(bufItem);
            }
            else {
                bufSupply->publish(bufItem);
            }
        }

        // See if items in tickMap/timeMap have stayed over the limit
        if (timeMap.size() > 0) {

            // Iterate through the map
            auto it = timeMap.begin();

            while (it != timeMap.end()) {
                // Check if "now" is more than FAST_STORE_MICROSEC microsec ahead of this entry
                if ((microSec - it->second) >= FAST_STORE_MICROSEC) {
//std::cout << " tick is TOO OLD" << std::endl;
                    // It's too old, so release buffer resources
                    uint64_t tik = it->first;
                    auto bItem = tickMap[tik];

                    if (takeStats) {
                        stats->discardedBuffers++;
                        stats->discardedBytes += bItem->getDataLen();
                        // This will under count pkts discarded since not all have arrived yet
                        stats->discardedPackets  += bItem->getUserInt();
                    }

                    if (dumpBufs) {
                        bufSupply->release(bItem);
                    }
                    else {
                        bItem->setValidData(false);
                        bufSupply->publish(bItem);
                    }

                    // Erase the entry in tickMap associated with this tick
                    tickMap.erase(tik);

                    // Erase the current element, erase() will return the
                    // next iterator. So, don't need to increment.
                    it = timeMap.erase(it);
                }
                else {
                    // We've reached the newer entries which are < 10 msec earlier, so end loop
                    break;
                }
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



/**
 * Thread to read all buffers from a ALL data sources.
 * There is one buffer supply for each data source.
 * Not all buffers have valid data, so ignore those with no data.
 * @param arg
 */
static void *threadReadAllBuffers(void *arg) {

    threadArg *tArg = (threadArg *) arg;
    int id = tArg->sourceId;
    auto & supply = tArg->supplyMap;


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
    int sourceIds[MAX_SOURCES];
    int sourceCount = 0;

    bool debug = false;
    bool useIPv6 = false;
    bool keepStats = false;
    bool pinCores = false;
    bool pinBufCores = false;
    bool dumpBufs = false;
    bool lumpBufs = false;
    bool noRestart = false;
    bool jointStats = false;

    // CP stuff
    int portRange = 0; // translates to PORT_RANGE_1 in proto enum
    uint16_t cpPort = 18347;

    char dataAddr[16];
    memset(dataAddr, 0, 16);

    char cpAddr[16];
    memset(cpAddr, 0, 16);

    char beName[33];
    memset(beName, 0, 33);

    char cpToken[33];
    memset(cpToken, 0, 33);
    //----------------------

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);

    for (int i = 0; i < MAX_SOURCES; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize,
              &tickPrescale, &startingCore, &coreCount,
              &startingBufCore, sourceIds,
              &startingPort, &debug, &useIPv6,
              &keepStats, &dumpBufs, &lumpBufs,
              &noRestart, &jointStats,
              listeningAddr, filename,
              dataAddr, cpAddr, cpToken, beName, &cpPort, &portRange);

    pinCores = startingCore >= 0;
    pinBufCores = startingBufCore >= 0;

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


    for (int i = 0; i < MAX_SOURCES; i++) {
        if (sourceIds[i] > -1) {
            sourceCount++;
            std::cerr << "Expecting source " << sourceIds[i] << " in position " << i << std::endl;
        } else {
            break;
        }
    }

    if (sourceCount < 1) {
        sourceIds[0] = 1;
        sourceCount = 1;
        std::cerr << "Defaulting to (single) source id = 1" << std::endl;
    }

    //---------------------------------------------------
    // Map to convert from data source id to the index in our local array of source ids
    //---------------------------------------------------
    std::unordered_map<int, int> srcMap;
    for (int i = 0; i < sourceCount; i++) {
        srcMap[sourceIds[i]] = i;
        //fprintf(stderr, "mainThd: srcMap[%d] = %d\n", sourceIds[i], i);
    }


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
    // key = source id, val = stats
    std::shared_ptr < std::unordered_map < int, std::shared_ptr < packetRecvStats >> > allBuildStats = nullptr;

    if (keepStats) {
        allBuildStats = std::make_shared < std::unordered_map < int, std::shared_ptr < packetRecvStats >> > ();

        for (int i = 0; i < sourceCount; i++) {
            buildStats[i] = std::make_shared<packetRecvStats>();
            clearStats(buildStats[i]);
            allBuildStats->insert({sourceIds[i], buildStats[i]});
        }
    }


    // Arrays for holding threads & their args
    pthread_t thds[sourceCount];
    threadArg *tArg[sourceCount];

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
//    // Map of buffer supplies. Each map --> key = src id, value = supply.
//    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMap;
    // array of buffer supplies
    std::shared_ptr <Supplier<BufferItem>> supplyMaps[sourceCount];
    for (int i = 0; i < sourceCount; i++) {
        supplyMaps[i] = std::make_shared < Supplier < BufferItem >> (ringSize, false);
    }

    // For each data source ...
    for (int i = 0; i < sourceCount; i++) {
        // Start thread to reassemble buffers of packets from 1 source
        auto arg = tArg[i] = (threadArg *) calloc(1, sizeof(threadArg));
        if (arg == nullptr) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        arg->supplyMap = supplyMaps[i];

        arg->stats = buildStats[i];
        arg->dump = dumpBufs;
        arg->debug = debug;
        arg->sourceCount = sourceCount;
        arg->pktConsumerId = i;
        arg->sourceId = sourceIds[i];
        arg->bufferSize = (int) bufSize;
        arg->tickPrescale = (int) tickPrescale;

        arg->port = startingPort;
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
            threadArg *targ = (threadArg *) calloc(1, sizeof(threadArg));
            if (targ == nullptr) {
                fprintf(stderr, "out of mem\n");
                return -1;
            }

            // Supply of buffers for holding reassembled data from a single source.
            // One from each reassembly thread.
            targ->supplyMap = supplyMaps[i];
            targ->noRestart = noRestart;
            targ->debug = debug;
            targ->sourceId = sourceIds[i];
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
    if (keepStats) {
        threadStruct *targ = (threadStruct *) calloc(1, sizeof(threadStruct));
        if (targ == nullptr) {
            fprintf(stderr, "out of mem\n");

            return -1;
        }

        targ->jointStats = jointStats;
        targ->sourceCount = sourceCount;
        targ->stats = allBuildStats;
        memcpy(targ->sourceIds, sourceIds, sizeof(sourceIds));

        pthread_t thd2;
        status = pthread_create(&thd2, NULL, rateThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }


//    enum PortRange {
//        PORT_RANGE_1 = 0;
//        PORT_RANGE_2 = 1;
//        PORT_RANGE_4 = 2;
//        PORT_RANGE_8 = 3;
//        PORT_RANGE_16 = 4;
//        PORT_RANGE_32 = 5;
//        PORT_RANGE_64 = 6;
//        PORT_RANGE_128 = 7;
//        PORT_RANGE_256 = 8;
//        PORT_RANGE_512 = 9;
//        PORT_RANGE_1024 = 10;
//        PORT_RANGE_2048 = 11;
//        PORT_RANGE_4096 = 12;
//        PORT_RANGE_8192 = 13;
//        PORT_RANGE_16384 = 14;
//    }


    // If using control plane, fake things since there is no internal fifo ...
    if (std::strlen(cpAddr) > 0) {
        // Convert integer range in PortRange enum
        // Set port range according to sourceCount
        switch (sourceCount) {
            case 1:
                portRange = 0;
                break;
            case 2:
                portRange = 1;
                break;
            case 3:
            case 4:
                portRange = 2;
                break;
            case 5:
            case 6:
            case 7:
            case 8:
                portRange = 3;
                break;
            default:
                // up to 16 inputs
                portRange = 4;
                break;
        }

        auto range = PortRange(portRange);
        float setPoint = 0.F;
        float fillPercent = 0.F;
        float pidError = 0.F;
        int eventSize = bufSize, numEvents = 1024;

        LbControlPlaneClient client(cpAddr, cpPort,
                                    dataAddr, startingPort, range,
                                    beName, cpToken,
                                    eventSize, numEvents, setPoint);

        // Register this client with the grpc server &
        // wait for server to send session token in return.
        // Token stored internally in client.
        int32_t err = client.Register();
        if (err == 1) {
            printf("GRPC client %s communication error with server when registering, exit\n", beName);
            exit(1);
        }

        printf("GRPC client %s registered!\n", beName);

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Update the changing variables
            client.update(fillPercent, pidError);

            // Send to server
            err = client.SendState();
            if (err == 1) {
                printf("GRPC client %s communication error with server during sending of data, exit\n", beName);
                exit(1);
            }
        }
    }
    else {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(7777));
        }
    }

    return 0;
}

