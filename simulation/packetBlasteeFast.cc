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
 * <p>Receive generated data sent by packetBlaster.c program(s).
 * This assumes there is an emulator or FPGA between this and the sending program.
 * In order to get a handle on dropped packets, it's necessary to know exactly how
 * many packets were sent. This is done by sending the exact same size buffer over
 * and over. It's also necessary to have the tick # changed in set increments.
 * Otherwise, it's impossible to know what was lost.</p>
 *
 * <p>This differs from packetBlaseeBuf. That program reads get a 9k buf from supply,
 * fills it by reading UDP packet into it, then puts it back. A second thread
 * gets a big buf from another supply, then gets enough pkts from the packet supply
 * to build a full buffer there.</p>
 *
 * This program, on the other hand, has a supply of 90kB buffers in which 10 packets
 * can be stored. If a final packet of a buf is received it will end the accumulation
 * in the 90KB buf. A second thd gets a big buf like before, then gets enough pkts from
 * the packet supply of 90kB bufs to build a full buffer there. So it tries to do
 * copies of packets into fewer buffers to lower overhead in the use of the buffer supplies
 * (disruptor rings). We'll see if this makes any difference.
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

#include "BufferSupply.h"
#include "BufferSupplyItem.h"
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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <starting listening UDP port (increment for each source)>]",
            "        [-b <internal buffer byte size>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-f <file for stats>]",
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
 * @param listenAddr    filled with IP address to listen on.
 * @param filename      filled with name of file in which to write stats.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      int *core, int *coreBuf, int *sourceIds, uint16_t* port,
                      bool *debug, bool *useIPv6, bool *keepStats,
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
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = %llu\n",
                       sourceIds[i], pktRate, pktAvgRate, droppedCount);
            }

            // Actual Data rates (no header info)
            dataRate    = ((double) byteCount) / microSec;
            dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSec;

#ifdef __linux__
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, pkt cpu %d, buf cpu %d, bufs %u\n",
                   dataRate, dataAvgRate, stats[i]->cpuPkt, stats[i]->cpuBuf, stats[i]->builtBuffers);

            if (writeToFile) {
                fprintf(fp, "%lld,%d,%d,%d,%lld,%lld,%d,%d\n", totalMicroSec/1000000, sourceIds[i], (int)(pktRate/1000), (int)(dataRate),
                        droppedCount, currDroppedPackets[i], stats[i]->cpuPkt, stats[i]->cpuBuf);
                fflush(fp);
            }
#else
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, bufs %u\n",
                   dataRate, dataAvgRate, stats[i]->builtBuffers);

            if (writeToFile) {
                fprintf(fp, "%lld,%d,%d,%d,%lld,%lld\n", totalMicroSec/1000000, sourceIds[i], (int)(pktRate/1000), (int)(dataRate),
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
    /** Supply holding buffers of reassembled data. */
    std::shared_ptr<ejfat::BufferSupply> supply;
    /** Supply holding buffers of single UDP packets. */
    std::shared_ptr<ejfat::BufferSupply> pktSupply;
    /** Byte size of buffers contained in supply. */
    int bufferSize;

    /** For reading packet thread. */
    int udpSocket;

    /** For reading packet thread. */
    int sourceId;

    bool debug;

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
    std::shared_ptr<ejfat::BufferSupply> supply = tArg->supply;
    std::shared_ptr<ejfat::BufferSupply> pktSupply = tArg->pktSupply;
    bool debug   = tArg->debug;
    auto stats   = tArg->stats;
    int sourceId = tArg->sourceId;
    int core     = tArg->core;

    // Track cpu by calling sched_getcpu roughly once per sec or 2
    int cpuLoops = 2000;
    int loopCount = cpuLoops;

    uint64_t packetTick;
    uint32_t sequence, expectedSequence = 0;

    bool packetFirst, packetLast;
    bool takeStats = stats != nullptr;
    bool getMorePkts = true;

    int  nBytes, bufIndex, baseIndex, chunk = 10;
    ssize_t totalBytesRead = 0;
    char *putDataAt;
    char *getDataFrom;
    size_t remainingLen;
    uint32_t *intArray;

    std::shared_ptr<BufferSupplyItem> item;
    std::shared_ptr<BufferSupplyItem> pktItem;
    std::shared_ptr<ByteBuffer> itemBuf;

    char *buffer;
    char *pktBuffer;


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

    while (true) {

        expectedSequence = 0;
        takeStats = stats != nullptr;
        totalBytesRead = 0;

        //------------------------------------------------------
        // Get item (new buf) from supply for reassembled bufs
        //------------------------------------------------------
        item = supply->get();
        // Get reference to item's ByteBuffer object
        itemBuf = item->getClearedBuffer();
        // Get reference to item's byte array (underlying itemBuf)
        buffer = reinterpret_cast<char *>(itemBuf->array());
        size_t bufCapacity = itemBuf->capacity();

        while (true) {

            //-------------------------------------------------------------
            // Get item contained packets previously read in by another thd
            //-------------------------------------------------------------
            if (getMorePkts) {
                pktItem = pktSupply->consumerGet();
                // Get reference to item's byte array
                getDataFrom = pktBuffer = reinterpret_cast<char *>(pktItem->getBuffer()->array());
                // Get the RE headers data (stored in item), 6 ints for each pkt
                intArray = (uint32_t *) pktItem->getUserInts();
                // There are up to chunk (10) packets in this buffer, one every 9000 bytes, start with first
                bufIndex = 0;
                getMorePkts = false;
            }

            baseIndex = 6*bufIndex;

            // Extract header data previously stored in array by read pkt thread
            packetFirst  = intArray[baseIndex] ? true : false;
            packetLast   = intArray[baseIndex + 1] ? true : false;
            sequence     = intArray[baseIndex + 2];
            nBytes       = (int)intArray[baseIndex + 5];
            packetTick   = (uint64_t) ((uint64_t)(intArray[baseIndex + 3]) << 32 | intArray[baseIndex + 4]);
//            fprintf(stderr, "assem: bufIndex = %d, read nbytes = %d, pktFrst = %s, seq = %u, at %p\n", bufIndex, nBytes,
//                               btoa(packetFirst), sequence, &intArray[baseIndex]);

            if (packetFirst) {
                putDataAt = buffer;
                remainingLen = bufCapacity;
                totalBytesRead = 0;
                expectedSequence = 0;
            }

            // Make sure enough room in buffer.
            // If the command line's -b option properly set the buffer sizes,
            // they should never have to be expanded!
            if (remainingLen - nBytes < 0) {
                // Preserves existing data up to limit() while doubling underlying array
                item->expandBuffer(2*(bufCapacity + nBytes));

                // underlying array has changed
                buffer = reinterpret_cast<char *>(itemBuf->array());

                putDataAt = buffer + totalBytesRead;
                bufCapacity = itemBuf->capacity();
                remainingLen = bufCapacity - totalBytesRead;
            }

            // Copy data into reassembly buffer
            memcpy(putDataAt, getDataFrom + HEADER_BYTES, nBytes);

            // If we went thru all chunk pkts stored in pktBuffer or hit the last pkt of the buffer,
            // we know it's the last pkt, so move on
            if ((++bufIndex == chunk) || packetLast) {
                // Release packet buffer back to supply for reuse
                pktSupply->release(pktItem);
                getMorePkts = true;
            }
            else {
                // Packets are stored every 9k bytes
                getDataFrom = pktBuffer + bufIndex * 9000;
            }

            putDataAt += nBytes;
            remainingLen -= nBytes;
            totalBytesRead += nBytes;

            // Keep track of limit by hand since we're using underlying array directly.
            // This will ensure copying of data when expanding buffer (see above).
            itemBuf->limit(totalBytesRead);

            if (debug)
                fprintf(stderr, "Received %d data bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
                        nBytes, sequence, btoa(packetLast), btoa(sequence == 0));

            // Check to see if packet is out-of-sequence
            if (sequence != expectedSequence) {
                if (debug) fprintf(stderr, "\n    Got seq %u, expecting %u, quit program\n", sequence, expectedSequence);
                exit(-1);
            }

            // Get ready to look for next in sequence
            expectedSequence++;

            // Check RE header consistency
            if (sequence == 0) {
                if (!packetFirst) {
                    fprintf(stderr, "Bad RE header, first bit was NOT set on seq = 0\n");
                    exit(-1);
                }
            }
            else if (packetFirst) {
                fprintf(stderr, "Bad RE header, first bit was set but seq > 0\n");
                exit(-1);
            }

            if (debug)
                fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s\n",
                        remainingLen, expectedSequence, btoa(packetFirst), btoa(packetLast));

            if (debug) fprintf(stderr, "assem: 1\n");
            // If very last packet, go to next reassembly buffer
            if (packetLast) {

                // Send the finished buffer to the next guy using circular buffer
                item->setUserLong(packetTick);
                supply->publish(item);

                // Finish up some stats
                if (takeStats) {
                    stats->builtBuffers++;
#ifdef __linux__
                    // If core hasn't been pinned, track it
                    if ((core < 0) && (loopCount-- < 1)) {
                        stats->cpuBuf = sched_getcpu();
                        loopCount = cpuLoops;
    //printf("Read pkt thd: get CPU\n");
                    }
#endif
                }
                break;
            }
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
    int bufSize = 100000;
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


    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);

    for (int i=0; i < 128; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, &startingCore, &startingBufCore, sourceIds,
              &startingPort, &debug, &useIPv6, &keepStats, listeningAddr, filename);

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

    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint16_t dataId;


    // Array of stat object for handing to threads which read packets and assemble buffers
    std::shared_ptr<packetRecvStats> stats[sourceCount];
    for (int i=0; i < sourceCount; i++) {
        if (keepStats) {
            stats[i] = std::make_shared<packetRecvStats>();
            clearStats(stats[i]);
        }
        else {
            stats[i] = nullptr;
        }
    }


    //-----------------------------------
    // For each data source ...
    //-----------------------------------
    for (int i=0; i < sourceCount; i++) {

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
            serverAddr6.sin6_port = htons(startingPort + i);
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
            serverAddr.sin_port = htons(startingPort + i);
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
        // Supply in which each buf holds 10 Jumbo packets (90kB).
        // Make this 1024 bufs * 90kB/buf = 92MB
        //---------------------------------------------------
        int pktRingSize = 1024;
        int pktRingBufSize = 90000;
        std::shared_ptr<ejfat::BufferSupply> pktSupply =
                std::make_shared<ejfat::BufferSupply>(pktRingSize, pktRingBufSize, ByteOrder::ENDIAN_LOCAL, true);

        //---------------------------------------------------
        // Supply in which each buf will hold reconstructed buffer.
        // Make these buffers sized as given on command line (100kB default) and expand as necessary.
        //---------------------------------------------------
        int ringSize = 1024;
        std::shared_ptr<ejfat::BufferSupply> supply =
                std::make_shared<ejfat::BufferSupply>(ringSize, bufSize, ByteOrder::ENDIAN_LOCAL, true);
        supplies[i] = supply;

        //---------------------------------------------------
        // Start thread to assemble buffers of packets from this source
        //---------------------------------------------------
        threadArg *tArg2 = (threadArg *) calloc(1, sizeof(threadArg));
        if (tArg2 == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        tArg2->supply     = supply;
        tArg2->pktSupply  = pktSupply;
        tArg2->stats      = stats[i];
        tArg2->sourceId   = sourceIds[i];
        tArg2->debug      = debug;
        if (pinBufCores) {
            tArg2->core = startingBufCore + i;
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
        // Start thread to read packets from this source
        //---------------------------------------------------
        threadArg *tArg = (threadArg *)calloc(1, sizeof(threadArg));
        if (tArg == nullptr) {
            fprintf(stderr, "out of mem\n");
            return -1;
        }

        tArg->udpSocket    = udpSocket;
        tArg->pktSupply    = pktSupply;
        tArg->stats        = stats[i];
        tArg->sourceId     = sourceIds[i];
        tArg->expectedTick = 0;
        tArg->tickPrescale = 1;
        if (pinCores) {
            tArg->core = startingCore + i;
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

    std::shared_ptr<BufferSupplyItem> item;
    std::shared_ptr<ByteBuffer> itemBuf;
    char *buffer;

    while (true) {

        // How to combine all sub buffers into one big one?
        // How do we know what size to make it?
        //    -  Start with an educated guess and allow for expansion

        // Note: in this blastee, buffers from different ticks would be combined since
        // the sources are not started at the exact same time

        for (int i=0; i < sourceCount; i++) {

            auto supply = supplies[i];

            //------------------------------------------------------
            // Get reassembled buffer
            //------------------------------------------------------
            item = supply->consumerGet();
            // Get reference to item's ByteBuffer object
            itemBuf = item->getBuffer();
            // Get reference to item's byte array (underlying itemBuf)
            buffer = reinterpret_cast<char *>(itemBuf->array());
            size_t bufCapacity = itemBuf->capacity();
//            long packetTick = item->getUserLong();
//fprintf(stderr, "s%d/%ld ", i, packetTick);

            // ETC, ETC

            // Release buffer back to supply for reuse
            supply->release(item);
        }
//fprintf(stderr, "\n");
        stats[0]->combinedBuffers++;
    }

    return 0;
}

