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
 * This assumes there is a load balancing emulator or FPGA between this and the sending program.
 * Note that this program uses fast, non-locking ring buffers.
 * This program is intended to be a full-blown implementation of how to handle multiple
 * data sources coming into 1 socket and having it all sorted and assembled into separate
 * buffers which can then be accessed by the user.
 * </p>
 * <p>
 * In this program, a single, main thread reads from a single socket in which packets from
 * multiple (16 max) data sources are arriving. There is a ring buffer with pre-allocated
 * structures in which to store the incoming packets. This thread looks at the packet headers
 * to find the source id and puts a reference to each packet on another ring - one for each
 * source.
 * </p>
 * <p>
 * There are additional threads, one for each data source, which read packets from a ring with packet
 * references in order to do the reassembly. Each of these threads reassembles events into a buffer
 * obtained from a (ring-based, recycling) source of buffers (again, one per source reassembly thd).
 * Depending on command line parameters, additional threads can be started (one per source id) to
 * access the final buffers.
 * </p>
 * <p>
 * There is also a thread to calculate and print rates.
 * </p>
 * <p>
 * Each reassembly thread accounts for out-of-order and duplicated packets.
 * It will only keep up to a max of 5 partially constructed events in an effort to wait for
 * straggling packets with the oldest event being discarded if more storage is needed.
 * Also, old events will have their memory released if not fully constructed before 10 milliseconds.
 * </p>
 * <p>
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
#include <utility>
#include <unordered_map>
#include <errno.h>

#include "BufferItem.h"
#include "PacketRefItem.h"
#include "PacketStoreItem.h"
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

// Max number of chars in a name
#define INPUT_LENGTH_MAX 256

// Max # of data input sources
#define MAX_SOURCES 16

// Max number of milliseconds that incomplete buffers are kept,
// hoping for late packets to arrive.
#define MILLISEC_STORED 10


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <starting listening UDP port (increment for each source, 17750 default)>]",
            "        [-b <internal buffer byte size, 100kB default>]",
            "        [-r <UDP receive buffer byte size, system default>]",
            "        [-f <file for stats>]",
            "        [-dump (no thd to get & merge buffers)]",
            "        [-stats (keep stats)]",
            "        [-ids <comma-separated list of incoming source ids>]",
            "        [-pinRead <starting core # for read thd>]",
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
                      uint16_t* port,
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
            printf("     Data:  %3.4g MB/s,  %3.4g Avg, pkt cpu %d, buf cpu %d, bufs %" PRId64 "\n\n",
                   dataRate, dataAvgRate, mapp[src]->cpuPkt, mapp[src]->cpuBuf, mapp[src]->builtBuffers);

           if (writeToFile) {
                fprintf(fp, "%" PRId64 ",%d,%d,%d,%" PRId64 ",%" PRId64 ",%d,%d\n\n", totalMicroSecs[i]/1000000, src,
                        (int)(pktRate/1000), (int)(dataRate),
                        missingByteCount, (currDiscardBytes[i] + currDropBytes[i]),
                        mapp[src]->cpuPkt, mapp[src]->cpuBuf);
                fflush(fp);
            }
#else
            printf("     Data:    %3.4g MB/s,  %3.4g Avg, bufs %" PRId64 "\n\n",
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
    /** One buffer supply for each reassembly thread. Used in threadReadBuffers thread. */
    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMap;

    /** Supply with full UDP packets. */
    std::shared_ptr<Supplier<PacketStoreItem>> pktSupply;

    /** Supply holding UDP packet references. */
    std::shared_ptr<Supplier<PacketRefItem>> refSupply;

    /** Supply holding reassembly buffers. */
//    std::shared_ptr<Supplier<BufferItem>> bufSupply;

    /** Byte size of buffers contained in supply. */
    int bufferSize;
    int mtu;
    int sourceCount;
    int pktConsumerId;
    int sourceId;

    bool debug;
    bool dump;

    // shared ptr of stats
    std::shared_ptr<packetRecvStats> stats;

    int core;

} threadArg;


/**
 * <p>
 * Thread to assemble incoming packets into their original buffers.
 *
 * There is an input data ring in which references to packets from just one data source have been placed
 * by the reading thread. (This reading thd looks to see which source a packet is from
 * and places it into the proper data ring).
 * There is another ring (one per source in this application) which contains empty buffers
 * designed to hold built events.
 * An empty buffer is taken from the buffer ring. Then packets are taken from the
 * data ring and reconstructed into an event in that empty buffer.
 * When the event is fully constructed, the buffer is released back to the ring
 * which is then accessed by another thread which is looking for the built events.
 * </p>
 * <p>
 * How is the decision made to discard packets?
 * If packets come out-of-order, then more than one buffer will be constructed at one time.
 * This thread will only keep up to a max of 5 partially constructed events in an effort to wait for
 * straggling packets with the oldest event being discarded if more storage is needed.
 * Also, old events will have their memory released if not fully constructed before 10 milliseconds.
 * </p>
 * <p>
 * After all packets from 1 packet array are consumed, a scan is made to
 * see if anything needs to be discarded.
 * </p>
 * <p>
 * All out-of-order and duplicate packets are handled.
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

    int id = tArg->pktConsumerId;
    int sourceId = tArg->sourceId;

    auto pktSupply = tArg->pktSupply;
    auto refSupply = tArg->refSupply;
    auto bufSupply = tArg->supplyMap[sourceId];

    /** Byte size of buffers contained in supply. */

    bool dumpBufs = tArg->dump;
    bool debug    = tArg->debug;

    auto stats    = tArg->stats;
    bool takeStats = (stats != nullptr);

    // expected max size of packets
    int mtu = tArg->mtu;
    if (mtu < 1) {
        mtu = 9000;
    }
    else if (mtu < 1400) {
        mtu = 1400;
    }

    std::shared_ptr<BufferItem>    bufItem;
    std::shared_ptr<PacketRefItem> refItem;
    std::shared_ptr<ByteBuffer>    buf;

#ifdef __linux__

    int core = tArg->core;
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
    // key = tick, val = epoch in millisec
    // This will allow us to dump old, partially assembled bufs/pkts
    std::map<uint64_t, int64_t> timeMap;
    // For reading time
    struct timespec now;
    int64_t milliSec = 0;

    // Need to figure out if source was killed and restarted.
    // If this is the case, then the tick # will be much smaller than the previous value.
    uint64_t largestTick = 0;
    uint32_t offset, length;
    uint64_t tick, prevTick = UINT64_MAX;


    while (true) {

        //-------------------------------------------------------------
        // Get item containing packet previously read in by main thd
        //-------------------------------------------------------------
        refItem = refSupply->consumerGet();
        reHeader *hdr = refItem->getPacket()->getHeader();
        tick   = hdr->tick;
        offset = hdr->offset;
        length = hdr->length;

        assert(hdr->dataId == sourceId);

        // If NOT the same tick as previous ...
        if (tick != prevTick) {
            if (tick > largestTick) {
                // With the load balancer, ticks can only increase. This new, largest tick
                // will not have an associated buffer yet, so create it, store it, etc.

                //std::cout << "\get new buf for tick " << tick << std::endl;

                // store bufItem in map
                tickMap[tick] = bufItem = bufSupply->get();
                bufItem->setEventNum(tick);
                bufItem->setDataLen(length);

                bufItem->setHeader(hdr); // TODO: remove?

                // store current time in msec
                clock_gettime(CLOCK_MONOTONIC, &now);
                milliSec = ((1000L * now.tv_sec) + (now.tv_nsec/1000000L));
                timeMap.insert({tick, milliSec});

                largestTick = tick;

                // Are we at limit of # of buffers being worked on? If so, discard oldest
                if (tickMap.size() > 4) {
                    auto oldestEntry = *(tickMap.begin());
                    uint64_t oldestTick = oldestEntry.first;
                    std::shared_ptr<BufferItem> oldestBufItem = oldestEntry.second;
                    tickMap.erase(oldestTick);
                    timeMap.erase(oldestTick);

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
            else {
                bufItem = tickMap[tick];
                if (bufItem == nullptr) {
                    // If there is no buffer associated with this tick available,
                    //    1) it may have been reassembled and this is a late, duplicate packet, or
                    //    2) it is a very late packet and the incomplete buffer was deleted/released.
                    // In both of those cases, it can be released and ignored. So go to the next packet.
                    refItem->releasePacket();
                    continue;
                }
            }
        }

        // Check for duplicate packets, ie if its offset has been stored already
        auto &offsets = bufItem->getOffsets();
        if (offsets.find(offset) != offsets.end()) {
            // There was already a packet with this offset, so ignore this duplicate packet!!
            refItem->releasePacket();
            std::cout << "Got duplicate packet for event " << tick << ", offset " << offset << std::endl;
            continue;
        }
        else {
            // Record this packet's presence in buffer by storing the unique offset in a set
            offsets.insert(offset);
        }

        // Keep track so if next packet is same source/tick, we don't have to look it up
        prevTick = tick;

        // We are using the ability of the BufferItem to store a user's long.
        // Use it store how many bytes we've copied into it so far.
        // This will enable us to tell if we're done w/ reassembly.
        // Likewise, store # packets copied in user int.
        int64_t bytesSoFar = bufItem->getUserLong();
        int32_t pktsSoFar  = bufItem->getUserInt();
        int64_t dataLen    = length - HEADER_BYTES;
        //std::cout << "pkt len = " << dataLen << std::endl;

        // Do we have memory to store entire buf? If not, expand.
        if (length > bufItem->getBuffer()->capacity()) {
            std::cout << "EXPAND BUF! to " << (length + 27000) << std::endl;
            // Preserves all existing data while increasing underlying array
            bufItem->expandBuffer(length + 27000); // also fit in 3 extra jumbo packets
        }

        // The neat thing about doing things this way is we don't have to track out-of-order packets.
        // Copy things into the buffer into its final spot.
        auto data = (uint8_t *) (refItem->getPacket()->getPacket()) + HEADER_BYTES;
        memcpy(bufItem->getBuffer()->array() + offset, data, dataLen);

        // Release packet since we've copied its data
        refItem->releasePacket();

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
            //std::cout << "Remove tck " << tick << " src " << sourceId << " from map, bytes = " << bytesSoFar << std::endl;

            if (takeStats) {
                stats->acceptedBytes += bytesSoFar;
                stats->builtBuffers++;
                stats->acceptedPackets += bufItem->getUserInt();
            }

            // Pass buffer to waiting consumer or just dump it
            if (dumpBufs) {
                //std::cout << "   " << id << ": dump tck " << ick << " src " << sourceId << std::endl;
                bufSupply->release(bufItem);
            }
            else {
                //std::cout << "   " << id << ": pub tck " << tick << " src " << sourceId << std::endl;
                bufSupply->publish(bufItem);
            }
        }

        // See if items in tickMap/timeMap have stayed over the limit (MILLISEC_STORED)
        if (timeMap.size() > 0) {

            clock_gettime(CLOCK_MONOTONIC, &now);
            milliSec = ((1000L * now.tv_sec) + (now.tv_nsec/1000000L));

            // Iterate through the map
            auto it = timeMap.begin();
            while (it != timeMap.end()) {
                // Check if "now" is more than MILLISEC_STORED millisec ahead of this entry
                if ((milliSec - it->second) >= MILLISEC_STORED) {
                    // It's too old, so release buffer resources
                    uint64_t tik = it->first;
                    auto bItem = tickMap[tik];
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






/**
 * Thread to read all filled buffers from a single, particular data source.
 * There is only one buffer supply for each data source.
 * @param arg
 * @return
 */
static void *threadReadBuffers(void *arg) {

    threadArg *tArg = (threadArg *) arg;
    int id = tArg->sourceId;
    auto & supply = tArg->supplyMap[id];

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
//                // std::shared_ptr<ByteBuffer> buffer = bufItem->getBuffer();
//                // or more directly thru its byte array:
//                uint8_t *buf = bufItem->getBuffer()->array();
//                size_t dataBytes = bufItem->getBuffer()->limit();
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

    //---------------------------------------------------
    // Map to convert from data source id to the index in our local array of source ids
    //---------------------------------------------------
    std::unordered_map<int, int> srcMap;
    for (int i=0; i < sourceCount; i++) {
        srcMap[sourceIds[i]] = i;
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
    std::shared_ptr<packetRecvStats> readStats = nullptr;
    std::shared_ptr<packetRecvStats> buildStats[sourceCount];
    // key = source id, val = stats
    std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> allBuildStats;

    if (keepStats) {
        readStats = std::make_shared<packetRecvStats>();
        clearStats(readStats);

        allBuildStats = std::make_shared<std::unordered_map<int, std::shared_ptr<packetRecvStats>>>();

        for (int i = 0; i < sourceCount; i++) {
            buildStats[i] = std::make_shared<packetRecvStats>();
            clearStats(buildStats[i]);
            allBuildStats->insert({srcMap[i], buildStats[i]});
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
    }

    fprintf(stderr, "UDP port %d, socket recv buffer = %d bytes, source count = %d\n",
            startingPort, recvBufSize, sourceCount);

    // Arrays for holding threads & their args
    pthread_t thds[sourceCount];
    threadArg *tArg[sourceCount];

    //---------------------------------------------------
    // Create supply in which to hold incoming packets in PacketStoreItem objects.
    //---------------------------------------------------
    int pktRingSize = 8192;  // ~75MB allocation
    bool orderedRelease = false; // reassembly thds will be finished with pkts in no fixed order
    std::shared_ptr<Supplier<PacketStoreItem>> pktSupply =
            std::make_shared<Supplier<PacketStoreItem>>(pktRingSize, orderedRelease);

    //---------------------------------------------------
    // References to these PacketStoreItem objects are placed into a PacketRefItem from another supply.
    // Thus, no data copying happens. Packets from a single source go to a single supply.
    // There will be one reassembly thread which takes the single source packets from one
    // of these supplies and builds buffers.
    //---------------------------------------------------
    int holdRingSize = 1024;
    // packets used and released in order by reassembly thread
    orderedRelease = true;
    // set the packet supply once since it's the same for all packets
    PacketRefItem::setDefaultFactorySettings(pktSupply);
    std::shared_ptr<Supplier<PacketRefItem>> refPktSupplys[sourceCount];
    for (int i=0; i < sourceCount; i++) {
        refPktSupplys[i] = std::make_shared<Supplier<PacketRefItem>>(holdRingSize, orderedRelease);
    }

    //---------------------------------------------------
    // Each reassembly thd will have a buffer supply in which to hold reconstructed buffers.
    // Make these buffers sized as given on command line (100kB default) and expand as necessary.
    // For really small buffers (1 or 2 pkts), they may be created out of order
    // if pkts come out-of-order, so the orderedRelease flag should be false.
    //---------------------------------------------------
    int ringSize = 64;
    BufferItem::setEventFactorySettings(ByteOrder::ENDIAN_LOCAL, bufSize);
    // Map of buffer supplies. Each map --> key = src id, value = supply.
    std::unordered_map<int, std::shared_ptr<Supplier<BufferItem>>> supplyMap;
    for (int j=0; j < sourceCount; j++) {
        auto bufSupply = std::make_shared<Supplier<BufferItem>>(ringSize, false);
        supplyMap[sourceIds[j]] = bufSupply;
    }

    // For each data source ...
    for (int i=0; i < sourceCount; i++) {
        // Start thread to reassemble buffers of packets from all sources
        auto arg = tArg[i] = (threadArg *) calloc(1, sizeof(threadArg));
        if ( arg == nullptr) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        arg->pktSupply  = pktSupply;
        arg->refSupply  = refPktSupplys[i];
        arg->supplyMap  = supplyMap;

        arg->stats = buildStats[i];
        arg->dump  = dumpBufs;
        arg->debug = debug;
        arg->sourceCount = sourceCount;
        arg->pktConsumerId = i;

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
    // Thread(s) to read fully reassembled buffers
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
            targ->supplyMap = supplyMap;
            targ->debug = debug;
            targ->sourceId = source;

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
        targ->stats = allBuildStats;
        targ->sourceIds = sourceIds;

        pthread_t thd2;
        status = pthread_create(&thd2, NULL, rateThread, (void *) targ);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    // Time Delay Here???

    struct timespec timeout;
    timeout.tv_sec = TIMEOUT;
    timeout.tv_nsec = 0;


    while (true) {
        // Get object from supply in which to store packet
        auto item = pktSupply->get();

        // Read UDP packet
        char* pkt = item->getPacket();
        ssize_t bytesRead = recvfrom(udpSocket, pkt, PacketStoreItem::size, 0, nullptr, nullptr);
        if (bytesRead < 0) {
            if (debug) fprintf(stderr, "recvmsg failed: %s\n", strerror(errno));
            return (RECV_MSG);
        }
        else if (bytesRead < HEADER_BYTES) {
            if (debug) fprintf(stderr, "packet does not contain not enough data\n");
            return (INTERNAL_ERROR);
        }

        // Parse header & store in item
        reHeader *hdr = item->getHeader();
        parseReHeader(pkt, hdr);

        // Pkt is from this source
        uint16_t srcId = hdr->dataId;

        // Put ref to this pkt into the supply for this source.
        // Use our map from sourceId to index in our array.
        int localSrcIndex = srcMap[srcId];
        std::shared_ptr<Supplier<PacketRefItem>> & sup = refPktSupplys[localSrcIndex];
        // This version of the "get" call does NOT clear the item - the values for the supply and packet
        // we wrote in there. We'll keep the supply since it's always the same, but overwrite packet
        // with the new one.
        auto ref = sup->getAsIs();
        ref->setPacket(item);
        sup->publish(ref);
    }

    return 0;
}

