//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive generated data sent by packetBlaster.c program.
 * This assumes there is an emulator or FPGA between this and the sending program.
 */

#include <cstdlib>
#include <iostream>
#include <ctime>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cinttypes>
#include <vector>

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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer byte sizez>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-f <file for stats>]",
            "        [-sock <# of listening sockets, 16 max>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with packetBlaster.\n");
    fprintf(stderr, "        Similar to packetBlastee but uses multiple UDP sockets bound to the same port to read data.\n");
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
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param listenAddr    filled with IP address to listen on.
 * @param filename      filled with name of file in which to write stats.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      int *sockets, int *cores, uint16_t* port,
                      bool *debug, bool *useIPv6,
                      char *listenAddr, char *filename) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, nullptr, 1},
                          {"ip6",  0, nullptr, 2},
                          {"cores",  1, nullptr, 3},
                          {"sock",  1, nullptr, 4},
                          {nullptr,       0, nullptr,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:f:", long_options, nullptr)) != EOF) {

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
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n\n");
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

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(listenAddr, optarg);
                break;

            case 'f':
                // output stat file
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "Output file name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(filename, optarg);
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

            case 4:
                // # of UDP sockets
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0 && i_tmp < 17) {
                    *sockets = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -sock, 0 < port < 17\n");
                    exit(-1);
                }
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
static volatile uint64_t totalBytes=0, totalPackets=0;
static volatile int cpu=-1;
static int64_t discardedPackets, discardedBytes;
static int32_t discardedTicks;


// For stat thread
typedef struct threadStruct_t {
    char filename[101];
} threadStruct;


// Thread to send to print out rates
static void *thread(void *arg) {

    int64_t packetCount, byteCount;
    int64_t prevTotalPackets, prevTotalBytes;
    int64_t currTotalPackets, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;


    // File writing stuff
    bool writeToFile = false;
    auto *targ = static_cast<threadStruct *>(arg);
    char *filename = targ->filename;
    FILE *fp;
    if (strlen(filename) > 0) {
        // open file
        writeToFile = true;
        fp = fopen (filename, "w");
        // validate file open for writing
        if (!fp) {
            fprintf(stderr, "file open failed: %s\n", strerror(errno));
            return (nullptr);
        }

        // Write column headers
        fprintf(fp, "Sec,PacketRate(kHz),DataRate(MB/s),Dropped,TotalDropped,CPU\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate, totalRate, totalAvgRate;
    int64_t totalT = 0, time, discardedPkts, totalDiscardedPkts = 0, discardedTiks, totalDiscardedTiks = 0;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        prevTotalBytes   = totalBytes;
        prevTotalPackets = totalPackets;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalT = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        currTotalBytes   = totalBytes;
        currTotalPackets = totalPackets;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalPackets > 0) {
                skipFirst = false;
            }
            firstT = t1 = t2;
            totalT = totalBytes = totalPackets = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount   = currTotalBytes   - prevTotalBytes;
        packetCount = currTotalPackets - prevTotalPackets;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = totalPackets = 0;
            firstT = t1 = t2;
            continue;
        }

        // Dropped stuff rates
        discardedPkts = discardedPackets;
        discardedPackets = 0;
        totalDiscardedPkts += discardedPkts;

        discardedTiks = discardedTicks;
        discardedTicks = 0;
        totalDiscardedTiks += discardedTiks;

        pktRate = 1000000.0 * ((double) packetCount) / time;
        pktAvgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        if (packetCount == 0 && discardedPkts == 0) {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dumped pkts = 0?/everything? ", pktRate, pktAvgRate);
        }
        else {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dumped pkts = %" PRId64 ",  total %" PRId64 " ",
                     pktRate, pktAvgRate, discardedPkts, totalDiscardedPkts);
        }
        printf(": Dumped Ticks = %" PRId64 ", total = %" PRId64 "\n", discardedTiks, totalDiscardedTiks);

        // Actual Data rates (no header info)
        dataRate = ((double) byteCount) / time;
        dataAvgRate = ((double) currTotalBytes) / totalT;
        printf(" Data:     %3.4g MB/s,  %3.4g Avg, cpu %d\n", dataRate, dataAvgRate, cpu);

        totalRate = ((double) (byteCount + HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + HEADER_BYTES*currTotalPackets)) / totalT;
        printf(" Total:    %3.4g MB/s,  %3.4g Avg\n\n", totalRate, totalAvgRate);

        if (writeToFile) {
            fprintf(fp, "%" PRId64 ",%d,%d,%" PRId64 ",%" PRId64 ",%d\n", totalT/1000000, (int)(pktRate/1000), (int)(dataRate),
                    discardedPkts, totalDiscardedPkts, cpu);
            fflush(fp);
        }

        t1 = t2;
    }

    fclose(fp);
    return (nullptr);
}


// For receiving threads
typedef struct threadRecv_t {
    int  place;
    int  socket;
    int  bufSize;
    bool debug;
    std::shared_ptr<packetRecvStats> stats;
    std::shared_ptr<std::vector<int>> coreIds;
} threadRecv;



void *recvThd(void *arg) {

    threadRecv *tArg = (threadRecv *)arg;
    int place = tArg->place;
    int udpSocket = tArg->socket;
    auto coreIds= tArg->coreIds;
    bool debug = tArg->debug;
    auto stats= tArg->stats;
    int bufSize = tArg->bufSize;
    int tickPrescale = 1;
    ssize_t nBytes;

#ifdef __linux__

    int coreCount = coreIds->size();

    if (coreCount > 0) {

        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

//        for (int i=0; i < coreCount; i++) {
//            int core = (*coreIds)[i];
//            std::cerr << "Run reassembly thread #" << place << " on core " << core << "\n";
//            CPU_SET(core, &cpuset);
//        }
            int core = 80 + place;
            std::cerr << "Run reassembly thread #" << place << " on core " << core << "\n";
            CPU_SET(core, &cpuset);
        }
        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << std::endl;
        }
    }

#endif

    // Start with offset 0 in very first packet to be read
    uint64_t tick;
    uint16_t dataId;

    // If bufSize gets too big, it exceeds stack limits, so lets malloc it!
    char *dataBuf = (char *) malloc(bufSize);
    if (dataBuf == nullptr) {
        fprintf(stderr, "cannot allocate internal buffer memory of %d bytes\n", bufSize);
        exit(1);
    }

    while (true) {

        // Fill with data
        nBytes = getCompletePacketizedBuffer(dataBuf, bufSize, udpSocket,
                                             debug, &tick, &dataId, stats, tickPrescale);
        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
            }
            exit(1);
        }

        // With multiple UDP sockets listening on the same port,
        // ticks may not come in a fixed order, so ignore the order.

        totalBytes   += nBytes;
        totalPackets += stats->acceptedPackets;

        // stats
        discardedTicks   += stats->discardedBuffers;
        discardedBytes   += stats->discardedBytes;
        discardedPackets += stats->discardedPackets;
    }

    return nullptr;
}




int main(int argc, char **argv) {

    int socketCount = 1;
    ssize_t nBytes;
    // Set this to max expected data size
    int bufSize = 1020000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    uint16_t port = 17750;
    bool debug = false;
    bool useIPv6 = false;

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);

    int cores[10];
    for (int & core : cores) {
        core = -1;
    }

    // shared ptr to vector containing cores to run on
    auto coreIds = std::make_shared<std::vector<int>>();

    parseArgs(argc, argv, &bufSize, &recvBufSize,
              &tickPrescale, &socketCount, cores, &port, &debug,
              &useIPv6, listeningAddr, filename);

    int udpSockets[socketCount];

#ifdef __linux__

    if (cores[0] > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        int coreCount = 0;

        for (int i=0; i < 10; i++) {
            if (debug) std::cerr << "core[" << i << "] = " << cores[i] << "\n";
            if (cores[1] > -1) coreCount++;
        }

        coreIds->reserve(coreCount);

        for (int i=0; i < 10; i++) {
            if (cores[i] >= 0) {
                std::cerr << "Run reassembly thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
                (*coreIds)[i] = cores[i];
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
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

    // Start thread to do rate printout
    auto *targ = (threadStruct *)malloc(sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }
    std::memset(targ, 0, sizeof(threadStruct));
    if (strlen(filename) > 0) {
        std::memcpy(targ->filename, filename, sizeof(filename));
    }

    pthread_t thd;
    int status = pthread_create(&thd, nullptr, thread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

#ifdef __linux__
    // Track cpu by calling sched_getcpu roughly once per sec
    int cpuLoops = 50000;
    int loopCount = cpuLoops;
#endif

    // Statistics
    std::shared_ptr<packetRecvStats> stats = std::make_shared<packetRecvStats>();
    discardedTicks = 0;
    discardedPackets = 0;


    // Create all the server threads. They'll be created with the SO_REUSEPORT option
    // and on the same port. The hashing algorithm uses the sender IP & port and the receiver
    // IP & port. This ensures that everything from one sending IP and port arrive at the very same
    // receiver port. Nice for multithreading.

    for (int i=0; i < socketCount; i++) {
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
            if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

            int optval = 1;
            setsockopt(udpSockets[i], SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

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
            fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

            int optval = 1;
            setsockopt(udpSockets[i], SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));

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
            int err = bind(udpSockets[i], (struct sockaddr *) &serverAddr, sizeof(serverAddr));
            if (err != 0) {
                fprintf(stderr, "bind socket error\n");
                return -1;
            }
        }

        //---------------------------------------------------
        // Start thread to read in UDP sockets
        //---------------------------------------------------
        threadRecv *tArg = (threadRecv *) calloc(1, sizeof(threadRecv));
        if (tArg == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        tArg->socket  = udpSockets[i];
        tArg->debug   = debug;
        tArg->place   = i;
        tArg->stats   = stats;
        tArg->bufSize = bufSize;
        tArg->coreIds = coreIds;

        pthread_t thd1;
        status = pthread_create(&thd1, nullptr, recvThd, (void *) tArg);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    fprintf(stderr, "Internal buffer size = %d bytes\n", bufSize);
    sleep(1000000);

    return 0;
}

