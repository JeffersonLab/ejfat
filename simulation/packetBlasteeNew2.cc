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
 * Receive generated data sent by packetBlasterNew.c program(s).
 * This assumes there is an emulator or FPGA between this and the sending program.
 * Note that this program uses fast, non-locking ring buffers.
 * </p>
 *
 * <p>
 * In this program, a single, main thread reads from a single socket in which packets from multiple data sources
 * are arriving. There is a ring buffer with pre-allocated structures in which to store the
 * incoming packets. There is 2 additional threads which read packets from that ring in order to do the
 * reassembly. One of these reassembles even numbered ticks and the other odd.
 * There are also threads to do calculate rates and to read/dump reassembled buffers.
 * </p>
 * <p>
 * There are maps temporarily containing buffers in which to store reassembled data,
 * one for each incoming data source and specific tick.
 * The buffers come from a ring source much like the structs that hold packets also come from a (different) ring.
 * In the 2nd/3rd threads, packets are retrieved from that packet ring. These are placed in the proper buffer.
 * A buffer for a specific source/tick may already exist
 * (if not a new buf is grabbed) and it's filled with corresponding data.
 * When a buffer is fully reassembled, it's put back into the buffer ring from which it came.
 * There is one buffer supply for each of these 2 reassembly threads.
 * Note that while a buffer is being reassembled there is a place to store
 * it and find it according to its tick value (in unordered_map).
 * </p>
 * </p>
 * There is a thread which pulls off all reassembled buffers. Also another to do stats.
 * </p>
 *
 */

#include <cstdlib>
#include <iostream>
#include <time.h>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <errno.h>

#include "BufferItem.h"
#include "PacketsItem.h"
#include "PacketsItem2.h"
#include "SupplyItem.h"
#include "Supplier.h"
#include "SupplierN.h"

#include "ByteBuffer.h"
#include "ByteOrder.h"

#include "ejfat_assemble_ersap.hpp"

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


#define INPUT_LENGTH_MAX 256


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <starting listening UDP port (increment for each source)>]",
            "        [-b <internal buffer byte size>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-f <file for stats>]",
            "        [-dump (no thd to get & merge buffers)]",
            "        [-stats (keep stats)]",
            "        [-ids <comma-separated list of incoming source ids>]",
            "        [-pinRead <starting core #, 1 for each read thd>]",
            "        [-pinBuf <starting core #, 1 for each buf assembly thd>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with packetBlaster.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param bufSize       filled with buffer size.
 * @param recvBufSize   filled with UDP receive buffer size.
 * @param tickPrescale  expected increase in tick with each incoming buffer.
 * @param core          starting core id on which to run pkt reading threads.
 * @param coreBuf       starting core id on which to run buf assembly threads.
 * @param sourceIds     array of incoming source ids.
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param keepStats     keep and printout stats.
 * @param dump          don't have a thd which gets all buffers (for possible merging).
 * @param listenAddr    filled with IP address to listen on.
 * @param filename      filled with name of file in which to write stats.
 */
static void parseArgs(int argc, char **argv,
                      uint32_t* bufSize, int *recvBufSize, int *tickPrescale,
                      int *core, int *coreBuf, int *sourceIds, uint16_t* port,
                      bool *debug, bool *useIPv6, bool *keepStats, bool *dump,
                      char *listenAddr, char *filename) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, NULL, 1},
                          {"ip6",  0, NULL, 2},
                          {"pinRead",  1, NULL, 3},
                          {"pinBuf",  1, NULL, 4},
                          {"ids",  1, NULL, 5},
                          {"stats",  0, NULL, 6},
                          {"dump",  0, NULL, 7},
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




// Statistics


typedef struct threadStruct_t {
    int sourceCount;
    int *sourceIds;
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats;
    char filename[101];
} threadStruct;


// Thread to send to print out rates
static void *rateThread(void *arg) {

    int64_t byteCount, bufCount, pktCount;
    int64_t discardByteCount, discardBufCount, discardPktCount;
    int64_t dropByteCount, dropBufCount, dropPktCount;
    int64_t missingByteCount; // discarded + dropped

    // Ignore the first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    // Parse arg
    threadStruct *targ = static_cast<threadStruct *>(arg);
    int sourceCount = targ->sourceCount;
    int *sourceIds  = targ->sourceIds;

    auto stats = targ->stats;
    auto & mapp = (*(stats.get()));


    int64_t prevTotalPkts[sourceCount];
    int64_t prevTotalBytes[sourceCount];
    int64_t prevBuiltBufs[sourceCount];

    int64_t prevDiscardPkts[sourceCount];
    int64_t prevDiscardBytes[sourceCount];
    int64_t prevDiscardBufs[sourceCount];

    int64_t prevDropPkts[sourceCount];
    int64_t prevDropBytes[sourceCount];
    int64_t prevDropBufs[sourceCount];



    int64_t currTotalPkts[sourceCount];
    int64_t currTotalBytes[sourceCount];
    int64_t currBuiltBufs[sourceCount];

    int64_t currDiscardPkts[sourceCount];
    int64_t currDiscardBytes[sourceCount];
    int64_t currDiscardBufs[sourceCount];

    int64_t currDropPkts[sourceCount];
    int64_t currDropBytes[sourceCount];
    int64_t currDropBufs[sourceCount];



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
        fprintf(fp, "Sec,Source,PacketRate(kHz),DataRate(MB/s),Missing(bytes),TotalMissing(bytes),PktCpu,BufCpu\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate;
    int64_t totalMicroSec, microSec;
    struct timespec t1, t2, firstT;
    bool rollOver = false;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    // We've got to handle "sourceCount" number of data sources - each with their own stats
    while (true) {

        for (int i=0; i < sourceCount; i++) {
            prevTotalPkts[i]   = currTotalPkts[i];
            prevTotalBytes[i]  = currTotalBytes[i];
            prevBuiltBufs[i]   = currBuiltBufs[i];

            prevDiscardPkts[i]  = currDiscardPkts[i];
            prevDiscardBytes[i] = currDiscardBytes[i];
            prevDiscardBufs[i]  = currDiscardBufs[i];

            prevDropBytes[i]  = currDropBytes[i];
//            prevDropPkts[i]   = currDropPkts[i];
//            prevDropBufs[i]   = currDropBufs[i];
        }

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        microSec = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalMicroSec = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        for (int i=0; i < sourceCount; i++) {
            int src = sourceIds[i];
            currTotalPkts[i]    = mapp[src]->acceptedPackets;
            currTotalBytes[i]   = mapp[src]->acceptedBytes;
            currBuiltBufs[i]    = mapp[src]->builtBuffers;

            currDiscardPkts[i]  = mapp[src]->discardedPackets;
            currDiscardBytes[i] = mapp[src]->discardedBytes;
            currDiscardBufs[i]  = mapp[src]->discardedBuffers;

            currDropBytes[i]    = mapp[src]->droppedBytes;

            if (currTotalBytes[i] < 0) {
                rollOver = true;
            }
        }

        // Don't calculate rates until data is coming in
        if (skipFirst) {
            bool allSources = true;
            for (int i=0; i < sourceCount; i++) {
                if (currTotalPkts[i] < 1) {
                    allSources = false;
                }

                int src = sourceIds[i];
                currTotalBytes[i]   = mapp[src]->acceptedBytes    = 0;
                currTotalPkts[i]    = mapp[src]->acceptedPackets  = 0;
                currBuiltBufs[i]    = mapp[src]->builtBuffers     = 0;

                currDiscardPkts[i]  = mapp[src]->discardedPackets = 0;
                currDiscardBytes[i] = mapp[src]->discardedBytes   = 0;
                currDiscardBufs[i]  = mapp[src]->discardedBuffers = 0;

                currDropBytes[i]    = mapp[src]->droppedBytes     = 0;
            }

            if (allSources) skipFirst = false;
            firstT = t1 = t2;
            rollOver = false;
            continue;
        }

        // Start over tracking bytes and packets if #s roll over
        if (rollOver) {
            for (int i=0; i < sourceCount; i++) {
                int src = sourceIds[i];

                currTotalBytes[i]   = mapp[src]->acceptedBytes    = 0;
                currTotalPkts[i]    = mapp[src]->acceptedPackets  = 0;
                currBuiltBufs[i]    = mapp[src]->builtBuffers     = 0;

                currDiscardPkts[i]  = mapp[src]->discardedPackets = 0;
                currDiscardBytes[i] = mapp[src]->discardedBytes   = 0;
                currDiscardBufs[i]  = mapp[src]->discardedBuffers = 0;

                currDropBytes[i]    = mapp[src]->droppedBytes     = 0;
            }
            firstT = t1 = t2;
            rollOver = false;
            continue;
        }

        for (int i=0; i < sourceCount; i++) {
            int src = sourceIds[i];

            // Use for instantaneous rates/values
            byteCount = currTotalBytes[i] - prevTotalBytes[i];
            pktCount  = currTotalPkts[i]  - prevTotalPkts[i];
            bufCount  = currBuiltBufs[i]  - prevBuiltBufs[i];

            // TODO: Sign on this is WRONG!!!
            missingByteCount = (currDiscardBytes[i] + currDropBytes[i]) - (prevDiscardBytes[i] + prevDropBytes[i]);
            printf("  Packets:  currDiscardBytes = %" PRId64 ", currDropBytes = %" PRId64 ", prevDiscardBytes = %" PRId64 ", prevDropBytes = %" PRId64 "\n",
                   currDiscardBytes[i], currDropBytes[i], prevDiscardBytes[i], prevDropBytes[i]);


                    //            // Can't tell how many bufs & packets are completely dropped unless we know exactly what's coming in
//            discardByteCount = currDiscardBytes[i] - prevDiscardBytes[i];
//            discardPktCount  = currDiscardPkts[i]  - prevDiscardPkts[i];
//            discardBufCount  = currDiscardBufs[i]  - prevDiscardBufs[i];

            pktRate    = 1000000.0 * ((double) pktCount) / microSec;
            pktAvgRate = 1000000.0 * ((double) currTotalPkts[i]) / totalMicroSec;
            if (pktCount == 0 && missingByteCount == 0) {
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg\n", sourceIds[i], pktRate, pktAvgRate);
            }
            else {
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg, missing bytes = %" PRId64 "\n",
                       src, pktRate, pktAvgRate, missingByteCount);
            }

            // Actual Data rates (no header info)
            dataRate    = ((double) byteCount) / microSec;
            dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSec;

            // TODO: currently cpuPkt holds nothing, set in main thread - one value

#ifdef __linux__
            printf("     Data:  %3.4g MB/s,  %3.4g Avg, pkt cpu %d, buf cpu %d, bufs %u\n",
                   dataRate, dataAvgRate, mapp[src]->cpuPkt, mapp[src]->cpuBuf, mapp[src]->builtBuffers);

           if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 ",%d,%d\n", totalMicroSec/1000000, src,
                        (int)(pktRate/1000), (int)(dataRate),
                        missingByteCount, (currDiscardBytes[i] + currDropBytes[i]),
                        mapp[src]->cpuPkt, mapp[src]->cpuBuf);
                fflush(fp);
            }
#else
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, bufs %u\n",
                   dataRate, dataAvgRate, mapp[src]->builtBuffers);

            if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 "\n", totalMicroSec/1000000, src,
                        (int)(pktRate/1000), (int)(dataRate),
                        missingByteCount, (currDiscardBytes[i] + currDropBytes[i]));
                fflush(fp);
            }
#endif

        }

//        printf("     Combined Bufs:    %u\n\n", stats[0]->combinedBuffers);

        t1 = t2;
    }

    fclose(fp);
    return (NULL);
}



// Arg to pass to buffer reassembly thread
typedef struct threadArg_t {
    /** Supply of buffers for holding reassembled data.
     *  One for each reassembly thread. */
    std::shared_ptr<Supplier<BufferItem>> bufSupply1;
    std::shared_ptr<Supplier<BufferItem>> bufSupply2;

    /** Supply of structures holding UDP packets. */
    std::shared_ptr<SupplierN<PacketsItem>> pktSupply;

    /** Byte size of buffers contained in supply. */
    int bufferSize;
    int mtu;
    int sourceCount;
    int consumerId;

    bool debug;
    bool dump;
    bool useOneIovecBuf;
    bool buildEvenTicks;

    // shared ptr of map of stats
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats;

    uint64_t expectedTick;
    int tickPrescale;
    int core;

} threadArg;


/**
 * <p>
 * Thread to assemble incoming packets from one data ID into a single buffer.
 * It gets packets that have already been read in and placed into a disruptor's
 * circular buffe by another thread.
 * It places the assembled buffer into another circular buffer for gathering
 * by yet another thread.
 * </p>
 *
 * <p>
 * If the given tick value is <b>NOT</b> 0xffffffffffffffff, then it is the next expected tick.
 * And in this case, this method makes a number of assumptions:
 * <ul>
 * <li>Each incoming buffer/tick is split up into the same # of packets.</li>
 * <li>Each successive tick differs by tickPrescale.</li>
 * <li>If the sequence changes by 2, then a dropped packet is assumed.
 *     This results from the observation that for a simple network,
 *     there are never out-of-order packets.</li>
 * </ul>
 * </p>
 *
 *
 * @param arg   struct to be passed to thread.
 */
static void *threadAssemble(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int core = tArg->core;
    int sourceCount = tArg->sourceCount;
    int id = tArg->consumerId;

    std::shared_ptr<Supplier<BufferItem>> bufSupply;
    if (id == 0) {
        bufSupply = tArg->bufSupply1;
    }
    else {
        bufSupply = tArg->bufSupply2;
    };

    std::shared_ptr<SupplierN<PacketsItem>> pktSupply = tArg->pktSupply;

    bool dumpBufs = tArg->dump;
    bool debug    = tArg->debug;
    bool useOneIovecBuf = tArg->useOneIovecBuf;
    bool buildEven = tArg->buildEvenTicks;

    auto stats    = tArg->stats;
    auto & mapp = *stats;

    // expected max size of packets
    int mtu = tArg->mtu;
    if (mtu < 1) {
        mtu = 9000;
    }
    else if (mtu < 1400) {
        mtu = 1400;
    }

    int tickPrescale = tArg->tickPrescale;
    std::cerr << "Assemble thd tickPrescale = " << tickPrescale << "\n";

    // Track cpu by calling sched_getcpu roughly once per sec or 2
    int cpuLoops = 2000;
    int loopCount = cpuLoops;
    int clearLoop = 1;

    bool takeStats = stats != nullptr;

    std::shared_ptr<BufferItem>  bufItem;
    std::shared_ptr<PacketsItem> pktItem;
    std::shared_ptr<ByteBuffer>  buf;

#ifdef __linux__

    if (core > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run assemble thd for all sources on core " << core << "\n";
        CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }

//        if (takeStats) {
//            stats->cpuBuf = sched_getcpu();
//        }
    }

#endif

    // The following maps are very small.
    // No need to get fancy here as searches will be brief.

    // One map for each source: key = source id, val = pointer to map
    std::unordered_map<int, std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *> maps;

    // For a single source, a map keeps buffers being worked on: key = tick, val = buffer
    std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> buffers;
    std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *pmap;

    // One tick for each source: key = source id, val = largest tick value received
    std::unordered_map<int, uint64_t> largestSavedTick;


    while (true) {

        //-------------------------------------------------------------
        // Get item containing packets previously read in by another thd
        //-------------------------------------------------------------
        pktItem = pktSupply->consumerGet(id);
        size_t packetCount = pktItem->getPacketsFilled();

        int srcId, prevSrcId = -1;
        uint64_t tick, prevTick = UINT64_MAX;
        pmap = nullptr;

        assert(packetCount < 1);

        for (int i = 0; i < packetCount; i++) {
            reHeader *hdr = pktItem->getHeader(i);
            tick = hdr->tick;
            bool tickEven = tick % 2 == 0;

            // Skip this tick and go to the next if looking for odd and have even or vice versa
            if ((tickEven && !buildEven) || (!tickEven && buildEven)) {
                continue;
            }

            srcId = hdr->dataId;
            if (srcId != prevSrcId) {
                // Switching to a different data source ...

                // Get the right map if there is one, else make one
                pmap = maps[srcId];
                if (pmap == nullptr) {
//std::cout << "Create map for src " << srcId << std::endl;
                    maps[srcId] = pmap = new std::unordered_map<uint64_t, std::shared_ptr<BufferItem>>();
                }
//                else {
//                    std::cout << "Found map for src " << srcId << std::endl;
//                }

                // Get buffer in which to reassemble
                bufItem = (*pmap)[hdr->tick];
                // If there is no buffer existing for this tick, get one
                if (bufItem == nullptr) {
//std::cout << "  create buf for tick " << hdr->tick << std::endl;
                    // This call gets a reset bufItem
                    (*pmap)[hdr->tick] = bufItem = bufSupply->get();

                    // Copy header so that whoever gets the reassembled buffer has info about tick, src id, etc
                    bufItem->setHeader(hdr);

                    // Track the biggest tick to be RECEIVED from this source.
                    // Anything too much smaller will be tossed since a late packet cannot be
                    // really, really late.
                    if (hdr->tick > largestSavedTick[srcId]) {
//std::cout << "biggest tick " << hdr->tick << " for src " << srcId << std::endl;
                        largestSavedTick[srcId] = hdr->tick;
                    }
                }
//                else {
//                    std::cout << "  got buf for tick " << hdr->tick << std::endl;
//                }
            }
            else if (hdr->tick != prevTick) {
                // Same source as last pkt, but if NOT the same tick ...
                bufItem = (*pmap)[hdr->tick];
                if (bufItem == nullptr) {
//std::cout << "same source, create buf for tick " << hdr->tick << std::endl;
                    (*pmap)[hdr->tick] = bufItem = bufSupply->get();
                    bufItem->setHeader(hdr);
                    if (hdr->tick > largestSavedTick[srcId]) {
//std::cout << "biggest tick " << hdr->tick << " for src " << srcId << std::endl;
                        largestSavedTick[srcId] = hdr->tick;
                    }
                }
//                else {
//                    std::cout << "same source, have buf for tick " << hdr->tick << std::endl;
//                }
            }

            // Keep track so if next packet is same source/tick, we don't have to look it up
            prevSrcId = srcId;
            prevTick  = hdr->tick;

            // We are using the ability of the BufferItem to store a user's long.
            // Use it store how many bytes we've copied into it so far.
            // This will enable us to tell if we're done w/ reassembly.
            int32_t pktsSoFar  = bufItem->getUserInt();
            int64_t bytesSoFar = bufItem->getUserLong();
            int64_t dataLen    = pktItem->getPacket(i)->msg_len - HEADER_BYTES;
//std::cout << "pkt len " << dataLen << std::endl;

            // Do we have memory to store entire buf? If not, expand.
            if (hdr->length > bufItem->getBuffer()->capacity()) {
std::cout << "EXPAND BUF!!! to " << hdr->length << std::endl;
                // Preserves all existing data while increasing underlying array
                bufItem->expandBuffer(hdr->length + 27000); // also fit in 3 extra jumbo packets
            }

            // The neat thing about doing it this way is we don't have to track out-of-order packets.
            // We don't have to copy and store them!
            //
            // Duplicate packets just write over themselves. However,
            // duplicate packets will mess up our calculation on whether we've received all packets
            // that make up a buffer. The easiest way to deal with that is sending UDP over a single
            // network interface and having the receiver listen on a specific interface (not INADDR_ANY).
            // So, send in a manner in which duplication will not happen. I think we can control this
            // without too much effort.
            // The alternative is to add a sequential number to RE header to be able to track this.
            // That requires more bookkeeping on this receiving end.

//            if (useOneIovecBuf) {
                // Copy things into buf from the 1 iovec buffer
                memcpy(bufItem->getBuffer()->array() + hdr->offset,
                       (char *)(pktItem->getPacket(i)->msg_hdr.msg_iov[0].iov_base) + HEADER_BYTES,
                       dataLen);
//            }
//            else {
//                // Copy things into buf from 2nd iovec buffer
//                memcpy(bufItem->getBuffer()->array() + hdr->offset,
//                       pktItem->getPacket(i)->msg_hdr.msg_iov[1].iov_base,
//                       dataLen);
//            }

            // Keep track of how much we've written so far
            bytesSoFar += dataLen;
            bufItem->setUserLong(bytesSoFar);

            // Track # of packets written so far (will double count for duplicate packets)
            pktsSoFar++;
            bufItem->setUserInt(pktsSoFar);

            // If we've written all data to this buf ...
            if (bytesSoFar >= hdr->length) {
                // Done with this buffer, so set its limit to proper value
                bufItem->getBuffer()->limit(bytesSoFar);

                // Clear buffer from local map
                pmap->erase(hdr->tick);
//std::cout << "   remove " << hdr->tick << std::endl;

                //std::cout << "Remove tck " << hdr->tick << " src " << srcId << " from map, bytes = " << bytesSoFar << std::endl;

                if (takeStats) {
                    //fprintf(stderr, "Look up stat for source %d\n", srcId);
                    mapp[srcId]->acceptedBytes += hdr->length;
                    mapp[srcId]->builtBuffers++;
                    mapp[srcId]->acceptedPackets += bufItem->getUserInt();
                }

                // Pass buffer to waiting consumer or just dump it
                if (dumpBufs) {
//std::cout << "release tck " << hdr->tick << " src " << srcId << std::endl;
                    bufSupply->release(bufItem);
                }
                else {
                    // TODO: Somehow this must be tagged with tick and source
//std::cout << "publish tck " << hdr->tick << " src " << srcId << std::endl;
                    bufSupply->publish(bufItem);
                }
            }
        }

        // If here, we've gone thru a bundle of UDP packets,
        // time to get the next bundle.
        pktSupply->release(pktItem, id);

        // May want to do some house cleaning at this point.
        // There may be missing packets which have kept some ticks from some sources
        // from being completely reassembled and need to cleared out.
        // So, for each source, take biggest tick to be saved and remove all existing ticks
        // less than 2*tickPrescale and still being constructed. Keep stats.

        for (const auto &n: largestSavedTick) {
            int source = n.first;
            uint64_t bigTick = n.second;
            //std::cout << "clear biggest " << bigTick  << std::endl;

            // Compare this tick with the ticks in maps[source] and remove if too old
            std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *pm = maps[source];
            assert(pm != nullptr);

            // Using the iterator and the "erase" method, as shown,
            // will avoid problems invalidating the iterator
            for (auto it = pm->cbegin(); it != pm->cend();) {

                uint64_t tck = it->first;
                //std::cout << "   try " << tck << std::endl;
                std::shared_ptr<BufferItem> bItem = it->second;

                //std::cout << "entry + (2 * tickPrescale) " << (tck + 2 * tickPrescale) << "< ?? bigT = " <<  bigTick << std::endl;

                // Remember, tick values do NOT wrap around.
                // It may make more sense to have the inequality as:
                // tck < bigTick - 2*tickPrescale
                // Except then we run into trouble early with negative # in unsigned arithemtic
                // showing up as huge #s. So have negative term switch sides.
                // The idea is that any tick < 2 prescales below max Tick need to be removed from maps
                if (tck + 2 * tickPrescale < bigTick) {
                    //std::cout << "Remove " << tck << std::endl;
                    //std::cout << "   purge " << tck << std::endl;
                    //pm->erase(it++);
                    it = pm->erase(it);

                    if (takeStats) {
                        mapp[source]->discardedBuffers++;
                        mapp[source]->discardedBytes += bItem->getUserLong();
                        mapp[source]->discardedPackets += bItem->getUserInt();

                        // We can't count buffers that were entirely dropped
                        // unless we know exactly what's coming in.
                        mapp[source]->droppedBytes += bItem->getHeader().length - bItem->getUserLong();
                        // guesstimate
                        mapp[source]->droppedPackets += mapp[source]->discardedBytes / mtu;
                    }

                    // Release resources here
                    if (dumpBufs) {
                        bufSupply->release(bItem);
                    }
                    else {
                        // We need to label bad buffers.
                        // Perhaps we could reuse them. But if we do that,
                        // things will be out of order and access to filled buffers will be delayed!
                        bItem->setValidData(false);
                        bufSupply->publish(bItem);
                    }

                }
                else {
                    ++it;
                }
            }
        }

        // Finish up some stats
        if (takeStats) {
#ifdef __linux__
            // If core hasn't been pinned, track it
            if ((core < 0) && (loopCount-- < 1)) {
                mapp[srcId]->cpuBuf = sched_getcpu();
                loopCount = cpuLoops;
//printf("Read pkt thd: get CPU\n");
            }
#endif
        }
    }

    return nullptr;
}



/**
 * Thread to read packets from 1 data ID.
 * @param arg
 * @return
 */
static void *threadReadBuffers(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int id = tArg->consumerId;
    std::shared_ptr<Supplier<BufferItem>>  bufSupply;
    if (id == 0) {
        bufSupply = tArg->bufSupply1;
    }
    else {
        bufSupply = tArg->bufSupply2;
    };
    bool dumpBufs = tArg->dump;
//    bool debug    = tArg->debug;
//    auto stats    = tArg->stats;
//    auto & mapp = (*(stats.get()));

    std::shared_ptr<BufferItem> bufItem;

    // If bufs are not already dumped by the reassembly thread,
    // we need to put them back into the supply now.
    if (!dumpBufs) {
        while (true) {
            // Grab a fully reassembled buffer from Supplier
            bufItem = bufSupply->consumerGet();

            if (bufItem->validData()) {
                // do something with buffer here
            }

            // Release item for reuse
            bufSupply->release(bufItem);
        }
    }

    // Thread not needed and can exit.
    return nullptr;
}


int main(int argc, char **argv) {

    int status;
    ssize_t nBytes;
    // Set this to max expected data size
    uint32_t bufSize = 100000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    uint16_t startingPort = 7777;
    int startingCore = -1;
    int startingBufCore = -1;
    int sourceIds[128];
    int sourceCount = 0;
    bool debug = false;
    bool useIPv6 = false;
    bool keepStats = false;
    bool pinCores = false;
    bool pinBufCores = false;
    bool dumpBufs = false;


    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);

    for (int i = 0; i < 128; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, &startingCore, &startingBufCore, sourceIds,
              &startingPort, &debug, &useIPv6, &keepStats, &dumpBufs, listeningAddr, filename);

    pinCores = startingCore >= 0 ? true : false;
    pinBufCores = startingBufCore >= 0 ? true : false;

    for (int i = 0; i < 128; i++) {
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


#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

    // Let's start with 100kB buffers
    int BUFSIZE = 100000;
    int TIMEOUT = 1;
    int sockfd, retval;

    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint16_t dataId;

    // Shared pointer to map w/ key = source id & val = shared ptr of stats object
    auto stats = std::make_shared<std::unordered_map<int, std::shared_ptr<packetRecvStats>>>();
    auto &mapp = *stats;
    for (int i = 0; i < sourceCount; i++) {
        if (keepStats) {
      //      stats->insert(std::make_pair(sourceIds[i], std::make_shared<packetRecvStats>()));
fprintf(stderr, "Store stat for source %d\n", sourceIds[i]);
            mapp[sourceIds[i]] = std::make_shared<packetRecvStats>();
            clearStats(mapp[sourceIds[i]]);
        }
        else {
            mapp[sourceIds[i]] = nullptr;
        }
    }

    //    std::shared_ptr<packetRecvStats> stats = nullptr;
    //    if (keepStats) {
    //        stats = std::make_shared<packetRecvStats>();
    //        clearStats(stats);
    //    }


    //---------------------------------------------------
    // Create socket to read data from source ID
    //---------------------------------------------------
    int udpSocket;

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
        if (debug) fprintf(stderr, "UDP socket for all sources, recv buffer = %d bytes\n", recvBufSize);

        // Configure settings in address struct
        // Clear it out
        memset(&serverAddr6, 0, sizeof(serverAddr6));
        // it is an INET address
        serverAddr6.sin6_family = AF_INET6;
        // the startingPort we are going to receiver from, in network byte order
        serverAddr6.sin6_port = htons(startingPort);
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

        // Configure settings in address struct
        struct sockaddr_in serverAddr{};
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(startingPort);
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

        fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes\n", startingPort, recvBufSize);

    }

    // Use one read on one iovec buf to get packet and hdr together.
    // If false, read into 2 bufs, one just for header.
    bool useOneIovecBuf = true;

    //---------------------------------------------------
    // Supply in which each item holds 60 UDP packets
    // and parsed header info.
    //---------------------------------------------------
    int pktRingSize = 32;
    PacketsItem::setEventFactorySettings(60);
    std::shared_ptr<SupplierN<PacketsItem>> pktSupply =
            std::make_shared<SupplierN<PacketsItem>>(pktRingSize, true, 2);

    PacketsItem2::setEventFactorySettings(60);
    std::shared_ptr<Supplier<PacketsItem2>> pktSupply2 =
            std::make_shared<Supplier<PacketsItem2>>(pktRingSize, true);

    //---------------------------------------------------
    // Supply in which each buf will hold reconstructed buffer.
    // Make these buffers sized as given on command line (100kB default) and expand as necessary.
    //---------------------------------------------------
    int ringSize = 512;
    BufferItem::setEventFactorySettings(ByteOrder::ENDIAN_LOCAL, bufSize);
    std::shared_ptr<Supplier<BufferItem>> supply1 =
            std::make_shared<Supplier<BufferItem>>(ringSize, true);

    std::shared_ptr<Supplier<BufferItem>> supply2 =
            std::make_shared<Supplier<BufferItem>>(ringSize, true);

    //---------------------------------------------------
    // Start 1st thread to reassemble buffers of packets from all sources
    //---------------------------------------------------
    threadArg *tArg1 = (threadArg *) calloc(1, sizeof(threadArg));
    if (tArg1 == nullptr) {
        fprintf(stderr, "\n ******* ran out of memory\n\n");
        exit(1);
    }

    tArg1->bufSupply1 = supply1;
    tArg1->pktSupply = pktSupply;
    tArg1->stats = stats;
    tArg1->dump = dumpBufs;
    tArg1->debug = debug;
    tArg1->sourceCount = sourceCount;
    tArg1->useOneIovecBuf = useOneIovecBuf;
    tArg1->tickPrescale = 2;
    tArg1->consumerId = 0;
    tArg1->buildEvenTicks = true;
    if (pinBufCores) {
        tArg1->core = startingBufCore;
    }
    else {
        tArg1->core = -1;
    }

    pthread_t thd1;
    status = pthread_create(&thd1, NULL, threadAssemble, (void *) tArg1);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    //---------------------------------------------------
    // Start 2nd thread to reassemble buffers of packets from all sources
    //---------------------------------------------------
    threadArg *tArg2 = (threadArg *) calloc(1, sizeof(threadArg));
    if (tArg2 == nullptr) {
        fprintf(stderr, "\n ******* ran out of memory\n\n");
        exit(1);
    }

    tArg2->bufSupply2 = supply2;
    tArg2->pktSupply = pktSupply;
    tArg2->stats = stats;
    tArg2->dump = dumpBufs;
    tArg2->debug = debug;
    tArg2->sourceCount = sourceCount;
    tArg2->useOneIovecBuf = useOneIovecBuf;
    tArg2->tickPrescale = 2;
    tArg2->consumerId = 1;
    tArg2->buildEvenTicks = false;
    if (pinBufCores) {
        tArg2->core = startingBufCore + 1;
    }
    else {
        tArg2->core = -1;
    }

    pthread_t thd2;
    status = pthread_create(&thd2, NULL, threadAssemble, (void *) tArg2);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }


    //---------------------------------------------------
    // Thread(s) to read and/or dump fully reassembled buffers
    //---------------------------------------------------
    if (!dumpBufs) {
        threadArg *tArg = (threadArg *) calloc(1, sizeof(threadArg));
        if (tArg == nullptr) {
            fprintf(stderr, "out of mem\n");
            return -1;
        }

        tArg->bufSupply1 = supply1;
        tArg->bufSupply2 = supply2;
        tArg->stats = stats;
        tArg->dump = dumpBufs;
        tArg->debug = debug;
        tArg->sourceCount = sourceCount;
        tArg->consumerId = 0;

        tArg->expectedTick = 0;
        tArg->tickPrescale = 1;

        pthread_t thd;
        status = pthread_create(&thd, NULL, threadReadBuffers, (void *) tArg);
        if (status != 0) {
            fprintf(stderr, "Error creating thread for reading pkts\n");
            return -1;
        }

        threadArg *tArg22 = (threadArg *) calloc(1, sizeof(threadArg));
        if (tArg2 == nullptr) {
            fprintf(stderr, "out of mem\n");
            return -1;
        }

        tArg22->bufSupply1 = supply1;
        tArg22->bufSupply2 = supply2;
        tArg22->stats = stats;
        tArg22->dump = dumpBufs;
        tArg22->debug = debug;
        tArg22->sourceCount = sourceCount;
        tArg22->consumerId = 1;

        tArg22->expectedTick = 0;
        tArg22->tickPrescale = 1;

        pthread_t thd22;
        status = pthread_create(&thd22, NULL, threadReadBuffers, (void *) tArg22);
        if (status != 0) {
            fprintf(stderr, "Error creating thread for reading pkts\n");
            return -1;
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
        status = pthread_create(&thd2, NULL, rateThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    // Time Delay Here???

    std::shared_ptr<PacketsItem> item;

    struct timespec timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_nsec = 0;

#ifdef __linux__
    // Track cpu by calling sched_getcpu roughly once per sec or 2
    int cpuLoops = 2000;
    int loopCount = cpuLoops;
#endif


    again:

    while (true) {
        // Read all UDP packets here
        item = pktSupply->get();

        // Collect packets
        //int packetCount = recvmmsg(udpSocket, item->getPackets(), item->getMaxPacketCount(), MSG_WAITFORONE, &timeout);
        //int packetCount = recvmmsg(udpSocket, item->getPackets(), item->getMaxPacketCount(), MSG_WAITALL, &timeout);
        int packetCount = 0;
#ifdef __linux__
        // Getting rid of the timeout greatly speeds things up !!
        packetCount = recvmmsg(udpSocket, item->getPackets(), item->getMaxPacketCount(), MSG_WAITFORONE, nullptr);
#endif
        // Keep tabs on how many valid packets we have
        item->setPacketsFilled(packetCount);

        if (packetCount == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // if timeout, go 'round again
                goto again;
            }
            fprintf(stderr, "\n ******* error receiving UDP packets\n\n");
            exit(-1);
        }

        // Since all the packets have been read in, parse the headers
        // We could shift this code to the reassembly thread

        for (int i = 0; i < packetCount; i++) {
            unsigned int dataLen = item->getPacket(i)->msg_len;
            if (dataLen < 20) {
                // didn't read in enough data for even the header, ERROR
                fprintf(stderr, "\n ******* too little data in Datagram, bad data\n\n");
                exit(-1);
            }
            ejfat::parseReHeader(reinterpret_cast<char *>(item->getPacket(i)->msg_hdr.msg_iov[0].iov_base),
                                 item->getHeader(i));

        }

        // Send data to reassembly thread for consumption
        pktSupply->publish(item);

//#ifdef __linux__
//        // Finish up some stats
//        if (keepStats & !pinCores) {
//            // If core hasn't been pinned, track it
//            if ((startingCore < 0) && (loopCount-- < 1)) {
//                int cpuPkt = sched_getcpu();
//                loopCount = cpuLoops;
////printf("Read pkt thd: get CPU\n");
//            }
//        }
//#endif
    }

    return 0;
}
