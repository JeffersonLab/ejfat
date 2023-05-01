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
 * Receive data sent by source ids given on command line.
 * Place those in an ET system acting as a fifo.
 * You'll have to coordinate the number of data sources, with the setting up of the ET system.
 * For example, for 3 sources, run the ET with something like:
 *
 * <code>
 *   et_start_fifo -f /tmp/fifoEt -d -s 150000 -n 3 -e 1000
 * </code>
 *
 * You can then run this program like:
 *
 * <code>
 *   udp_rcv_et -et /tmp/fifoEt -ids 1,3,76 -p 17750
 * </code>
 *
 * This expects data sources 1,3, and 76. There will be room in each ET fifo entry to have
 * 3 buffers (ET events), one for each source. There will be 1000 entries. Each buffer will
 * be 150kB. Max # of sources is 16 (can change that below).
 */

#include <iostream>
#include <thread>
#include "ejfat_assemble_ersap_et.hpp"

#ifdef __linux__
    #include <sched.h>
#endif


using namespace ejfat;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------

// Max # of data input sources
#define MAX_SOURCES 16

#define INPUT_LENGTH_MAX 256


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] ",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-ids <comma-separated list of incoming source ids>]",
            "        [-core <starting core # for read thd>]",
            "        [-pinCnt <# of cores for read thd>]",
            "        [-et <ET file name>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc        arg count from main().
 * @param argv        arg list from main().
 * @param port        filled with UDP port to listen on.
 * @param debug       filled with debug flag.
 * @param fileName    filled with output file name.
 * @param listenAddr  filled with IP address to listen on.
 */
static void parseArgs(int argc, char **argv, uint16_t* port, bool *debug, int *sourceIds,
                      int *core, int *coreCnt, char *etFileName, char *listenAddr) {

    int c, i_tmp;
    bool help=false, err=false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {
                    {"et", 1, NULL, 1},
                    {"ids",  1, NULL, 2},
                    {"core",  1, NULL, 3},
                    {"pinCnt",  1, NULL, 4},
                    {0,    0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:a:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 1:
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "ET file name is too long\n");
                    exit(-1);
                }
                strcpy(etFileName, optarg);
                break;

            case 2:
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

            case 3:
                // Cores to run on for packet reading thds
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *core = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -core, need starting core #\n");
                    exit(-1);
                }

                break;

            case 4:
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

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
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

            case 'v':
                // VERBOSE
                *debug = true;
                break;

            case 'h':
                help = true;
                break;

            default:
                fprintf(stderr, "default error, switch = %c\n", c);
                printHelp(argv[0]);
                exit(2);
        }

    }

    if (strlen(etFileName) < 1) {
        err = true;
    }

    if (help || err) {
        printHelp(argv[0]);
        exit(2);
    }
}


// Statistics


typedef struct threadStruct_t {
    int sourceCount;
    int *sourceIds;
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats;
} threadStruct;


// Thread to send to print out rates
static void *rateThread(void *arg) {

    int64_t byteCount, pktCount, bufCount, discardByteCount, discardBufCount;

    // Parse arg
    threadStruct *targ = static_cast<threadStruct *>(arg);
    int sourceCount = targ->sourceCount;
    int *sourceIds  = targ->sourceIds;

    for (int i=0; i < sourceCount; i++)  {
        std::cerr << "Stat thd, expecting source " << sourceIds[i] << " in position " << i << std::endl;
    }


    auto stats = targ->stats;
    auto & mapp = (*(stats.get()));


    int64_t prevTotalPkts[sourceCount];
    int64_t prevTotalBytes[sourceCount];
    int64_t prevBuiltBufs[sourceCount];

    int64_t prevDiscardBytes[sourceCount];
    int64_t prevDiscardBufs[sourceCount];

    int64_t currTotalPkts[sourceCount];
    int64_t currTotalBytes[sourceCount];
    int64_t currBuiltBufs[sourceCount];

    int64_t currDiscardBytes[sourceCount];
    int64_t currDiscardBufs[sourceCount];

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


    double pktRate, pktAvgRate, dataRate, dataAvgRate, bufRate, bufAvgRate;
    int64_t microSec;
    struct timespec tEnd, t1;
    bool rollOver = false, allSrcsSending = false;
    int sendingSrcCount = 0;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);

    // We've got to handle "sourceCount" number of data sources - each with their own stats
    while (true) {

        // Loop for zeroing stats when first starting - for accurate rate calc
        for (int i=0; i < sourceCount; i++) {
            if (dataArrived[i] && skippedFirst[i] == 1) {
                // Data is now coming in. To get an accurate rate, start w/ all stats = 0
                int src = sourceIds[i];
                currTotalBytes[i]   = mapp[src]->acceptedBytes    = 0;
                currTotalPkts[i]    = mapp[src]->acceptedPackets  = 0;
                currBuiltBufs[i]    = mapp[src]->builtBuffers     = 0;

                currDiscardBytes[i] = mapp[src]->discardedBytes   = 0;
                currDiscardBufs[i]  = mapp[src]->discardedBuffers = 0;

                // Start the clock for this source
                clock_gettime(CLOCK_MONOTONIC, &tStart[i]);
//fprintf(stderr, "started clock for src %d, clear currTotalPkts[%d] = %" PRId64 "\n", src, i, currTotalPkts[i]);

                // From now on we skip this zeroing step
                skippedFirst[i]++;
            }
        }

        for (int i=0; i < sourceCount; i++) {
            prevTotalPkts[i]   = currTotalPkts[i];
            prevTotalBytes[i]  = currTotalBytes[i];
            prevBuiltBufs[i]   = currBuiltBufs[i];

            prevDiscardBytes[i] = currDiscardBytes[i];
            prevDiscardBufs[i]  = currDiscardBufs[i];
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
            currTotalPkts[i]    = mapp[src]->acceptedPackets;
            currTotalBytes[i]   = mapp[src]->acceptedBytes;
            currBuiltBufs[i]    = mapp[src]->builtBuffers;

            currDiscardBytes[i] = mapp[src]->discardedBytes;
            currDiscardBufs[i]  = mapp[src]->discardedBuffers;

            if (currTotalBytes[i] < 0) {
                rollOver = true;
            }
        }

        // Don't start calculating stats until data has come in all channels for a full cycle.
        // Keep track of when that starts
        if (!allSrcsSending) {
            for (int i = 0; i < sourceCount; i++) {
                if (!dataArrived[i] && currTotalPkts[i] > 0) {
//fprintf(stderr, "currTotalPkts[%d] = %" PRId64 ", set dataArrived[%d] = true\n", i, currTotalPkts[i], i);
                    dataArrived[i] = true;
                    sendingSrcCount++;

                    if (sendingSrcCount == sourceCount) {
                        allSrcsSending = true;
//fprintf(stderr, "All %d data sources are sending data now\n", sourceCount);
                    }
                }
            }
        }

        // Start over tracking bytes and packets if #s roll over
        if (rollOver) {
            for (int i=0; i < sourceCount; i++) {
                int src = sourceIds[i];

                currTotalBytes[i]   = mapp[src]->acceptedBytes    = 0;
                currTotalPkts[i]    = mapp[src]->acceptedPackets  = 0;
                currBuiltBufs[i]    = mapp[src]->builtBuffers     = 0;

                currDiscardBytes[i] = mapp[src]->discardedBytes   = 0;
                currDiscardBufs[i]  = mapp[src]->discardedBuffers = 0;
            }
            t1 = tEnd;
            rollOver = false;
            continue;
        }

        for (int i=0; i < sourceCount; i++) {
            // Dota not coming in yet from this source so do NO calcs
            if (!dataArrived[i]) {
                std::cerr << "data not arrived from source " << sourceIds[i] << std::endl;
                continue;
            }

            // Skip first stat cycle as the rate calculations will be off
            if (skippedFirst[i] < 1) {
                //printf("%d skip %d\n", sourceIds[i], skippedFirst[i]);
                skippedFirst[i]++;
                continue;
            }

            int src = sourceIds[i];

            // Use for instantaneous rates/values
            byteCount = currTotalBytes[i] - prevTotalBytes[i];
            pktCount  = currTotalPkts[i]  - prevTotalPkts[i];
            bufCount  = currBuiltBufs[i]  - prevBuiltBufs[i];

            discardByteCount = currDiscardBytes[i] - prevDiscardBytes[i];
            discardBufCount  = currDiscardBufs[i]  - prevDiscardBufs[i];


            pktRate    = 1000000.0 * ((double) pktCount) / microSec;
            pktAvgRate = 1000000.0 * ((double) currTotalPkts[i]) / totalMicroSecs[i];
            printf("%d Packets:  %3.4g Hz,  %3.4g Avg\n", src, pktRate, pktAvgRate);

            // Actual Data rates (no header info)
            dataRate    = ((double) byteCount) / microSec;
            dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSecs[i];
            printf("     Data:  %3.4g MB/s,  %3.4g Avg, bufs %u, discard(%" PRId64 ", total %" PRId64 ")\n",
                   dataRate, dataAvgRate, mapp[src]->builtBuffers, discardByteCount, currDiscardBytes[i]);

            // Buffer rates
            bufRate    = 1000000.0 * ((double) bufCount) / microSec;
            bufAvgRate = 1000000.0 * ((double) currBuiltBufs[i]) / totalMicroSecs[i];
            printf("     Bufs:  %3.4g Hz,  %3.4g Avg, discard(%" PRId64 ", total %" PRId64 ")\n\n",
                   bufRate, bufAvgRate, discardBufCount, currDiscardBufs[i]);
        }

        t1 = tEnd;
    }

    return (nullptr);
}



int main(int argc, char **argv) {

    int udpSocket;
    // Set this to max expected data size
    uint16_t port = 7777;
    bool debug = false;

    int startingCore = -1;
    int coreCount = 1;
    int sourceIds[MAX_SOURCES];
    int sourceCount = 0;

    char etFileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(etFileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    for (int i = 0; i < MAX_SOURCES; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &port, &debug, sourceIds, &startingCore, &coreCount, etFileName, listeningAddr);

    for (int i = 0; i < MAX_SOURCES; i++) {
        if (sourceIds[i] > -1) {
            sourceCount++;
            std::cerr << "Expecting source " << sourceIds[i] << " in position " << i << std::endl;
        }
        else {
            break;
        }
    }

    if (sourceCount < 1) {
        sourceIds[0] = 0;
        sourceCount = 1;
        std::cerr << "Defaulting to (single) source id = 0" << std::endl;
    }

    bool pinCores = startingCore >= 0 ? true : false;

#ifdef __linux__

    if (pinCores) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        for (int i=0; i < coreCount; i++) {
            std::cerr << "Run receiving thd for all sources on core " << (startingCore + i) << "\n";
            // First cpu is at 0 for CPU_SET
            // (80 - 87) inclusive is best for receiving over network for ejfat nodes
            CPU_SET(startingCore + i, &cpuset);
        }

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }

#endif

    // Create UDP socket
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
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
        // TODO: handle error properly
        if (debug) fprintf(stderr, "bind socket error\n");
    }

    // TODO: cmd line option?
    bool keepStats = true;

    // Place to gather stats for each data source
    // Shared pointer to map w/ key = source id & val = shared ptr of stats object
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats = nullptr;

    if (keepStats) {
        stats = std::make_shared<std::unordered_map<int, std::shared_ptr<packetRecvStats>>>();
        auto &mapp = *stats;
        for (int i = 0; i < sourceCount; i++) {
            fprintf(stderr, "Store stat for source %d\n", sourceIds[i]);
            mapp[sourceIds[i]] = std::make_shared<packetRecvStats>();
            clearStats(mapp[sourceIds[i]]);
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

        targ->sourceCount = sourceCount;
        targ->stats = stats;
        targ->sourceIds = sourceIds;

        pthread_t thd2;
        int status = pthread_create(&thd2, NULL, rateThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    //-------------------------------------------------------
    // Connect to ET system, by default connect to local sys
    //-------------------------------------------------------

    et_sys_id etid;
    et_fifo_id fid;
    et_openconfig openconfig;

    et_open_config_init(&openconfig);
    if (et_open(&etid, etFileName, openconfig) != ET_OK) {
        fprintf(stderr, "et_open problems\n");
        exit(1);
    }

    size_t eventSize;
    err = et_system_geteventsize(etid, &eventSize);
    if (err != ET_OK || eventSize < 9000) {
        fprintf(stderr, "Events need to be at least 9000 bytes instead of %d\n", (int)eventSize);
        exit(1);
    }

    // We're a producer of FIFO data
    err = et_fifo_openProducer(etid, &fid, sourceIds, sourceCount);
    if (err != ET_OK) {
        et_perror(err);
        exit(1);
    }

    // Call routine that reads packets, puts data into fifo entry, places entry into ET in a loop
    getBuffers(udpSocket, fid, debug, 1, stats);

    return 0;
}



