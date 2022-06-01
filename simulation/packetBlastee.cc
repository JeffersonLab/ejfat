//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive locally generated data sent by udp_send.c program.
 * This program handles sequentially numbered packets that may arrive out-of-order.
 * This assumes there is an emulator or FPGA between this and the sending program.
 */

#include <cstdlib>
#include <time.h>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>

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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer byte sizez>]",
            "        [-r <UDP receive buffer byte size>]",
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
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param listenAddr    filled with IP address to listen on.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      uint16_t* port, bool *debug,
                      char *listenAddr) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, NULL, 1},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:", long_options, 0)) != EOF) {

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
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
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
static std::atomic<uint32_t> dropped;


// Thread to send to print out rates
static void *thread(void *arg) {

    uint64_t packetCount, byteCount;
    uint64_t prevTotalPackets, prevTotalBytes;
    uint64_t currTotalPackets, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    double rate, avgRate;
    int64_t totalT = 0, time;
    struct timespec t1, t2;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);

    while (true) {

        prevTotalBytes   = totalBytes;
        prevTotalPackets = totalPackets;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);

        currTotalBytes   = totalBytes;
        currTotalPackets = totalPackets;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalPackets > 0) {
                skipFirst = false;
            }
            t1 = t2;
            totalT = totalBytes = totalPackets = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount   = currTotalBytes   - prevTotalBytes;
        packetCount = currTotalPackets - prevTotalPackets;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = totalPackets = 0;
            t1 = t2;
            continue;
        }

        // Packet rates
        time = 1000000L * (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec)/1000L;
        totalT += time;

        rate = 1000000.0 * ((double) packetCount) / time;
        avgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        if (dropped == 0) {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped = 0?/everything?, time = %lld microsec\n", rate, avgRate, time);
        }
        else {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped = %u, time = %lld microsec\n", rate, avgRate, dropped.load(), time);
        }

        // Actual Data rates (no header info)
        rate = ((double) byteCount) / time;
        avgRate = ((double) currTotalBytes) / totalT;
        // Must print out t to keep it from being optimized away
        printf(" Data:    %3.4g MB/s,  %3.4g Avg\n\n", rate, avgRate);

        dropped.store(0);

        clock_gettime(CLOCK_MONOTONIC, &t1);
    }

    return (NULL);
}



int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    // Set this to max expected data size
    int bufSize = 1020000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    uint16_t port = 7777;
    bool debug = false;

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, &port, &debug, listeningAddr);

    // Create UDP socket
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);

#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif
    setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
    // Read back what we supposedly set
    socklen_t size = sizeof(int);
    int recvBufBytes = 0;
    getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
    fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

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
        fprintf(stderr, "bind socket error\n");
        return -1;
    }

    // Start thread to do rate printout
    pthread_t thd;
    int status = pthread_create(&thd, NULL, thread, (void *) nullptr);
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

    // Statistics
    packetRecvStats stats;
    dropped.store(0);

    while (true) {

        clearStats(&stats);

        // Fill with data
        nBytes = getCompletePacketizedBuffer(dataBuf, bufSize, udpSocket,
                                             debug, &tick, &dataId, &stats,
                                             tickPrescale, outOfOrderPackets);
        if (nBytes < 0) {
            if (debug) {
                if (nBytes == BUF_TOO_SMALL) {
                    fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
                }
                else {
                    fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
                }
            }
            return (0);
        }

//        if (stats.droppedPackets != 0) {
//            fprintf(stderr, "Dropped %llu pkts\n", stats.droppedPackets);
//        }

        totalBytes   += nBytes;
        totalPackets += stats.acceptedPackets;
        // atomic
        dropped += stats.droppedPackets;

        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale;
    }

    return 0;
}

