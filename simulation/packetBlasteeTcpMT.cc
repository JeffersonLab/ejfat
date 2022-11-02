//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive generated data sent by clasBlaster.c program.
 * This assumes there is an emulator or FPGA between this and the sending program.
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
#include "ejfat_network.hpp"

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
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer byte sizez>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-pri <realtime process priority, default = max>]",
            "        [-f <file for stats>]",
            "        [-s_host <TCP server host>]",
            "        [-s_port <TCP server port>]",
            "        [-s_if <interface (dot-decimal)) to access TCP server thru>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with clasBlaster and send data to TCP server.\n");
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
 * @param rtPriority    filled with realtime priority.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param listenAddr    filled with IP address to listen on.
 * @param filename      filled with name of file in which to write stats.
 * @param server        IP address of TCP server to send buffers to.
 * @param serverPort    port of TCP server to send buffers to.
 * @param serverIP      IP address of interface thru which to talk to TCP server.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      int *cores, uint16_t* port, int *rtPriority,
                      bool *debug, bool *useIPv6,
                      char *listenAddr, char *filename,
                      char *server, uint16_t *serverPort, char *serverIF) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, NULL, 1},
                          {"ip6",  0, NULL, 2},
                          {"cores",  1, NULL, 3},
                          {"pri",  1, NULL, 6},
                          {"s_host",  1, NULL, 7},
                          {"s_port",  1, NULL, 8},
                          {"s_if",  1, NULL, 9},
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

            case 6:
                // Realtime priority
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *rtPriority = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -pri, pri >= 1\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 7:
                // TCP server host
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid arg to -s_host, server host name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(server, optarg);
                break;

            case 8:
                // TCP server port
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *serverPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -s_port, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 9:
                // Dot decimal interface thru which to talk to TCP server
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "Invalid arg to -s_if, dot decimal interface name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(serverIF, optarg);
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
static std::atomic<uint32_t> droppedPackets;
static std::atomic<uint32_t> droppedTicks;
typedef struct threadStruct_t {
    char filename[101];
} threadStruct;


// Thread to send to print out rates
static void *thread(void *arg) {

    uint64_t packetCount, byteCount;
    uint64_t prevTotalPackets, prevTotalBytes;
    uint64_t currTotalPackets, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;


    // File writing stuff
    bool writeToFile = false;
    threadStruct *targ = static_cast<threadStruct *>(arg);
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
        fprintf(fp, "Sec,PacketRate(kHz),DataRate(MB/s),Dropped,TotalDropped,CPU\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate, totalRate, totalAvgRate;
    int64_t totalT = 0, time, droppedPkts, totalDroppedPkts = 0, droppedTiks, totalDroppedTiks = 0;
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
        droppedPkts = droppedPackets;
        droppedPackets.store(0);
        totalDroppedPkts += droppedPkts;

        droppedTiks = droppedTicks;
        droppedTicks.store(0);
        totalDroppedTiks += droppedTiks;

        pktRate = 1000000.0 * ((double) packetCount) / time;
        pktAvgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        if (packetCount == 0 && droppedPkts == 0) {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = 0?/everything? ", pktRate, pktAvgRate);
        }
        else {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = %" PRId64 " ", pktRate, pktAvgRate, droppedPkts);
        }
        printf(": Dropped Ticks = %" PRId64 ", total = %" PRId64 "\n", droppedTiks, totalDroppedTiks);

        // Actual Data rates (no header info)
        dataRate = ((double) byteCount) / time;
        dataAvgRate = ((double) currTotalBytes) / totalT;
        printf(" Data:     %3.4g MB/s,  %3.4g Avg, cpu %d, dropped pkts %" PRId64 ", total %" PRId64 "\n",
               dataRate, dataAvgRate, cpu, droppedPkts, totalDroppedPkts);

        totalRate = ((double) (byteCount + HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + HEADER_BYTES*currTotalPackets)) / totalT;
        printf(" Total:    %3.4g MB/s,  %3.4g Avg\n\n", totalRate, totalAvgRate);

        if (writeToFile) {
            fprintf(fp, "%" PRId64 ",%d,%d,%" PRId64 ",%" PRId64 ",%d\n", totalT/1000000, (int)(pktRate/1000), (int)(dataRate),
                    droppedPkts, totalDroppedPkts, cpu);
            fflush(fp);
        }

        t1 = t2;
    }

    fclose(fp);
    return (NULL);
}




// Arg to pass to buffer sending thread
typedef struct threadArg_t {
    /** Supply holding buffers of reassembled data. */
    std::shared_ptr<ejfat::BufferSupply> supply;

    /** Byte size of buffers contained in supply. */
    int bufferSize;

    bool debug;

    uint64_t expectedTick;
    uint32_t tickPrescale;

    int core;
    int tcpSocket;

} threadArg;


/**
 * Thread to send already assembled buffers to a TCP server.
 * @param arg   struct to be passed to thread.
 */
// Thread to parse stored packets for 1 SOURCE ID / ENTROPY VALUE
static void *sendingThread(void *arg) {

    threadArg *tArg = (threadArg *) arg;
    std::shared_ptr<ejfat::BufferSupply> supply = tArg->supply;
    bool debug    = tArg->debug;
    int core      = tArg->core;
    int tcpSocket = tArg->tcpSocket;

#ifdef __linux__

    if (core > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        std::cerr << "Run assemble reading thd on core " << core << "\n";
        CPU_SET(core, &cpuset);

        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }


#endif

    while (true) {
        //------------------------------------------------------
        // Get item (filled buffer) from supply
        //------------------------------------------------------
        auto item = supply->consumerGet();
        // Get reference to item's ByteBuffer object
        auto itemBuf = item->getBuffer();
        // Get reference to item's byte array (underlying itemBuf)
        char *buffer = reinterpret_cast<char *>(itemBuf->array());
        int nBytes = (int)(item->getUserLong());

        // We first send the size of the buffer in bytes (in BIG endian).
        //fprintf(stderr, "Sending %d bytes\n", bytes);
        //bytes = htonl(bytes);

        if (tcpWrite(tcpSocket, (void *) &nBytes, 4) != 4) {
            close(tcpSocket);
            fprintf(stderr, "Error writing size\n");
            perror("bad write");
            return nullptr;
        }

        // Then we send the buffer itself
        if (tcpWrite(tcpSocket, (void *) buffer, nBytes) != nBytes) {
            close(tcpSocket);
            fprintf(stderr, "Error writing buffer\n");
            perror("bad buf write");
            return nullptr;
        }

        supply->release(item);
    }

    return nullptr;
}




int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    // Set this to max expected data size
    int bufSize = 1020000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    int rtPriority = 0;
    uint16_t port = 7777;
    uint16_t serverPort = 8888;
    int cores[10];
    bool debug = false;
    bool useIPv6 = false;

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);
    char server[101];
    memset(server, 0, 101);
    char interface[16];
    memset(interface, 0, 16);
    bool sendToServer = false;

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, cores, &port, &rtPriority, &debug,
              &useIPv6, listeningAddr, filename, server, &serverPort, interface);


    std::cerr << "Tick prescale = " << tickPrescale << "\n";


#ifdef __linux__

    if (cores[0] > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        if (debug) {
            for (int i=0; i < 10; i++) {
                     std::cerr << "core[" << i << "] = " << cores[i] << "\n";
            }
        }

        for (int i=0; i < 10; i++) {
            if (cores[i] >= 0) {
                std::cerr << "Run reassembly thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
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

    // Do we connect to a TCP server and send the data there?
    if (strlen(server) > 0 && serverPort > 1023) {
        sendToServer = true;
    }

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
        if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

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
        fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

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
            return -1;
        }
    }

    std::cerr << "Tick prescale = " << tickPrescale << "\n";


    int tcpSocket = 0;
    std::shared_ptr<ejfat::BufferSupply> supply = nullptr;


    if (sendToServer) {
        //---------------------------------------------------
        // Supply in which each buf will hold reconstructed buffer.
        // Make these buffers sized as given on command line (100kB default) and expand as necessary.
        //---------------------------------------------------
        int ringSize = 16384;
        supply = std::make_shared<ejfat::BufferSupply>(ringSize, bufSize, ByteOrder::ENDIAN_LOCAL, true);

            // Connect to server
            if (tcpConnect(server, interface, (unsigned short) serverPort,
                           recvBufSize, 0, true, &tcpSocket, NULL) != 0) {
                fprintf(stderr, "cannot connect to TCP server\n");
                return -1;
            }

        //---------------------------------------------------
        // Start thread to send buffers to TCP server
        //---------------------------------------------------
        threadArg *tArg2 = (threadArg *) calloc(1, sizeof(threadArg));
        if (tArg2 == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        tArg2->supply    = supply;
        tArg2->debug     = debug;
        tArg2->tcpSocket = tcpSocket;

        pthread_t thd1;
        int status = pthread_create(&thd1, NULL, sendingThread, (void *) tArg2);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }


    //---------------------------------------------------
    // Start thread to do rate printout
    //---------------------------------------------------
    threadStruct *targ = (threadStruct *)malloc(sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }
    std::memset(targ, 0, sizeof(threadStruct));
    if (strlen(filename) > 0) {
        std::memcpy(targ->filename, filename, sizeof(filename));
    }

    pthread_t thd;
    int status = pthread_create(&thd, NULL, thread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint16_t dataId;

    // If bufSize gets too big, it exceeds stack limits, so lets malloc it!
    char *dataBuf = (char *) malloc(bufSize);
    if (dataBuf == NULL) {
        fprintf(stderr, "cannot allocate internal buffer memory of %d bytes\n", bufSize);
        return -1;
    }

    fprintf(stderr, "Internal buffer size = %d bytes\n", bufSize);

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;

    // Track cpu by calling sched_getcpu roughly once per sec
    int cpuLoops = 50000;
    int loopCount = cpuLoops;

    // Statistics
    std::shared_ptr<packetRecvStats> stats = std::make_shared<packetRecvStats>();
    droppedTicks.store(0);
    droppedPackets.store(0);


    std::cerr << "Tick prescale = " << tickPrescale << "\n";

    while (true) {

        clearStats(stats);
        uint64_t diff, prevTick = tick;

        // Get buffer from supply
        auto item = supply->get();
        // Get reference to item's ByteBuffer object
        auto itemBuf = item->getClearedBuffer();
        // Get reference to item's byte array (underlying itemBuf)
        char *buffer = reinterpret_cast<char *>(itemBuf->array());
        size_t bufCapacity = itemBuf->capacity();

        // Fill with data
        nBytes = getCompletePacketizedBuffer(buffer, bufCapacity, udpSocket,
                                             debug, &tick, &dataId, stats,
                                             tickPrescale, outOfOrderPackets);
        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
            }

            close(udpSocket);

            if (sendToServer && tcpSocket > 0) {
                close(tcpSocket);
            }
            return (0);
        }

//        fprintf(stderr, "Received buffer of %d bytes, tpre %d\n", (int)nBytes, tickPrescale);
//
        diff = tick - prevTick;
        if (diff != 0) {
            fprintf(stderr, "Error in tick increment, %" PRIu64 ", tick = %" PRIu64 "\n", diff, tick);
        }

        totalBytes   += nBytes;
        totalPackets += stats->acceptedPackets;

        // atomic
        droppedTicks   += stats->droppedTicks;
        droppedPackets += stats->droppedPackets;

        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale;

#ifdef __linux__
        if (loopCount-- < 1) {
            cpu = sched_getcpu();
            loopCount = cpuLoops;
        }
#endif

        // Send to server if desired
        if (sendToServer) {
            // Store data len in bytes
            item->setUserLong(((long)nBytes & 0xffffffffL));
            // Make available to the sender thread
            supply->publish(item);
        }
        else {
            // Return buffer to supply for reuse
            supply->release(item);
        }

    }

    return 0;
}

