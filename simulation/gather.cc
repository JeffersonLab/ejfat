//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive locally generated data sent by scatter.c program.
 * This program handles sequentially numbered packets that may arrive out-of-order.
 * This assumes there is an emulator or FPGA between this and the sending program.
 */

#include <pthread.h>
#include <time.h>
#include <queue>

#include <cstdio>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <thread>
#include <chrono>
#include <cinttypes>


#include <iostream>
#include "ByteBuffer.h"
#include "BufferSupply.h"
#include "BufferSupplyItem.h"

#include "ejfat_assemble_ersap.hpp"

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
            "        [-p <starting UDP port>]",
            "        [-b <internal buffer byte size>]",
            "        [-c <buffer count / ring>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-i <id count>]",
            "        [-repeat (repeatedly send same data in loop)]",
            "        [-f <output filename>]");

    fprintf(stderr, "        EJFAT UDP packet receiver that reads a file that the packetizer sends over multiple ports\n");
    fprintf(stderr, "        with a separate thread for each id to reassemble. Data can be repeatedly read.\n");
    fprintf(stderr, "        If read only once, it will write to file.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param bufSize       filled with buffer size.
 * @param recvBufSize   filled with UDP receive buffer size.
 * @param startingPort  filled with the first UDP port to listen on.
 * @param idCount       filled with number of data sources.
 * @param count         filled with number of bufs / ring.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param repeat        filled with repeated reading flag (to be used with same flag on sending end).
 * @param fileName      filled with output file name.
 * @param listenAddr    filled with IP address to listen on.
 */
static void parseArgs(int argc, char **argv, int* bufSize, int *recvBufSize,
                      uint16_t* startingPort, int *idCount, int *count,
                      bool *debug, bool *useIPv6, bool *repeat,
                      char *fileName, char *listenAddr) {

    int c, i_tmp;
    bool help = false, gotFileName = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"ip6",  0, NULL, 1},
             {"repeat",  0, NULL, 2},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:i:f:c:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *startingPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
                    exit(-1);
                }
                break;

            case 'i':
                // ID count
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *idCount = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -i, idCount > 0\n");
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
                // File to send
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -f, file name is too long\n");
                    exit(-1);
                }
                gotFileName = true;
                strcpy(fileName, optarg);
                break;

            case 'c':
                // Buffers / ring                                                                                                                                                          count
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *count = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -c, count > 0\n");
                    exit(-1);
                }
                break;

            case 1:
                // use IP version 6
                *useIPv6 = true;
                break;

            case 2:
                // repeat
                *repeat = true;
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


// Quantities to be shared by each sending thread
static uint32_t offset = 0,sendBufSize = 0;
static bool debug = false;
static char filename[INPUT_LENGTH_MAX];



// Arg to pass to threads
struct threadArg {
    int id;
    int bufferSize;
    int socket;
    bool repeat;
    std::shared_ptr<ejfat::BufferSupply> supply;
};


// Thread to send to gather application
static void *thread(void *arg) {

    struct threadArg *tArg = (struct threadArg *) arg;
    int id = tArg->id;
    int bufSize = tArg->bufferSize;
    int udpSocket = tArg->socket;
    bool repeat = tArg->repeat;
    std::shared_ptr<ejfat::BufferSupply> supply = tArg->supply;

    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    // If bufSize gets too big, it exceeds stack limits, so lets malloc it!

    fprintf(stderr, "Internal buffer size = %d bytes\n", bufSize);

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;

    // Statistics
    uint32_t packetCount=0, tickPrescale = 1;
    int64_t packetsRead=0, byteCount=0, totalBytes=0, totalPackets=0;
    uint16_t dataId;

    std::shared_ptr<packetRecvStats> stats = std::make_shared<packetRecvStats>();

    while (true) {

        // Get buffer from supply
        auto item = supply->get();
        std::shared_ptr<ByteBuffer> buf = item->getClearedBuffer();
        clearStats(stats);

        // Fill with data
        ssize_t nBytes = getCompletePacketizedBuffer((char *)buf->array(), bufSize, udpSocket,
                                                      debug, &tick, &dataId, stats, tickPrescale,
                                                      outOfOrderPackets);
        if (nBytes < 0) {
            if (debug) {
                if (nBytes == BUF_TOO_SMALL) {
                    fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
                }
                else {
                    fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
                }
            }
            return (NULL);
        }

        if (stats->droppedPackets != 0) {
            fprintf(stderr, "Dropped at least %" PRIu64 " packets\n", stats->droppedPackets);
        }

        // Send it to consumer
        buf->limit(nBytes);
        // Store number of bytes in user int (not including headers)
        int32_t* userInts = item->getUserInts();
        userInts[0] = nBytes;
        supply->publish(item);

//        byteCount   += nBytes;
//        packetsRead += stats->acceptedPackets;

        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale;

        if (!repeat) {
            break;
        }

        outOfOrderPackets.clear();
    }

    return (NULL);
}




// Statistics
static volatile uint64_t totalBytes=0;


// Thread to send to print out rates
static void *rateThread(void *arg) {

    uint64_t byteCount, prevTotalBytes, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    double rate, avgRate;
    int64_t totalT = 0, time;
    struct timespec t1, t2;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);

    while (true) {

        prevTotalBytes = totalBytes;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);

        currTotalBytes = totalBytes;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalBytes > 0) {
                skipFirst = false;
            }
            t1 = t2;
            totalT = totalBytes = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount = currTotalBytes - prevTotalBytes;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = 0;
            t1 = t2;
            continue;
        }

        // Rates
        time = 1000000L * (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec)/1000L;
        totalT += time;

        // Actual Data rates (no header info)
        rate = ((double) byteCount) / time;
        avgRate = ((double) currTotalBytes) / totalT;
        // Must print out t to keep it from being optimized away
        printf(" Data:    %3.4g MB/s,  %3.4g Avg\n\n", rate, avgRate);

        clock_gettime(CLOCK_MONOTONIC, &t1);
    }

    return (NULL);
}




int main(int argc, char **argv) {

    int idCount, ringSize = 4;
    // Set this to max expected data size
    int bufSize = 1020000;
    int recvBufBytes = 0, recvBufSize = 0;
    uint16_t startingPort = 7777;

    bool repeat = false;
    bool useIPv6 = false;

    char fileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(fileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    parseArgs(argc, argv, &bufSize, &recvBufSize, &startingPort, &idCount, &ringSize,
              &debug, &useIPv6, &repeat, fileName, listeningAddr);

    std::shared_ptr<ejfat::BufferSupply> supplies[idCount];

    for (int i = 0; i < idCount; i++) {

        int udpSocket;

        supplies[i] = std::make_shared<ejfat::BufferSupply>(ringSize, bufSize);

        // Fall back on defaults if not set by user
        if (recvBufSize == 0) {
#ifdef __APPLE__
            // By default set recv buf size to 7.4 MB which is the highest
            // it wants to go before before reverting back to 787kB.
            recvBufBytes = 7400000;
#else
            // By default set recv buf size to 25 MB
            recvBufBytes = 25000000;
#endif
        }
        else {
            recvBufBytes = recvBufSize;
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
            setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0;
            getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

            // Configure settings in address struct
            // Clear it out
            memset(&serverAddr6, 0, sizeof(serverAddr6));
            // it is an INET address
            serverAddr6.sin6_family = AF_INET6;
            // the port we are going to receiver from, in network byte order
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
            setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0;
            getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

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
        }

        // Thread to send part of buffer to gather application
        struct threadArg *arg = (threadArg *) malloc(sizeof(struct threadArg));
        if (arg == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        // Place for incoming data will be a supply of buffers (disruptor's ring buffer)
        arg->supply = supplies[i];
        arg->socket = udpSocket;
        arg->id = i;
        arg->repeat = repeat;
        arg->bufferSize = bufSize;

        pthread_t thd;
        int status = pthread_create(&thd, NULL, thread, (void *) arg);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    pthread_t rthd;
    int status = pthread_create(&rthd, NULL, rateThread, nullptr);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating rate thread\n\n");
        return -1;
    }

    // Open output file

    FILE *fp = nullptr;
    if (strlen(fileName) > 0) {
        fp = fopen (fileName, "w");
        // validate file open for writing
        if (!fp) {
            fprintf(stderr, "file open failed: %s\n", strerror(errno));
            return 1;
        }
    }

    // Statistics

    while (true) {

        // Now rejoin, thru the ring buffers, everything collected in each UDP socket
        for (int i = 0; i < idCount; i++) {

            // Get buffer from supply that a receiving thread wrote data into
//std::cout << "       Get supply from ring = " << i << std::endl;
            auto item = supplies[i]->consumerGet();

            // Get data
//std::cout << "       Got buffer"  << std::endl;
            auto byteBuf = item->getBuffer();
//std::cout << "       client got " << (item->getUserInt()) << " bytes" << ", should be same as lim "
//          << byteBuf->limit() << std::endl;
            totalBytes += item->getUserInts()[0];

            // Write out what was received
            if (fp != nullptr) {
std::cout << "       writing data to file!" << byteBuf->limit() << std::endl;
                writeBuffer((char *) (byteBuf->array()), byteBuf->limit(), fp, debug);
            }

            // Send it to back to supply for reuse
            supplies[i]->release(item);
        }

        if (!repeat || fp != nullptr) {
            break;
        }
    }

    printf(" EXIT LOOP\n");

    if (fp != nullptr) {
        fclose(fp);
    }

    // Don't let this thread end?
    sleep(4);

    return 0;
}

