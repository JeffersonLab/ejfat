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
 * Receive generated data sent by packetBlaster.c program(s).
 * This assumes there is an emulator or FPGA between this and the sending program.
 * Note that this program uses fast, non-locking, concurrent queues
 * (see blockingconcurrentqueue.h, concurrentqueue.h, readerwritercircularqueue.h,
 * readerwriterqueue.h, atomicops.h, and lightweightsemaphore.h).
 * </p>
 *
 * <p>
 * In this program, a single thread reads from a single socket in which packets from multiple data sources
 * are arriving. There is a concurrent queue with pre-allocated structures in which to store the
 * incoming packets. There is another thread which reads packets from that queue in order to do the
 * reassembly.
 * </p>
 * <p>
 * There are other queues, each with buffers in which to store reassembled data, one for each incoming data source.
 * In a second thread, packets are retrieved from that packet queue. Depending on the source, a buffer from that
 * source's buffer queue will be filled with corresponding data. When a buffer is fully reassembled, it's put back
 * into the buffer queue from which it came. Note that while a buffer is being reassembled there is a place to store
 * it and find it according to its tick value (in unordered_map).
 * </p>
 * </p>
 * Finally, there is a thread which pulls off all reassembled buffers.
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
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <errno.h>

#include "BufferSupply.h"
#include "BufferSupplyItem.h"

#include "BufferItem.h"
#include "PacketsItem.h"
#include "SupplyItem.h"
#include "Supplier.h"

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
    std::shared_ptr<packetRecvStats> *stats;
    char filename[101];
} threadStruct;


// Thread to send to print out rates
static void *rateThread(void *arg) {

    uint64_t packetCount, byteCount, droppedCount;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    // Parse arg
    threadStruct *targ = static_cast<threadStruct *>(arg);
    int sourceCount = targ->sourceCount;
    int *sourceIds  = targ->sourceIds;
    std::shared_ptr<packetRecvStats> *stats = targ->stats;

    uint64_t prevTotalPackets[sourceCount];
    uint64_t prevTotalBytes[sourceCount];
    uint64_t prevDroppedPackets[sourceCount];

    uint64_t currTotalPackets[sourceCount];
    uint64_t currTotalBytes[sourceCount];
    uint64_t currDroppedPackets[sourceCount];

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
        fprintf(fp, "Sec,Source,PacketRate(kHz),DataRate(MB/s),Dropped,TotalDropped,PktCpu,BufCpu\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate;
    int64_t totalMicroSec, microSec, droppedPkts, totalDroppedPkts = 0;
    struct timespec t1, t2, firstT;
    bool rollOver = false;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    // We've got to handle "sourceCount" number of data sources - each with their own stats
    while (true) {

        for (int i=0; i < sourceCount; i++) {
            prevTotalBytes[i]     = currTotalBytes[i];
            prevTotalPackets[i]   = currTotalPackets[i];
            prevDroppedPackets[i] = currDroppedPackets[i];
        }

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        microSec = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalMicroSec = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        for (int i=0; i < sourceCount; i++) {
            currTotalBytes[i]     = stats[i]->acceptedBytes;
            currTotalPackets[i]   = stats[i]->acceptedPackets;
            currDroppedPackets[i] = stats[i]->droppedPackets;
            if (currTotalBytes[i] < 0) {
                rollOver = true;
            }
        }

        // Don't calculate rates until data is coming in
        if (skipFirst) {
            bool allSources = true;
            for (int i=0; i < sourceCount; i++) {
                if (currTotalPackets[i] < 1) {
                    allSources = false;
                }
                currTotalBytes[i]     = stats[i]->acceptedBytes   = 0;
                currTotalPackets[i]   = stats[i]->acceptedPackets = 0;
                currDroppedPackets[i] = stats[i]->droppedPackets  = 0;
                currDroppedPackets[i] = stats[i]->droppedTicks    = 0;
                currDroppedPackets[i] = stats[i]->builtBuffers    = 0;
            }
            if (allSources) skipFirst = false;
            firstT = t1 = t2;
            rollOver = false;
            continue;
        }

        // Start over tracking bytes and packets if #s roll over
        if (rollOver) {
            for (int i=0; i < sourceCount; i++) {
                currTotalBytes[i]   = stats[i]->acceptedBytes   = 0;
                currTotalPackets[i] = stats[i]->acceptedPackets = 0;
            }
            firstT = t1 = t2;
            rollOver = false;
            continue;
        }

        for (int i=0; i < sourceCount; i++) {

            // Use for instantaneous rates/values
            byteCount    = currTotalBytes[i]     - prevTotalBytes[i];
            packetCount  = currTotalPackets[i]   - prevTotalPackets[i];
            droppedCount = currDroppedPackets[i] - prevDroppedPackets[i];

            pktRate    = 1000000.0 * ((double) packetCount) / microSec;
            pktAvgRate = 1000000.0 * ((double) currTotalPackets[i]) / totalMicroSec;
            if (packetCount == 0 && droppedCount == 0) {
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg\n", sourceIds[i], pktRate, pktAvgRate);
            }
            else {
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = %" PRIu64 "\n",
                       sourceIds[i], pktRate, pktAvgRate, droppedCount);
            }

            // Actual Data rates (no header info)
            dataRate    = ((double) byteCount) / microSec;
            dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSec;

#ifdef __linux__
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, pkt cpu %d, buf cpu %d, bufs %u\n",
                   dataRate, dataAvgRate, stats[i]->cpuPkt, stats[i]->cpuBuf, stats[i]->builtBuffers);

            if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRIu64 ",%" PRIu64 ",%d,%d\n", totalMicroSec/1000000, sourceIds[i], (int)(pktRate/1000), (int)(dataRate),
                        droppedCount, currDroppedPackets[i], stats[i]->cpuPkt, stats[i]->cpuBuf);
                fflush(fp);
            }
#else
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, bufs %u\n",
                   dataRate, dataAvgRate, stats[i]->builtBuffers);

            if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRIu64 ",%" PRIu64 "\n", totalMicroSec/1000000, sourceIds[i], (int)(pktRate/1000), (int)(dataRate),
                        droppedCount, currDroppedPackets[i]);
                fflush(fp);
            }
#endif


        }
        printf("     Combined Bufs:    %u\n\n", stats[0]->combinedBuffers);

        t1 = t2;
    }

    fclose(fp);
    return (NULL);
}



// Arg to pass to buffer reassembly thread
typedef struct threadArg_t {
    /** Supply of buffers for holding reassembled data. */
    std::shared_ptr<ejfat::Supplier<BufferItem>> bufSupply;

    /** Supply of sstructures holding UDP packets. */
    std::shared_ptr<ejfat::Supplier<PacketsItem>> pktSupply;

    /** Byte size of buffers contained in supply. */
    int bufferSize;

    int sourceCount;

    bool debug;
    bool dump;

    // shared ptr of stats
    std::shared_ptr<packetRecvStats> stats;

    uint64_t expectedTick;
    uint32_t tickPrescale;
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
// Thread to parse stored packets for 1 SOURCE ID / ENTROPY VALUE
static void *threadAssemble(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    std::shared_ptr<ejfat::Supplier<BufferItem>>  bufSupply = tArg->bufSupply;
    std::shared_ptr<ejfat::Supplier<PacketsItem>> pktSupply = tArg->pktSupply;
    bool dumpBufs = tArg->dump;
    bool debug    = tArg->debug;
    auto stats    = tArg->stats;

    int core      = tArg->core;
    int sourceCount = tArg->sourceCount;
    uint32_t tickPrescale = tArg->tickPrescale;

    // Track cpu by calling sched_getcpu roughly once per sec or 2
    int cpuLoops = 2000;
    int loopCount = cpuLoops;

    bool takeStats = stats != nullptr;

    std::shared_ptr<BufferItem>  bufItem;
    std::shared_ptr<PacketsItem> pktItem;
    std::shared_ptr<ByteBuffer>  buf;


#ifdef __linux__

    if (core > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run assemble reading thd for source " <<  sourceId << " on core " << core << "\n";
        CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }

        if (takeStats) {
            stats->cpuBuf = sched_getcpu();
        }
    }

#endif

    // The following maps are very small.
    // No need to get fancy here as searches will be brief.

    // One map for each source: key = source id, val = pointer to map
    std::unordered_map<int, std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *> maps;

    // For a single source, a map keeps buffers being worked on: key = tick, val = buffer
    std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> buffers;

    std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *pmap;

    // One tick for each source: key = source id, val = largest tick value to be reassembled
    std::unordered_map<int, uint64_t> largestSavedTick;


    while (true) {

        //-------------------------------------------------------------
        // Get item containing packets previously read in by another thd
        //-------------------------------------------------------------
        pktItem = pktSupply->consumerGet();
        size_t packetCount = pktItem->getPacketsFilled();

        int prevSrcId = -1;
        uint64_t prevTick = UINT64_MAX;
        pmap = nullptr;


        for (int i = 0; i < packetCount; i++) {
            reHeader *hdr = pktItem->getHeader(i);

            int srcId = hdr->dataId;
            if (srcId != prevSrcId) {
                // Switching to a different data source ...

                // Get the right map if there is one, else make one
                pmap = maps[srcId];
                if (pmap == nullptr) {
                    maps[srcId] = pmap = new std::unordered_map<uint64_t, std::shared_ptr<BufferItem>>();
                }

                // Get buffer in which to reassemble
                bufItem = (*pmap)[hdr->tick];
                // If there is no buffer existing for this tick, get one
                if (bufItem == nullptr) {
                    // This call gets a reset bufItem
                    (*pmap)[hdr->tick] = bufItem = bufSupply->get();
                }
            }
            else if (hdr->tick != prevTick) {
                // Same source as last pkt, but if NOT the same tick ...
                bufItem = (*pmap)[hdr->tick];
                if (bufItem == nullptr) {
                    (*pmap)[hdr->tick] = bufItem = bufSupply->get();
                }
            }

            // Keep track so if next packet is same source/tick, we don't have to look it up
            prevSrcId = srcId;
            prevTick  = hdr->tick;

            // We are using the ability of the BufferItem to store a user's long.
            // Use it store how many bytes we've copied into it so far.
            // This will enable us to tell if we're done w/ reassembly.
            int64_t bytesSoFar = bufItem->getUserLong();
            int64_t dataLen = pktItem->getPacket(i)->msg_len - HEADER_BYTES;

            // Do we have memory to store entire buf? If not, expand.
            if (hdr->length > bufItem->getBuffer()->capacity()) {
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

            // Copy things into buf
            memcpy(bufItem->getBuffer()->array() + hdr->offset,
                   pktItem->getPacket(i)->msg_hdr.msg_iov[1].iov_base,
                   dataLen);

            // Keep track of how much we've written so far
            bytesSoFar += dataLen;
            bufItem->setUserLong(bytesSoFar);

            // If we've written all data to this buf ...
            if (bytesSoFar >= hdr->length) {
                // Done with this buffer, so set its limit to proper value
                bufItem->getBuffer()->limit(bytesSoFar);

                // Clear buffer from local map
                pmap->erase(hdr->tick);

                if (takeStats) {
                    stats->acceptedBytes += hdr->length;
                    stats->builtBuffers++;
                }

                // Track the biggest tick to be saved from this source
                if (hdr->tick > largestSavedTick[srcId]) {
                    largestSavedTick[srcId] = hdr->tick;
                }

                // Pass buffer to waiting consumer or just dump it
                if (dumpBufs) {
                    bufSupply->release(bufItem);
                }
                else {
                    bufSupply->publish(bufItem);
                }
            }
        }

        // If here, we've gone thru a bundle of UDP packets,
        // time to get the next bundle.
        pktSupply->release(pktItem);

        // May want to do some house cleaning at this point.
        // There may be missing packets which have kept some ticks from some sources
        // from being completely reassembled and need to cleared out.
        // So, for each source, take biggest tick to be saved and remove all existing ticks
        // less than 2*tickPrescale and still being constructed. Keep stats.

        // Iterate over map
        for (const auto& n : largestSavedTick) {
            int source = n.first;
            uint64_t tick = n.second;

            // Compare this tick with the ticks in maps[source] and remove if too old
            std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *pm = maps[source];
            if (pm != nullptr) {
                for (const auto &nn: *pm) {
                    uint64_t tck = nn.first;
                    auto bItem = nn.second;
                    if (tck < tick - 2 * tickPrescale) {
                        pm->erase(tck);
                        // Release resources here
                        // TODO: Can we release some and publish others?????
                        // TODO: If not, we need to label bad buffers!!!!!!!
                        bufSupply->release(bufItem);
                    }
                }
            }
        }
        

//        volatile uint64_t droppedPackets;   /**< Number of dropped packets. This cannot be known exactly, only estimate. */
//        volatile uint64_t acceptedPackets;  /**< Number of packets successfully read. */
//        volatile uint64_t discardedPackets; /**< Number of bytes discarded because reassembly was impossible. */
//
//        volatile uint64_t droppedBytes;     /**< Number of bytes dropped. */
//        volatile uint64_t acceptedBytes;    /**< Number of bytes successfully read, NOT including RE header. */
//        volatile uint64_t discardedBytes;   /**< Number of bytes dropped. */
//
//        volatile uint32_t droppedBuffers;    /**< Number of ticks/buffers for which no packets showed up. */
//        volatile uint32_t discardedBuffers;  /**< Number of ticks/buffers discarded. */
//        volatile uint32_t builtBuffers;      /**< Number of ticks/buffers fully reassembled. */


        // Finish up some stats
        if (takeStats) {
#ifdef __linux__
            // If core hasn't been pinned, track it
            if ((core < 0) && (loopCount-- < 1)) {
                stats->cpuBuf = sched_getcpu();
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
static void *threadReadPackets(void *arg) {

    threadArg *tArg = (threadArg *) arg;
    std::shared_ptr<ejfat::BufferSupply> pktSupply = tArg->pktSupply;
    int udpSocket         = tArg->udpSocket;
    bool debug            = tArg->debug;
    auto stats            = tArg->stats;
    uint64_t expectedTick = tArg->expectedTick;
    uint32_t tickPrescale = tArg->tickPrescale;
    int sourceId          = tArg->sourceId;
    int core              = tArg->core;
    bool knowExpectedTick = expectedTick != 0xffffffffffffffffL;


    // Track cpu by calling sched_getcpu roughly once per sec or 2
    int cpuLoops = 10000;
    int loopCount = cpuLoops;

    // Statistics
    int64_t  prevTick = -1;
    uint64_t packetTick;
    uint32_t sequence, prevSequence = 0, expectedSequence = 0;
    bool dumpTick = false;
    bool takeStats = stats == nullptr ? false : true;
    bool packetFirst, packetLast;
    int  bytesRead, baseIndex;
    char *buffer;
    char *writeDataTo;
    ssize_t nBytes;

    bool getAnotherBuf = true;
    // Each pktBuffer is 90KB which contains up to 10 Jumbo packets
    int32_t chunk = 10;
    int bufIndex = chunk;
    uint32_t *intArray;

    std::shared_ptr<BufferSupplyItem> item;

#ifdef __linux__

    if (core > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run pkt reading thd for source " <<  sourceId << " on core " << core << "\n";
        CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }

        if (takeStats) {
            stats->cpuPkt = sched_getcpu();
        }
    }

#endif

    while (true) {
        // Grab an empty buffer from ByteBufferSupply which will hold up to 10 Jumbo packets
        item = pktSupply->get();
        // Track where to put packet in this buffer, start at beginning
        bufIndex = 0;
        // Get reference to item's byte array
        buffer = reinterpret_cast<char *>(item->getClearedBuffer()->array());
        intArray = (uint32_t *) (item->getUserInts());

        // There's room for chunk (10) packets
        while (bufIndex < chunk) {

            writeDataTo =  buffer + (bufIndex * 9000);

            // Read packet into place in buffer (1 packet every 9k bytes)
            bytesRead = recvfrom(udpSocket, writeDataTo, 9000, 0, NULL, NULL);
            if (bytesRead < 0) {
                if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));
                exit(-1);
            }
            nBytes = bytesRead - HEADER_BYTES;

            // TODO: What if we get a zero-length packet???

            // Parse header, storing everything for later convenience.
            // We've set "item" to have user int array of size 60 so we
            // can put 6 ints from each of the 10 packets in it.
            baseIndex = 6*bufIndex;
            parseReHeaderFast(writeDataTo, intArray, baseIndex, &packetTick);

            // store extra info in item's int array
            intArray[baseIndex + 5] = nBytes;

            // useful quantities
            sequence    = intArray[baseIndex + 2];
            packetFirst = intArray[baseIndex + 0] ? true : false;
            packetLast  = intArray[baseIndex + 1] ? true : false;
//            fprintf(stderr, "parse RE header, base index = %d, seq = %u, first = %s, last = %s\n",
//                    baseIndex, sequence, btoa(packetFirst), btoa(packetLast));

            // This if-else statement is what enables the packet reading/parsing to keep
            // up an input rate that is too high (causing dropped packets) and still salvage
            // some of what is coming in.
            if (packetTick != prevTick) {
                // If we're here, either we've just read the very first legitimate packet,
                // or we've dropped some packets and advanced to another tick in the process.
                expectedSequence = 0;

                if (sequence != 0) {
                    // Already have trouble, looks like we dropped the first packet of a tick,
                    // and possibly others after it.
                    // So go ahead and dump the rest of the tick in an effort to keep up.
                    //printf("Skip %hu, %llu - %u\n", packetDataId, packetTick, sequence);
                    //printf("S %llu - %u\n", packetTick, sequence);
                    //getAnotherBuf = false;
                    continue;
                }

                // Dump everything we saved from previous tick.
                dumpTick = false;
            }
            // Same tick as last packet
            else {
                if (dumpTick || (std::abs((int) (sequence - prevSequence)) > 1)) {
                    // If here, the sequence hopped by at least 2,
                    // probably dropped at least 1,
                    // so drop rest of packets for record.
                    // This branch of the "if" will no longer
                    // be executed once the next record shows up.
                    expectedSequence = 0;
                    dumpTick = true;
                    prevSequence = sequence;
                    //printf("Dump %hu, %llu - %u\n", packetDataId, packetTick, sequence);
                    //printf("D %llu - %u\n", packetTick, sequence);
                    //getAnotherBuf = false;
                    continue;
                }
            }

            if (packetLast) {
                if (takeStats) {
                    if (knowExpectedTick) {
                        int64_t diff = packetTick - expectedTick;
                        if (diff % tickPrescale != 0) {
                            // Error in the way we set things up
                            // This should always be 0.
                            fprintf(stderr, "    Using wrong value for tick prescale, %u\n", tickPrescale);
                            exit(-1);
                        }
                        else {
                            stats->droppedTicks += diff / tickPrescale;
                            //printf("Dropped %u, dif %lld, t %llu x %llu \n", stats->droppedTicks, diff, packetTick, expectedTick);
                        }
                    }

                    // Total microsec to read buffer
                    stats->acceptedBytes += nBytes;
                    stats->acceptedPackets += sequence + 1;
                    stats->droppedPackets = stats->droppedTicks * (sequence + 1);
                    //printf("Pkts %llu, dropP %llu, dropT %u, t %llu x %llu \n",
                    //             stats->acceptedPackets, stats->droppedPackets, stats->droppedTicks, packetTick, expectedTick);

#ifdef __linux__
                    // If core hasn't been pinned, track it
                    if ((core < 0) && (loopCount-- < 1)) {
                        stats->cpuPkt = sched_getcpu();
                        loopCount = cpuLoops;
    //printf("Read pkt thd: get CPU\n");
                    }
#endif
                }

                expectedTick = packetTick + tickPrescale;
                prevTick = packetTick;
                prevSequence = sequence;
                // Send data to assembly thread if this is the last packet of a buffer.
                //
                // This is done to avoid the general problem of hanging on recvfrom
                // because the very last packet was just read and we have not yet released
                // the buffer in placed in.
                // If it hangs now, that's only because the last packet was dropped.
                break;
            }

            if (takeStats) {
                stats->acceptedBytes += nBytes;
            }

            prevTick = packetTick;
            prevSequence = sequence;
            bufIndex++;
        }

        // Send buffer containing packets to assembly thread
        pktSupply->publish(item);
    }

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

    for (int i=0; i < 128; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, &startingCore, &startingBufCore, sourceIds,
              &startingPort, &debug, &useIPv6, &keepStats, &dumpBufs, listeningAddr, filename);

    pinCores = startingCore >= 0 ? true : false;
    pinBufCores = startingBufCore >= 0 ? true : false;

    for (int i=0; i < 128; i++) {
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
        sourceCount  = 1;
        std::cerr << "Defaulting to (single) source id = 0" << std::endl;
    }

    // Place to store all supplies (of which each contained item is a reassembled buffer from 1 source)
    std::shared_ptr<ejfat::BufferSupply> supplies[sourceCount];


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

//    // Array of stat object for handing to threads which read packets and assemble buffers
//    std::shared_ptr<packetRecvStats> stats[sourceCount];
//    for (int i=0; i < sourceCount; i++) {
//        if (keepStats) {
//            stats[i] = std::make_shared<packetRecvStats>();
//            clearStats(stats[i]);
//        }
//        else {
//            stats[i] = nullptr;
//        }
//    }

    std::shared_ptr<packetRecvStats> stats = nullptr;
    if (keepStats) {
        stats = std::make_shared<packetRecvStats>();
        clearStats(stats);
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
            return -1;
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);
        if (debug) fprintf(stderr, "UDP socket for source %d recv buffer = %d bytes\n", sourceIds[i], recvBufSize);

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

        fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes\n", (startingPort + i), recvBufSize);

    }


    //---------------------------------------------------
    // Supply in which each item holds 200 UDP packets
    // and parsed header info.
    //---------------------------------------------------
    int pktRingSize = 32;
    PacketsItem::setEventFactorySettings(200);
    std::shared_ptr<ejfat::Supplier<PacketsItem>> pktSupply =
            std::make_shared<ejfat::Supplier<PacketsItem>>(pktRingSize, true);


    //---------------------------------------------------
    // Supply in which each buf will hold reconstructed buffer.
    // Make these buffers sized as given on command line (100kB default) and expand as necessary.
    //---------------------------------------------------
    int ringSize = 1024;
    BufferItem::setEventFactorySettings(ByteOrder::ENDIAN_LOCAL, bufSize);
    std::shared_ptr<ejfat::Supplier<BufferItem>> supply =
            std::make_shared<ejfat::Supplier<BufferItem>>(ringSize, true);


    //---------------------------------------------------
    // Start thread to assemble buffers of packets from this source
    //---------------------------------------------------
    threadArg *tArg2 = (threadArg *) calloc(1, sizeof(threadArg));
    if (tArg2 == NULL) {
        fprintf(stderr, "\n ******* ran out of memory\n\n");
        exit(1);
    }

    tArg2->bufSupply  = supply;
    tArg2->pktSupply  = pktSupply;
    tArg2->stats      = stats;
    tArg2->dump       = dumpBufs;
    tArg2->debug      = debug;
    tArg2->sourceCount = sourceCount;
    if (pinBufCores) {
        tArg2->core = startingBufCore;
    }
    else {
        tArg2->core = -1;
    }

    pthread_t thd1;
    status = pthread_create(&thd1, NULL, threadAssemble, (void *) tArg2);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    //---------------------------------------------------
    // Thread to empty reassembled buffers from ring
    //---------------------------------------------------
    threadArg *tArg = (threadArg *)calloc(1, sizeof(threadArg));
    if (tArg == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }

    tArg2->bufSupply   = supply;
    tArg->stats        = stats;
    tArg->sourceId     = sourceIds[i];
    tArg->expectedTick = 0;
    tArg->tickPrescale = 1;
    if (pinCores) {
        tArg->core = startingCore;
    }
    else {
        tArg->core = -1;
    }

    pthread_t thd;
    status = pthread_create(&thd, NULL, threadReadPackets, (void *) tArg);
    if (status != 0) {
        fprintf(stderr, "Error creating thread for reading pkts\n");
        return -1;
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

again:

    while (true) {

        // Read all UDP packets here
        item = pktSupply->get();

        // Collect packets until full or timeout expires.
        // How much time to collect 200 packets @ 100Gb (12.5GB) / sec ? ---> (200*9000) / 12.5e9 = .14 millisec
        // @ 1MB/sec ---> 1.8 sec
        int packetCount = recvmmsg(udpSocket, item->getPackets(), item->getMaxPacketCount(), 0, &timeout);
        if (packetCount == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // if timeout, go 'round again
                goto again;
            }
            fprintf(stderr, "\n ******* error receiving UDP packets\n\n");
            exit (-1);
        }


//        // Allocate array of mmsghdr structs each containing a single UDP packet
//        // (spread over 2 buffers, 1 for hdr, 1 for data).
//        packets = new mmsghdr[maxPktCount];
//
//        for (int i = 0; i < maxPktCount; i++) {
//            packets[i].msg_hdr.msg_iov = new struct iovec[2];
//            packets[i].msg_hdr.msg_iovlen = 2;
//
//            // Where RE header goes
//            packets[i].msg_hdr.msg_iov[0].iov_base = new uint8_t[HEADER_BYTES];
//            packets[i].msg_hdr.msg_iov[0].iov_len = HEADER_BYTES;
//
//            // Where data goes (can hold jumbo frame)
//            packets[i].msg_hdr.msg_iov[1].iov_base = new uint8_t[9000];
//            packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
//        }

        // Since all the packets have been read in, parse the headers

        // TODO: We could shift this code to the reassembly thread

        for (int i=0; i < packetCount; i++) {
            unsigned int dataLen = item->getPacket(i)->msg_len;
            if (dataLen < 20) {
                // didn't read in enough data for even the header, ERROR
            }
            ejfat::parseReHeader(reinterpret_cast<char *>(item->getPacket(i)->msg_hdr.msg_iov[0].iov_base),
                                 item->getHeader(i));
        }

        // Send data to reassembly thread for consumption
        pktSupply->publish(item);
    }

    return 0;
}

