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
 * <p>Receive generated data sent by packetBlasterFat.c program.
 * This assumes there is an emulator or FPGA between this and the sending program.
 * In order to get a handle on dropped packets, it's necessary to know exactly how
 * many packets were sent. This is done by sending the exact same size buffer over
 * and over. It's also necessary to have the tick # changed in set increments.
 * Otherwise, it's impossible to know what was lost.</p>
 *
 * In conjunction with packetBlasterFat, this program seeks to increase the bandwidth
 * between a UDP sender and receiver by using 2 UDP sockets. Buffers sent by the blaster
 * alternate in round-robin form from one socket to the other.
 * This receiver will read and assemble buffers in 1 thread for each socket.
 * These are used by a 3rd thread which alternates getting buffers between both
 * receiving threads.
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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <starting listening UDP port (increment for each socket)>]",
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
                      int* bufSize, int *recvBufSize, int *tickPrescale,
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

    // variables for the stats of combined inputs
    uint64_t droppedPacketsCombined, totalDroppedPacketsCombined;
    double   dataRateCombined, dataAvgRateCombined, pktRateCombined, pktAvgRateCombined;

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
    fprintf(stderr, "stats, source count = %d\n", sourceCount);
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
                stats[i]->droppedTicks = 0;
                stats[i]->builtBuffers = 0;
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

        droppedPacketsCombined = totalDroppedPacketsCombined = 0;
        dataRateCombined = dataAvgRateCombined = pktRateCombined = pktAvgRateCombined = 0;

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

            pktRateCombined             += pktRate;
            pktAvgRateCombined          += pktAvgRate;
            dataRateCombined            += dataRate;
            dataAvgRateCombined         += dataAvgRate;
            droppedPacketsCombined      += droppedCount;
            totalDroppedPacketsCombined += currDroppedPackets[i];

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

        printf("Combined:  Pkts>  %3.4g Hz,  %3.4g Avg  <Data>  %3.4g MB/s,  %3.4g Avg  <Drops>  %" PRIu64 "   %" PRIu64 " (total)\n\n",
               pktRateCombined, pktAvgRateCombined,
               dataRateCombined, dataAvgRateCombined,
               droppedPacketsCombined, totalDroppedPacketsCombined);

        //printf("     Combined Bufs:    %u\n\n", stats[0]->combinedBuffers);

        t1 = t2;
    }

    fclose(fp);
    return (NULL);
}



// Arg to pass to buffer reassembly thread
typedef struct threadArg_t {

    /** Supply holding buffers of single UDP packets. */
    std::shared_ptr<ejfat::BufferSupply> bufSupply;

    /** Byte size of buffers contained in supply. */
    int bufferSize;

    /** For reading packet thread. */
    int udpSocket;

    /** For reading packet thread. */
    uint16_t sourceId;

    bool debug;
    bool dump;

    std::shared_ptr<packetRecvStats> stats;

    int streams;
    int place;

    uint64_t tick;
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
// Thread to parse stored packets for 1 SOURCE ID / ENTROPY VALUE
static void *threadReassemble(void *arg) {

    threadArg *tArg = (threadArg *) arg;
    std::shared_ptr<ejfat::BufferSupply> bufSupply = tArg->bufSupply;
    bool dumpBufs    = tArg->dump;
    bool debug       = tArg->debug;
    auto stats       = tArg->stats;
    uint16_t dataId  = tArg->sourceId;
    int core         = tArg->core;
    int udpSocket    = tArg->udpSocket;
    int streams      = tArg->streams;
    int place        = tArg->place;
    uint64_t tick    = tArg->tick;
    int tickPrescale = tArg->tickPrescale;

    bool takeStats = stats != nullptr;

    // Track cpu by calling sched_getcpu roughly once per sec or 2
    int cpuLoops = 2000;
    int loopCount = cpuLoops;
    ssize_t nBytes, totalPackets = 0, totalBytes = 0;
    char *buffer;

#ifdef __linux__

    if (core > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run assemble reading thd for source " <<  dataId << " on core " << core << std::endl;
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

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;

    static std::atomic<uint32_t> dropped;

    std::shared_ptr<BufferSupplyItem> item;
    std::shared_ptr<ByteBuffer> itemBuf;


    // Statistics
    dropped.store(0);

    while (true) {

        item = bufSupply->get();
        // Get reference to item's ByteBuffer object
        itemBuf = item->getClearedBuffer();
        // Get reference to item's byte array (underlying itemBuf)
        buffer = reinterpret_cast<char *>(itemBuf->array());
        size_t bufCapacity = itemBuf->capacity();

        // Fill with data
        nBytes = getCompletePacketizedBuffer(buffer, bufCapacity, udpSocket,
                                             debug, &tick, &dataId, stats,
                                             tickPrescale, outOfOrderPackets);
        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d)\n", (int)bufCapacity);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
            }
            return (0);
        }

        // Place it back into supply for consumer
        if (dumpBufs) {
            bufSupply->release(item);
        }
        else {
            bufSupply->publish(item);
        }

        totalBytes += nBytes;

        if (takeStats) {
            totalPackets += stats->acceptedPackets;
            dropped += stats->droppedPackets;
        }

        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale * streams;

#ifdef __linux__
        if (loopCount-- < 1) {
            stats->cpuBuf = sched_getcpu();
            loopCount = cpuLoops;
        }
#endif

    }

    return nullptr;
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


#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

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

    std::shared_ptr<ejfat::BufferSupply> supplies[sourceCount];
    int udpSockets[sourceCount];

    //-----------------------------------
    // For each data source ...
    //-----------------------------------
    for (int i=0; i < sourceCount; i++) {

        //---------------------------------------------------
        // Create socket to read data from source ID
        //---------------------------------------------------

        if (useIPv6) {
            struct sockaddr_in6 serverAddr6{};

            // Create IPv6 UDP socket
            if ((udpSockets[i] = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                return -1;
            }

            // Set & read back UDP receive buffer size
            socklen_t size = sizeof(int);
            setsockopt(udpSockets[i], SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
            recvBufSize = 0;
            getsockopt(udpSockets[i], SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);
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
            int err = bind(udpSockets[i], (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                return -1;
            }
        }
        else {
            // Create UDP socket
            if ((udpSockets[i] = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                return -1;
            }

            // Set & read back UDP receive buffer size
            socklen_t size = sizeof(int);
            setsockopt(udpSockets[i], SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
            recvBufSize = 0;
            getsockopt(udpSockets[i], SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);

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
            int err = bind(udpSockets[i], (struct sockaddr *) &serverAddr, sizeof(serverAddr));
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
        int bufRingSize = 1024;
        std::shared_ptr<ejfat::BufferSupply> bufSupply =
                std::make_shared<ejfat::BufferSupply>(bufRingSize, bufSize, ByteOrder::ENDIAN_LOCAL, true);
        supplies[i] = bufSupply;

        //---------------------------------------------------
        // Start thread to assemble buffers of packets from this source
        //---------------------------------------------------
        threadArg *tArg2 = (threadArg *) calloc(1, sizeof(threadArg));
        if (tArg2 == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        tArg2->bufSupply    = bufSupply;
        tArg2->udpSocket    = udpSockets[i];
        tArg2->stats        = stats[i];
        tArg2->sourceId     = sourceIds[i];
        tArg2->dump         = dumpBufs;
        tArg2->debug        = debug;
        tArg2->tickPrescale = tickPrescale;
        tArg2->streams      = sourceCount;
        tArg2->place        = i;
        tArg2->tick         = i*tickPrescale;

        if (pinBufCores) {
            tArg2->core = startingBufCore + i;
        }
        else {
            tArg2->core = -1;
        }

        pthread_t thd1;
        status = pthread_create(&thd1, NULL, threadReassemble, (void *) tArg2);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
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

        targ->stats = stats;
        targ->sourceIds = sourceIds;
        targ->sourceCount = sourceCount;

        pthread_t thd2;
        status = pthread_create(&thd2, NULL, rateThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    char *buffer;
    std::shared_ptr<ByteBuffer> itemBuf;
    std::shared_ptr<BufferSupplyItem> item;

    while (true) {

        for (int i=0; i < sourceCount; i++) {

            if (dumpBufs) {
                // sleep
                sleep(1);
            }
            else {
                //------------------------------------------------------
                // Get reassembled buffer
                //------------------------------------------------------
                auto supply = supplies[i];
                item = supply->consumerGet();
                // Get reference to item's ByteBuffer object
                itemBuf = item->getBuffer();
                // Get reference to item's byte array (underlying itemBuf)
                buffer = reinterpret_cast<char *>(itemBuf->array());
                size_t bufCapacity = itemBuf->capacity();

                //------------------------------------------------------
                // Do something with the data here
                //------------------------------------------------------
                // long packetTick = item->getUserLong();
                // fprintf(stderr, "s%d/%ld ", i, packetTick);

                //------------------------------------------------------
                // Release buffer back to supply for reuse
                //------------------------------------------------------
                supply->release(item);
            }

        }

        if (keepStats) {
            stats[0]->combinedBuffers += sourceCount;
        }

        //fprintf(stderr, "\n");
    }


    return 0;
}

