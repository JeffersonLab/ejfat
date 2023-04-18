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
 * This assumes there is an emulator or FPGA between this and the sending program.
 * Note that this program uses fast, non-locking ring buffers.
 * This program is intended to be a full-blown implementation of how to handle multiple
 * data sources coming into 1 socket and having it all sorted and assembled into separate
 * buffers which can then be accessed by the user.
 * </p>
 *
 * <p>
 * In this program, a single, main thread reads from a single socket in which packets from
 * multiple (16 max) data sources are arriving. There is a ring buffer with pre-allocated
 * structures in which to store the incoming packets. There are N (up to 3) additional
 * threads which read packets from that ring in order to do the
 * reassembly. Each of these reassembles the Nth numbered event with a different offset
 * so that each ends up reassembling different events.
 * There are also threads to do calculate rates and to read/dump reassembled buffers.
 * </p>
 * <p>
 * There are maps temporarily containing buffers in which to store reassembled data,
 * one for each incoming data source and specific tick.
 * The buffers come from a ring source much like the structs that hold packets also come from a (different) ring.
 * In the reassembly threads, packets are retrieved from the packet ring. These are placed in the proper buffer.
 * In each of these N threads there is one buffer source for each data source id.
 * A buffer for a specific source/tick may already exist from previous use
 * (if not a new buf is grabbed) and it's filled with corresponding data.
 * When a buffer is fully reassembled, it's put back into the buffer ring from which it came.
 * Note that while a buffer is being reassembled there is a place to store
 * it and find it according to its tick value (in unordered_map).
 * </p>
 * <p>
 * There are threads which pull off all reassembled buffers - one for each data source.
 * Also another thread to do stats.
 * There is some sensitivity to the number of packets in each PacketsItem. Best value seems to be around 20.
 * </p>
 *
 * <p>
 * So what happens when there are multiple sources? say 2 for sake of argument?
 * 1) If src1 and src2 are completely independent, use a separate copy of this receiving program for each src.
 * The treatment of data will be completely different for each anyway so this makes the most sense.
 * 2) If the sources need to be combined and processed together, this program can help.
 * Each receiving thread is looking for certain ticks. If there are 2 such threads, one looks for event ticks
 * and the other, odd. So pick the thread looking at even ticks. It knows how many sources are expected and
 * their values (given as cmd line args). One buffer is used to construct/contain data for one src and one tick.
 * </p>
 *
 * <p>
 * Look on Carl Timmer's Mac at /Users/timmer/DataGraphs/NumaNodes.xlsx in order see results
 * of a study of the ejfat nodes at Jefferson Lab, 100Gb network.
 * Use packetBlaster.cc to produce events.
 * To produce events at roughly 3 GB/s, use arg "-cores 60" where 60 can just as
 * easily be 0-63. To produce at roughly 3.4 GB/s, use cores 64-79, 88-127.
 * To produce at 4.7 GB/s, use cores 80-87. (Notices cores # start at 0).
 * This receiver will be able to receive all data sent at about
 * 3 GB/s, any more than that and it starts dropping packets.
 * To receive at that rate, the args "-pinRead 80 -pinCnt 5" must be used to specify the fastest
 * network cores for reading packets, and needs at least 5 of them to distribute the work.
 * Even then packets are occasionally dropped.
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
#include <unordered_map>
#include <errno.h>

#include "BufferItem.h"
#include "PacketsItemN.h"
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

// Max number of chars in a name
#define INPUT_LENGTH_MAX 256

// Max # of data input sources
#define MAX_SOURCES 16

// Max number of reassembly threads
#define MAX_RE_THREADS 3


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
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
            "        [-thds <# of reassembly threads, 6 max>]",
            "        [-pinRead <starting core # or read thd>]",
            "        [-pinCnt <# of cores for read thd>]",
            "        [-pinBuf <starting core #, 1 for each buf assembly thd>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with packetBlaster.\n");
    fprintf(stderr, "        It can receive from multiple data sources simultaneously.\n");
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
 * @param thdCnt        number of reassembly threads (6 max).
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
                      int *core, int *coreCnt, int *coreBuf, int *sourceIds,
                      int *thdCnt, uint16_t* port,
                      bool *debug, bool *useIPv6, bool *keepStats, bool *dump,
                      char *listenAddr, char *filename) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, NULL, 1},
                          {"ip6",  0, NULL, 2},
                          {"pinRead",  1, NULL, 3},
                          {"pinCnt",  1, NULL, 8},
                          {"pinBuf",  1, NULL, 4},
                          {"ids",  1, NULL, 5},
                          {"stats",  0, NULL, 6},
                          {"dump",  0, NULL, 7},
                          {"thds",  1, NULL, 9},
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

            case 8:
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


            case 6:
                // Keep stats
                *keepStats = true;
                break;

            case 7:
                // dump buffers without gathering into 1 thread
                *dump = true;
                break;

            case 9:
                // Number of reassembly thds
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0 && i_tmp <= MAX_RE_THREADS) {
                    *thdCnt = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -thds, 0 < # of thds <= %d)\n", MAX_RE_THREADS);
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

    int64_t byteCount, pktCount, bufCount;
    int64_t missingByteCount; // discarded + dropped

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
        fprintf(fp, "Sec,Source,PacketRate(kHz),DataRate(MB/s),Missing(bytes),TotalMissing(bytes),PktCpu,BufCpu\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate;
    int64_t microSec;
    struct timespec tEnd, t1;
    bool rollOver = false;

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

                currDiscardPkts[i]  = mapp[src]->discardedPackets = 0;
                currDiscardBytes[i] = mapp[src]->discardedBytes   = 0;
                currDiscardBufs[i]  = mapp[src]->discardedBuffers = 0;

                currDropBytes[i]    = mapp[src]->droppedBytes     = 0;

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

            prevDropBytes[i]  = currDropBytes[i];
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

        // Don't start calculating stats until data has come in for a full cycle.
        // Keep track of when that starts.
        for (int i=0; i < sourceCount; i++) {
            if (currTotalPkts[i] > 0) {
                dataArrived[i] = true;
            }
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
            t1 = tEnd;
            rollOver = false;
            continue;
        }

        for (int i=0; i < sourceCount; i++) {
            // Dota not coming in yet from this source so do NO calcs
            if (!dataArrived[i]) continue;

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

            missingByteCount = (currDiscardBytes[i] + currDropBytes[i]) - (prevDiscardBytes[i] + prevDropBytes[i]);
//            printf("  Packets:  currDiscardBytes = %" PRId64 ", currDropBytes = %" PRId64 ", prevDiscardBytes = %" PRId64 ", prevDropBytes = %" PRId64 "\n",
//                   currDiscardBytes[i], currDropBytes[i], prevDiscardBytes[i], prevDropBytes[i]);

//            // Can't tell how many bufs & packets are completely dropped unless we know exactly what's coming in
//            discardByteCount = currDiscardBytes[i] - prevDiscardBytes[i];
//            discardPktCount  = currDiscardPkts[i]  - prevDiscardPkts[i];
//            discardBufCount  = currDiscardBufs[i]  - prevDiscardBufs[i];

            pktRate    = 1000000.0 * ((double) pktCount) / microSec;
            pktAvgRate = 1000000.0 * ((double) currTotalPkts[i]) / totalMicroSecs[i];
            if (pktCount == 0 && missingByteCount == 0) {
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg\n", sourceIds[i], pktRate, pktAvgRate);
            }
            else {
                printf("%d Packets:  %3.4g Hz,  %3.4g Avg, missing bytes = %" PRId64 "\n",
                       src, pktRate, pktAvgRate, missingByteCount);
            }

            // Actual Data rates (no header info)
            dataRate    = ((double) byteCount) / microSec;
            dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSecs[i];

            // TODO: currently cpuPkt holds nothing, set in main thread - one value

            // TODO: look at this to see if it works for multiple sources
#ifdef __linux__
            printf("     Data:  %3.4g MB/s,  %3.4g Avg, pkt cpu %d, buf cpu %d, bufs %u\n\n",
                   dataRate, dataAvgRate, mapp[src]->cpuPkt, mapp[src]->cpuBuf, mapp[src]->builtBuffers);

           if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 ",%d,%d\n\n", totalMicroSecs[i]/1000000, src,
                        (int)(pktRate/1000), (int)(dataRate),
                        missingByteCount, (currDiscardBytes[i] + currDropBytes[i]),
                        mapp[src]->cpuPkt, mapp[src]->cpuBuf);
                fflush(fp);
            }
#else
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, bufs %u\n\n",
                   dataRate, dataAvgRate, mapp[src]->builtBuffers);

            if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 "\n\n", totalMicroSecs[i]/1000000, src,
                        (int)(pktRate/1000), (int)(dataRate),
                        missingByteCount, (currDiscardBytes[i] + currDropBytes[i]));
                fflush(fp);
            }
#endif

        }

        t1 = tEnd;
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
    /** Supply of buffers for holding reassembled data from a single source.
     *  One from each reassembly thread. Used in threadReadBuffers thread. */
    std::shared_ptr<Supplier<BufferItem>> bufSupplies[MAX_RE_THREADS];

    /** Used to hold map, key = src id, val = buffer supply, for reassembly thds. */
    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMap;

    /** Supply of structures holding UDP packets. */
    std::shared_ptr<SupplierN<PacketsItemN>> pktSupply;

    /** Byte size of buffers contained in supply. */
    int bufferSize;
    int mtu;
    int sourceCount;
    int pktConsumerId;
    int sourceId;

    int everyNth;
    int tickOffset;  // 0, 1, 2, ...

    bool debug;
    bool dump;

    // shared ptr of map of stats
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats;

    uint64_t expectedTick;
    int tickPrescale;
    int core;

} threadArg;


/**
 * <p>
 * Thread to assemble incoming packets into their original buffers.
 * Data may come from multiple data sources. Each buffer supply,
 * which supplies buffers in which to contruct the original data,
 * will be associated with only one (1) data source.
 * Thus each one of these "threadAssemble" threads will have 1 buffer supply
 * for each data source.
 * </p>
 * >p>
 * This thread gets packets that have already been read in and placed into a "packet" ring
 * buffer by another thread. It gets an empty buffer from the appropriate supply,
 * reassembles data in it as packets arrive, then places
 * a fully assembled buffer back into its supply (ring buffer).
 * Downstream, there is 1 thread per data source. These threads have access to the
 * fully assembled buffers.
 * </p>
 * <p>
 * How is the decision made to discard packets?
 * If packets come out-of-order, more than one buffer may be being constructed at ony one time.
 * This thread tracks the biggest tick # received from each source.
 * If the buffer for a particular tick has not been completed (due to missing packets),
 * it is discarded when its tick is &lt; biggestTick - 2 * tickPrescale.
 * Essentially, it dumps everything before latest 3 ticks.
 * Holding onto more ticks causes performance to degrade.
 * After all packets from 1 packet array are consumed, a scan is made to
 * see if anything needs to be discarded.
 * </p>
 * <p>
 * All out-of-order packets are handled; however, duplicate packets are not.
 * The generation of duplicate packets can be eliminated (or at least greatly reduced)
 * by setting up the UDP sockets properly.
 * </p>
 * <p>
 * What happens if a data source dies and restarts?
 * This causes the tick sequence to restart. In order to avoid causing problems
 * calculating various stats, like rates, the "biggest tick" from a source is
 * reset if 100 packets with smaller ticks show up after.
 * </p>
 *
 *
 * @param arg   struct to be passed to thread.
 */
static void *threadAssemble(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int core = tArg->core;
    int sourceCount = tArg->sourceCount;
    int id = tArg->pktConsumerId;

    auto pktSupply = tArg->pktSupply;

    bool dumpBufs = tArg->dump;
    bool debug    = tArg->debug;

    int everyNth = tArg->everyNth;
    // If ticks are tickBandwidth below the max tick, then source sequence has been restarted
    int tickBandwidth = 100*everyNth;
    int tickOffset = tArg->tickOffset;

    std::shared_ptr<Supplier<BufferItem>> bufSupply, prevBufSupply;
    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMap = tArg->supplyMap;

    auto stats    = tArg->stats;
    bool takeStats = stats != nullptr;
    std::unordered_map<int, std::shared_ptr<packetRecvStats>> mapp;
    if (takeStats) {
        mapp = *stats;
    }

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


    std::shared_ptr<BufferItem>  bufItem;
    std::shared_ptr<PacketsItemN> pktItem;
    std::shared_ptr<ByteBuffer>  buf;

#ifdef __linux__

    if (core > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run assemble thd for all sources on core " << core << "\n";
        CPU_SET(core, &cpuset);
        std::cerr << "Run assemble thd for all sources on core " << (core + 1) << "\n";
        CPU_SET(core+1, &cpuset);

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

    // Pointer to map keeps buffers being worked on: key = tick, val = buffer
    std::unordered_map<uint64_t, std::shared_ptr<BufferItem>> *pmap;

    // One tick for each source: key = source id, val = largest tick value received
    std::unordered_map<int, uint64_t> largestSavedTick;

    // Need to figure out if source was killed and restarted, i.e. the tick number
    // sequence has restarted. Account for this if we want stats to be accurate.
    // Do this by counting # of ticks below the max tick received. If we get
    // 100 smaller ticks, time to reset the max tic.
    // One count for each source: key = source id, val = # of smaller ticks received
    std::unordered_map<int, int> smallerTicks;

    std::cout << "Reassemble, tick offset = " << tickOffset << ", everyNth = " << everyNth << std::endl;

    bool wrongSrc = false;


    while (true) {

        //-------------------------------------------------------------
        // Get item containing packets previously read in by another thd
        //-------------------------------------------------------------
        pktItem = pktSupply->consumerGet(id);
        size_t packetCount = pktItem->getPacketsFilled();

        int srcId, prevSrcId = -1;
        uint64_t tick, prevTick = UINT64_MAX;
        pmap = nullptr;

        assert(packetCount > 0);

        for (int i = 0; i < packetCount; i++) {
            reHeader *hdr = pktItem->getHeader(i);
            tick = hdr->tick;

            // Skip this tick and go to the next if looking for odd and have even or vice versa
            if ((tick + tickOffset) % everyNth != 0) {
                continue;
            }

            srcId = hdr->dataId;
            if (srcId != prevSrcId) {
                // Switching to a different data source ...

                // First check to see if source is legit, else ignore.
                // If no map entry for this source, its unknown.
                if (supplyMap.count(srcId) == 0) {
                    if (!wrongSrc) {
                        // Print ONCE if there is an unapproved data source
std::cout << "Unexpected data source, id = " << srcId << ", ignoring this data" << std::endl;
                        wrongSrc = true;
                    }
                    continue;
                }

                bufSupply = supplyMap[srcId];

                //std::cout << "get bufSupply for src " << srcId << ", = " << bufSupply << std::endl;

                // Get the right map (key = tick, val = buffer) if there is one, else make one
                pmap = maps[srcId];
                if (pmap == nullptr) {
//std::cout << "Create map for src " << srcId << std::endl;
                    maps[srcId] = pmap = new std::unordered_map<uint64_t, std::shared_ptr<BufferItem>>();
                }

                // Get buffer in which to reassemble
                bufItem = (*pmap)[hdr->tick];
                // If there is no buffer existing for this tick, get one
                if (bufItem == nullptr) {
//std::cout << "\ndiff src,  create buf for tick " << hdr->tick << std::endl;
                    // This call gets a reset bufItem
                    (*pmap)[hdr->tick] = bufItem = bufSupply->get();

                    // Copy header so that whoever gets the reassembled buffer has info about tick, src id, etc
                    bufItem->setHeader(hdr);

                    // Track the biggest tick to be RECEIVED from this source.
                    // Anything too much smaller will be tossed since a late packet cannot be
                    // really, really late.
                    uint64_t largestTick = largestSavedTick[srcId];
                    if (hdr->tick > largestTick) {
//std::cout << "biggest tick " << hdr->tick << " for src " << srcId << std::endl;
                        largestSavedTick[srcId] = hdr->tick;
                    }
                    else if (hdr->tick < largestTick - tickBandwidth) {
                        // If here, we got a tick very much smaller than we should

                        if (++smallerTicks[srcId] > 100) {
                            // Got a much smaller tick 100x so sequence has been restarted, reset stats
                            largestSavedTick[srcId] = hdr->tick;
                            smallerTicks[srcId] = 0;

                            // AND throw out all stored, unfinished bufs since they'll never
                            // be finished at this point. These will act like time bombs.
                            // They have higher tick values and just sit there until we reach those
                            // tick values and then will try to add new packets with the old.
                            // Clear everything except what we just created. The old bufItems need to be labeled
                            // as invalid and moved on.

                            // Using the iterator and the "erase" method, as shown,
                            // will avoid problems invalidating the iterator
                            for (auto it = pmap->cbegin(); it != pmap->cend();) {
                                uint64_t tck = it->first;
                                std::shared_ptr<BufferItem> bItem = it->second;

                                // Remove all ticks larger than the first of the new sequence (hdr->tick)
                                if (tck > hdr->tick) {
                                    it = pmap->erase(it);
//std::cout << "  release stored buf for tick " << tck << std::endl;

                                    // Release resources here.
                                    if (dumpBufs) {
                                        bufSupply->release(bItem);
                                    }
                                    else {
                                        // We need to label bad buffers.
                                        bItem->setValidData(false);
                                        bufSupply->publish(bItem);
                                    }
                                }
                                else {
                                    ++it;
                                }
                            }
std::cout << "tick sequence has restarted for source " << srcId << ", reset stats" << std::endl;
                        }
                    }
                }
            }
            else if (hdr->tick != prevTick) {
                // Same source as last pkt, but if NOT the same tick ...
                bufItem = (*pmap)[hdr->tick];
                if (bufItem == nullptr) {
//std::cout << "\nsame src, create buf for tick " << hdr->tick << std::endl;
                    (*pmap)[hdr->tick] = bufItem = bufSupply->get();
                    bufItem->setHeader(hdr);
                    uint64_t largestTick = largestSavedTick[srcId];
                    if (hdr->tick > largestTick) {
//std::cout << "biggest tick " << hdr->tick << " for src " << srcId << std::endl;
                        largestSavedTick[srcId] = hdr->tick;
                    }
                    else if (hdr->tick < largestTick - tickBandwidth) {
                        if (++smallerTicks[srcId] > 100) {
                            largestSavedTick[srcId] = hdr->tick;
                            smallerTicks[srcId] = 0;

                            for (auto it = pmap->cbegin(); it != pmap->cend();) {
                                uint64_t tck = it->first;
                                std::shared_ptr<BufferItem> bItem = it->second;
                                if (tck > hdr->tick) {
                                    it = pmap->erase(it);
//std::cout << "  release stored buf for tick " << tck << std::endl;
                                    if (dumpBufs) {
                                        bufSupply->release(bItem);
                                    }
                                    else {
                                        bItem->setValidData(false);
                                        bufSupply->publish(bItem);
                                    }
                                }
                                else {
                                    ++it;
                                }
                            }
std::cout << "tick sequence has restarted for source " << srcId << ", reset stats" << std::endl;
                        }
                    }
                }
            }

            // Keep track so if next packet is same source/tick, we don't have to look it up
            prevSrcId = srcId;
            prevTick  = hdr->tick;

            // We are using the ability of the BufferItem to store a user's long.
            // Use it store how many bytes we've copied into it so far.
            // This will enable us to tell if we're done w/ reassembly.
            // Likewise, store # packets copied in user int.
            int64_t bytesSoFar = bufItem->getUserLong();
            int32_t pktsSoFar  = bufItem->getUserInt();
            int64_t dataLen    = pktItem->getPacket(i)->msg_len - HEADER_BYTES;
//std::cout << "pkt len " << dataLen << std::endl;

            // Do we have memory to store entire buf? If not, expand.
            if (hdr->length > bufItem->getBuffer()->capacity()) {
std::cout << "EXPAND BUF!!! to " << hdr->length << std::endl;
                // Preserves all existing data while increasing underlying array
                bufItem->expandBuffer(hdr->length + 27000); // also fit in 3 extra jumbo packets
            }

            // The neat thing about doing things this way is we don't have to track out-of-order packets.
            // We don't have to copy and store them!
            //
            // Duplicate packets just write over themselves. However,
            // duplicate packets will mess up our calculation on whether we've received all packets
            // that make up a buffer (dataLen). The easiest way to deal with that is sending UDP over a single
            // network interface and having the receiver listen on a specific interface (not INADDR_ANY).
            // So, send in a manner in which duplication will not happen. I think we can control this
            // without too much effort.
            // The alternative is to add a sequential number to RE header to be able to track this.
            // That requires more bookkeeping on this receiving end.

            // Copy things into the correct position in a buf from the iovec buffer
            auto data = (uint8_t *)(pktItem->getPacket(i)->msg_hdr.msg_iov[0].iov_base) + HEADER_BYTES;
            memcpy(bufItem->getBuffer()->array() + hdr->offset,data,dataLen);

            // Keep track of how much we've written so far
            bytesSoFar += dataLen;
            bufItem->setUserLong(bytesSoFar);

            // Track # of packets written so far (will double count for duplicate packets)
            pktsSoFar++;
            bufItem->setUserInt(pktsSoFar);

            // If we've written all data to this buf ...
            if (bytesSoFar >= hdr->length) {
                // Done with this buffer, so get it reading for reading
                bufItem->getBuffer()->limit(bytesSoFar).position(0);

                // Clear buffer from local map
                pmap->erase(hdr->tick);
//std::cout << "Remove tck " << hdr->tick << " src " << srcId << " from map, bytes = " << bytesSoFar << std::endl;

                if (takeStats) {
                    mapp[srcId]->acceptedBytes += bytesSoFar;
                    mapp[srcId]->builtBuffers++;
                    mapp[srcId]->acceptedPackets += bufItem->getUserInt();
                }

                // Pass buffer to waiting consumer or just dump it
                if (dumpBufs) {
//std::cout << "   " << id << ": dump tck " << hdr->tick << " src " << srcId << std::endl;
                    bufSupply->release(bufItem);
                }
                else {
//std::cout << "   " << id << ": pub tck " << hdr->tick << " src " << srcId << std::endl;
                    bufSupply->publish(bufItem);
                }
            }
        }

        // If here, we've gone thru a bundle of UDP packets,
        // time to get the next bundle.
//std::cout << "Release pkt @id " << id << std::endl;
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

                    // Release resources here.
                    // This source only has bufItems from supplyMap[source].
                    if (dumpBufs) {
//std::cout << "   " << id << " clear: dump tck " << tck << " src " << srcId << std::endl;
                        supplyMap[source]->release(bItem);
                    }
                    else {
                        // We need to label bad buffers.
                        // Perhaps we could reuse them. But if we do that,
                        // things will be out of order and access to filled buffers will be delayed!
                        bItem->setValidData(false);
//std::cout << "   " << id << " clear: pub tck " << tck << " src " << srcId << std::endl;
                        supplyMap[source]->publish(bItem);
                    }
                }
                else {
                    ++it;
                }
            }
        }

//        // Finish up some stats
//        if (takeStats) {
//#ifdef __linux__
//            // If core hasn't been pinned, track it
//            if ((core < 0) && (loopCount-- < 1)) {
//                mapp[srcId]->cpuBuf = sched_getcpu();
//                loopCount = cpuLoops;
////printf("Read pkt thd: get CPU\n");
//            }
//#endif
//        }
    }

    return nullptr;
}



/**
 * Thread to read all filled buffers from a single, particular data source.
 * In general, there will be one buffer supply from each reassembly thread
 * with this particular source.
 * @param arg
 * @return
 */
static void *threadReadBuffers(void *arg) {

    threadArg *tArg = (threadArg *) arg;

    int thdCount = tArg->everyNth;
    int id = tArg->sourceId;
//    int prescale = tArg->tickPrescale;

    std::cout << "   Started cleanup thread for source " << id << ", thd count = " << thdCount << std::endl;

    std::shared_ptr<Supplier<BufferItem>> bufSupplies[thdCount];
    for (int i=0; i < thdCount; i++) {
        std::cout << "   id " << id << " create buf supply for thd " << i << std::endl;
        bufSupplies[i] = tArg->bufSupplies[i];
    }

    std::shared_ptr<BufferItem> bufItem;

//    bool firstTime = true;
//    int64_t prevTick = 0;

    // If bufs are not already dumped by the reassembly thread,
    // we need to put them back into the supply now.
    while (true) {
        for (int i=0; i < thdCount; i++) {
            // Grab a fully reassembled buffer from Supplier
            bufItem = bufSupplies[i]->consumerGet();

//            // Check that ticks are in order, works only with 1 reassembly thread
//            reHeader hdr = bufItem->getHeader();
//            int64_t tick = hdr.tick;
//            if (firstTime) {
//                firstTime = false;
//            }
//            else {
//                // This only works with 1 reassembly thread.
//                // Using 2 can get ticks out of order here.
//                if ((tick - prevTick) != prescale) {
//                    std::cout << "   tick hopped by " << (tick - prevTick) << std::endl;
//                }
//            }
//            prevTick = tick;


//            if (bufItem->validData()) {
//                // Do something with buffer here.
//                // Data can be accessed thru the (shared pointer to) ByteBuffer object:
//                // std::shared_ptr<ByteBuffer> buffer = bufItem->getBuffer();
//                // or more directly thru its byte array:
//                uint8_t *buf = bufItem->getBuffer()->array();
//                size_t dataBytes = bufItem->getBuffer()->limit();
//                // Get access to meta data from its packet RE header
//                // if data source id or tick value is needed.
//                reHeader hdr = bufItem->getHeader();
//            }

            // Release item for reuse
            bufSupplies[i]->release(bufItem);
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
    uint16_t startingPort = 17750;
    int startingCore = -1;
    int coreCount = 1;
    int startingBufCore = -1;
    int sourceIds[MAX_SOURCES];
    int sourceCount = 0;
    int thdCount = 1;
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

    for (int i = 0; i < MAX_SOURCES; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize,
              &tickPrescale, &startingCore, &coreCount,
              &startingBufCore, sourceIds,
              &thdCount, &startingPort, &debug, &useIPv6,
              &keepStats, &dumpBufs, listeningAddr, filename);

    pinCores = startingCore >= 0 ? true : false;
    pinBufCores = startingBufCore >= 0 ? true : false;

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

    std::cout << "thdCount = " << thdCount << ", source count = " << sourceCount << std::endl;

    // Arrays for holding threads and buffer supplies
    pthread_t thds[thdCount];
    threadArg *tArg[thdCount];
    // Array of Maps of supplies. Each map --> key = src id, value = supply.
    // Each element of the array is for a given reassembly thread.
    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMaps[MAX_RE_THREADS];

    std::shared_ptr<Supplier<BufferItem>> bufSupplies[MAX_RE_THREADS];
    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMap;


    //---------------------------------------------------
    // Supply in which each item holds 20 UDP packets and parsed header info.
    //---------------------------------------------------
    int pktRingSize = 32;
    size_t maxPktsPerRecv = 20;
    PacketsItemN::setEventFactorySettings(maxPktsPerRecv, thdCount);
    std::shared_ptr<SupplierN<PacketsItemN>> pktSupply =
            std::make_shared<SupplierN<PacketsItemN>>(pktRingSize, true, thdCount);

    //---------------------------------------------------
    // Supplies in which each buf will hold reconstructed buffers.
    // Make these buffers sized as given on command line (100kB default) and expand as necessary.
    // For really small buffers (1 or 2 pkts), they may be created out of order
    // if pkts come out-of-order, so the orderedRelease flag should be false.
    //---------------------------------------------------
    int ringSize = 64; // 128 works too
    BufferItem::setEventFactorySettings(ByteOrder::ENDIAN_LOCAL, bufSize);
    for (int i=0; i < thdCount; i++) {
        // "source" number of buffer supplies for each reassembly thread
        for (int j=0; j < sourceCount; j++) {
            std::cout << "add entry for thd " << i << ", source " << sourceIds[j] << " into array of maps" << std::endl;
            supplyMaps[i][sourceIds[j]] = std::make_shared<Supplier<BufferItem>>(ringSize, false);
        }

        // Start thread to reassemble buffers of packets from all sources
        auto arg = tArg[i] = (threadArg *) calloc(1, sizeof(threadArg));
        if ( arg == nullptr) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        // maps copied thru assignment
        arg->supplyMap = supplyMaps[i];

        arg->pktSupply = pktSupply;
        arg->stats = stats;
        arg->dump  = dumpBufs;
        arg->debug = debug;
        arg->sourceCount = sourceCount;
        // tickPrescale tells us what is coming in from a source, there is also a
        // prescale factor from the # of thds used to reassemble.
        // Note that the command line only allows one tick prescale to be set
        // for all sources. That could be improved in a future version.
        std::cout << "set prescale for reass thd " << thdCount << " to " << arg->tickPrescale << std::endl;
        arg->tickPrescale = thdCount + tickPrescale;
        arg->pktConsumerId = i;

        arg->everyNth = thdCount;
        arg->tickOffset = i;

        if (pinBufCores) {
            arg->core = startingBufCore + 2*i;
        }
        else {
            arg->core = -1;
        }

        status = pthread_create(&thds[i], NULL, threadAssemble, (void *) arg);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    //---------------------------------------------------
    // Thread(s) to read and/or dump fully reassembled buffers
    //---------------------------------------------------
    if (!dumpBufs) {
         for (int j=0; j < sourceCount; j++) {
                threadArg *targ = (threadArg *) calloc(1, sizeof(threadArg));
                if (targ == nullptr) {
                    fprintf(stderr, "out of mem\n");
                    return -1;
                }

                // Supply of buffers for holding reassembled data from a single source.
                // One from each reassembly thread.
                int source = sourceIds[j];
                for (int i=0; i < thdCount; i++) {
                    targ->bufSupplies[i] = supplyMaps[i][source];
                }

                targ->debug = debug;
                targ->sourceId = source;
                targ->everyNth = thdCount;
                targ->tickPrescale = tickPrescale;

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

    std::shared_ptr<PacketsItemN> item;

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
        packetCount = recvmmsg(udpSocket, item->getPackets(), maxPktsPerRecv, MSG_WAITFORONE, nullptr);
        if (item->dataDiscarded()) {
            fprintf(stderr, "******* data lost receiving UDP packets\n");
            exit(-1);
        }
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

//            char *data = (char *)(item->getPacket(i)->msg_hdr.msg_iov[0].iov_base) + HEADER_BYTES;
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

