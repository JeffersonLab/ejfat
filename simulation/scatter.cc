//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * <p>
 * @file Take a file (read or piped to), split it into multiple buffers,
 * each split associated with a specific data id, and send them to an ejfat router
 * which then passes it to gather.cc to reassemble. Each id-associated buffer
 * will be sent to a different port but same host.
 */


#include <cstdlib>
#include <stdio.h>
#include <time.h>
#include <cmath>
#include <pthread.h>
#include <unistd.h>
#include <cinttypes.h>
#include "ejfat_packetize.hpp"

using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-repeat] [ip6]",
            "        [-host <host of FPGA or emulator]",
            "        [-port <port of FPGA or emulator]",
            "        [-f <file to split and send>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
            "        [-mtu <desired MTU size>]",
            "        [-t <tick>]",
            "        [-ver <version>]",
            "        [-id <starting data id>]",
            "        [-n <number of ids/senders>]",
            "        [-pro <protocol>]",
            "        [-e <starting entropy #>]",
            "        [-s <UDP send buffer size>]",
            "        [-repeat (repeatedly send same data in loop)]",
            "        [-dpre <delay prescale (2 skips every other delay)>]",
            "        [-d <delay in MICROsec between packets>]");

    fprintf(stderr, "        EJFAT UDP packet sender that will read a file and divide it into 1 chunk per data id\n");
    fprintf(stderr, "        with a separate thread for each id to packetize & send its data. Data can be repeatedly sent.\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *startingEntropy, int *version, uint16_t *startingId,
                      uint64_t* tick, uint32_t* delay,
                      uint32_t *delayPrescale, uint32_t *sendBufSize,
                      uint32_t *idCount, uint16_t *port,
                      bool *debug, bool *repeat, bool *useIPv6,
                      char* host, char *interface, char *file) {

    *mtu = 0;
    int c, i_tmp;
    int64_t tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"mtu",   1, NULL, 1},
             {"host",  1, NULL, 2},
             {"ver",   1, NULL, 3},
             {"id",    1, NULL, 4},
             {"pro",   1, NULL, 5},
             {"dpre",  1, NULL, 6},
             {"port",  1, NULL, 7},
             {"repeat",  0, NULL, 8},
             {"ip6",  0, NULL, 9},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhi:t:d:s:n:f:e:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 't':
                // TICK
                tmp = strtoll(optarg, nullptr, 0);
                if (tmp > -1) {
                    *tick = tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -t, tick > 0\n");
                    exit(-1);
                }
                break;

            case 's':
                // UDP SEND BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 100000) {
                    *sendBufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -s, UDP send buf size >= 100kB\n");
                    exit(-1);
                }
                break;

            case 'e':
                // ENTROPY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0) {
                    fprintf(stderr, "Invalid argument to -e. Entropy must be >= 0\n");
                    exit(-1);
                }
                *startingEntropy = i_tmp;
                break;

            case 'd':
                // DELAY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *delay = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -d, packet delay > 0\n");
                    exit(-1);
                }
                break;

            case 'n':
                // Number of ids
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *idCount = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -n, id count > 0\n");
                    exit(-1);
                }
                break;

            case 'i':
                // OUTGOING INTERFACE NAME / IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "interface address is bad\n");
                    exit(-1);
                }
                strcpy(interface, optarg);
                break;

            case 'f':
                // File to send
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -f, file name is too long\n");
                    exit(-1);
                }
                strcpy(file, optarg);
                break;

            case 1:
                // MTU
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 100) {
                    fprintf(stderr, "Invalid argument to -mtu. MTU buffer size must be > 100\n");
                    exit(-1);
                }
                *mtu = i_tmp;
                break;

            case 2:
                // LB HOST
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -host, host name is too long\n");
                    exit(-1);
                }
                strcpy(host, optarg);
                break;

            case 7:
                // LB Port
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 1024 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -port. Port must be >= 1024 and < 65536\n");
                    exit(-1);
                }
                *port = i_tmp;
                break;

            case 3:
                // VERSION
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 31) {
                    fprintf(stderr, "Invalid argument to -ver. Version must be >= 0 and < 32\n");
                    exit(-1);
                }
                *version = i_tmp;
                break;

            case 4:
                // Starting DATA_ID
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -id. Id must be >= 0 and < 65536\n");
                    exit(-1);
                }
                *startingId = i_tmp;
                break;

            case 5:
                // PROTOCOL
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0) {
                    fprintf(stderr, "Invalid argument to -pro. Protocol must be >= 0\n");
                    exit(-1);
                }
                *protocol = i_tmp;
                break;

            case 6:
                // Delay prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *delayPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -dpre, pdre >= 1\n");
                    exit(-1);
                }
                break;

            case 8:
                // REPEAT
                *repeat = true;
                break;

            case 9:
                // IP version 6
                *useIPv6 = true;
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
static uint32_t delay = 0, delayPrescale = 1, sendBufSize = 0, idCount = 1, maxUdpPayload;
static uint16_t lbPort = 0x4c42; // FPGA port is default
static uint64_t tick = 0;
static int mtu, version = 2, protocol = 1, startingEntropy = 0;
static uint16_t startingId = 1;
static bool debug = true, repeat = false, useIPv6 = false;
static char lbHost[INPUT_LENGTH_MAX], filename[INPUT_LENGTH_MAX], interface[16];


// Arg to pass to threads
struct threadArg {
    int id;
    int bufferSize;
    int socket;
    int entropy;
    bool repeat;
    char *buffer;
};


// Thread to send to gather application
static void *thread(void *arg) {

    struct threadArg *tArg = (struct threadArg *) arg;
    int id = tArg->id;
    int bufSize = tArg->bufferSize;
    int clientSocket = tArg->socket;
    int entropy = tArg->entropy;
    char *buf = tArg->buffer;
    uint32_t offset = 0;

    int err;
    bool firstBuffer = true;
    bool lastBuffer  = true;
    uint32_t delayCounter = delayPrescale;

    //fprintf(stdout, "spins = %u, currentSpins = %u\n", spins, currentSpins);
    if (debug) fprintf(stderr, "new thread: id = %d, entropy = %d, delay = %d\n\n", id, entropy, delay);

    // Statistics
    int64_t packetsSent=0, packetCount=0, byteCount=0, totalBytes=0, totalPackets=0;
    double rate = 0.0, avgRate = 0.0;
    int64_t totalT = 0, time, time1, time2;
    struct timespec t1, t2;

    // Get the current time
    clock_gettime(CLOCK_REALTIME, &t1);
    time1 = 1000L*t1.tv_sec + t1.tv_nsec/1000000L; // milliseconds

    while (true) {
//fprintf(stderr, "send buf: tick = %llu, id = %d, entropy = %d\n\n", tick, id, entropy);
        err = sendPacketizedBufferFast(buf, bufSize,
                                       maxUdpPayload, clientSocket,
                                       tick, protocol, entropy, version, id, &offset,
                                       delay, delayPrescale, &delayCounter,
                                       firstBuffer, lastBuffer, false, &packetsSent);

        if (err < 0) {
            // Should be more info in errno
            EDESTADDRREQ;
            fprintf(stderr, "\nsendPacketizedBuffer: errno = %d, %s\n\n", errno, strerror(errno));
            exit(1);
        }

//        // delay if any
//        if (delay > 0) {
//            if (--delayPrescaleCounter < 1) {
//                std::this_thread::sleep_for(std::chrono::microseconds(delay));
//                delayPrescaleCounter = delayPrescale;
//            }
//        }

        // One and done
        if (!repeat) {
            if (debug) fprintf(stderr, "new thread id %d: break since repeat is false\n", id);
            break;
        }

        byteCount   += bufSize;
        packetCount += packetsSent;
        offset = 0;
        tick++;

        // stats
        clock_gettime(CLOCK_REALTIME, &t2);
        time2 = 1000L*t2.tv_sec + t2.tv_nsec/1000000L; /* milliseconds */
        time = time2 - time1;

        if (time > 5000) {
            // reset things if #s rolling over
            if ( (totalBytes < 0) || (totalT < 0) )  {
                totalT = totalBytes = totalPackets = byteCount = packetCount = 0;
                time1 = time2;
                continue;
            }

            /* Packet rates */
            totalT += time;
//            rate = 1000.0 * ((double) packetCount) / time;
//            totalPackets += packetCount;
//            avgRate = 1000.0 * ((double) totalPackets) / totalT;
//printf(" Packets %d:  %3.4g Hz,    %3.4g Avg.\n", id, rate, avgRate);

            /* Actual Data rates (no header info) */
            rate = ((double) byteCount) / (1000*time);
            totalBytes += byteCount;
            avgRate = ((double) totalBytes) / (1000*totalT);
            printf(" Data %d:    %3.4g MB/s,  %3.4g Avg.\n\n", id, rate, avgRate);

            byteCount = packetCount = 0;

            clock_gettime(CLOCK_REALTIME, &t1);
            time1 = 1000L*t1.tv_sec + t1.tv_nsec/1000000L;
        }
    }

    return (NULL);
}


// Statistics
static volatile uint64_t totalBytes=0, totalPackets=0;


// Thread to send to print out rates
static void *rateThread(void *arg) {

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
        printf(" Packets:  %3.4g Hz,    %3.4g Avg, time = %" PRId64 " microsec\n", rate, avgRate, time);

        // Actual Data rates (no header info)
        rate = ((double) byteCount) / time;
        avgRate = ((double) currTotalBytes) / totalT;
        // Must print out t to keep it from being optimized away
        printf(" Data:    %3.4g MB/s,  %3.4g Avg\n\n", rate, avgRate);

        clock_gettime(CLOCK_MONOTONIC, &t1);
    }


    return (NULL);
}




/**
 * Doing things this way is like reading a buffer bit-by-bit
 * and passing it off to the parser bit-by-bit
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {

    memset(lbHost, 0, INPUT_LENGTH_MAX);
    memset(filename, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    strcpy(lbHost, "127.0.0.1");
    strcpy(interface, "lo0");

    parseArgs(argc, argv, &mtu, &protocol, &startingEntropy, &version, &startingId, &tick,
              &delay, &delayPrescale, &sendBufSize, &idCount, &lbPort,
              &debug, &repeat, &useIPv6, lbHost, interface, filename);

    fprintf(stderr, "File name = %s\n", filename);

    // Open file
    size_t fileSize = 0L;
    FILE *fp = fopen(filename, "r");
    if (fp == nullptr) {
        fprintf(stderr, "\n ******* file %s, does not exist or cannot open\n\n", filename);
        return -1;
    }

    // Find the size of the file
    fseek(fp, 0L, SEEK_END);
    fileSize = ftell(fp);
    rewind(fp);

    // There must be limits on file size (12MB) as we read it into memory
    if (fileSize > 12000000) {
        fprintf(stderr, "\n ******* file %s is too big, pick something < 10MB\n\n", filename);
        return -1;
    }

    // How many bytes of original buffer does each thread (id) send?
    // Note: the last id may send a little less.
    size_t partialBufSize = fileSize / idCount + 1;
    fprintf(stderr, "\n ******* file %s is %lu bytes, split into %d parts, each %lu\n", filename, fileSize, idCount, partialBufSize);
    size_t bytesToWrite, remainingBytes = fileSize;

    // TODO: make adjustments for last buffer's length !!!

    // Break data into multiple packets of max MTU size.
    // If the mtu was not set on the command line, get it progamatically
    if (mtu == 0) {
        mtu = getMTU(interface, debug);
    }

    // Jumbo (> 1500) ethernet frames are 9000 bytes max.
    // Don't exceed this limit.
    if (mtu > 9000) {
        mtu = 9000;
    }

    // 20 bytes = normal IPv4 packet header (60 is max), 8 bytes = max UDP packet header
    // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
    maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;

    if (debug) fprintf(stderr, "Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

    // Start up threads to do the sending
    for (int i=0; i < idCount; i++) {
        // Create UDP socket
        int clientSocket;

        if (useIPv6) {

            struct sockaddr_in6 serverAddr6;

            /* create a DGRAM (UDP) socket in the INET/INET6 protocol */
            if ((clientSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                return -1;
            }

            socklen_t size = sizeof(int);
            int sendBufBytes = 0;
#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            sendBufBytes = sendBufSize <= 0 ? 25000000 : sendBufSize;
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif
            sendBufBytes = 0; // clear it
            getsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket send buffer = %d bytes\n", sendBufBytes);

            // Configure settings in address struct
            // Clear it out
            memset(&serverAddr6, 0, sizeof(serverAddr6));
            // it is an INET address
            serverAddr6.sin6_family = AF_INET6;
            // the port we are going to send to, in network byte order
            serverAddr6.sin6_port = htons(lbPort);
            // the server IP address, in network byte order
            inet_pton(AF_INET6, lbHost, &serverAddr6.sin6_addr);

            int err = connect(clientSocket, (const sockaddr *)&serverAddr6, sizeof(struct sockaddr_in6));
            if (err < 0) {
                if (debug) perror("Error connecting UDP socket:");
                close(clientSocket);
                exit(1);
            }

        } else {

            struct sockaddr_in serverAddr;

            // Create UDP socket
            if ((clientSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                return -1;
            }

            // Try to increase send buf size to 25 MB
            socklen_t size = sizeof(int);
            int sendBufBytes = 0;
#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            sendBufBytes = sendBufSize <= 0 ? 25000000 : sendBufSize;
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif
            sendBufBytes = 0; // clear it
            getsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket send buffer = %d bytes\n", sendBufBytes);

            // Configure settings in address struct
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
//if (debug) fprintf(stderr, "Sending on UDP port %hu\n", lbPort);
            serverAddr.sin_port = htons(lbPort);
//if (debug) fprintf(stderr, "Connecting to host %s\n", lbHost);
            serverAddr.sin_addr.s_addr = inet_addr(lbHost);
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            int err = connect(clientSocket, (const sockaddr *)&serverAddr, sizeof(struct sockaddr_in));
            if (err < 0) {
                if (debug) perror("Error connecting UDP socket:");
                close(clientSocket);
                exit(1);
            }
        }

        // set the don't fragment bit
#ifdef __linux__
        {
            int val = IP_PMTUDISC_DO;
            setsockopt(clientSocket, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
        }
#endif

        // Thread to send part of buffer to gather application
        struct threadArg *arg = (threadArg *) malloc(sizeof(struct threadArg));
        if (arg == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            return -1;
        }

        // How many bytes do we write in this thread?
        // We try to write partialBufSize, unless there are less
        bytesToWrite = remainingBytes > partialBufSize ? partialBufSize : remainingBytes;
        remainingBytes -= bytesToWrite;

        arg->socket = clientSocket;
        arg->id = startingEntropy + i;
        arg->bufferSize = bytesToWrite;
        arg->entropy = startingEntropy + i;
        arg->repeat = repeat;

        if (debug) fprintf(stderr, "\nStart thread id = %d, entropy = %d, bytes to write = %lu, remaining = %lu\n\n",
                           arg->id, arg->entropy, bytesToWrite, remainingBytes);

        // Copy data to be sent
        arg->buffer = (char *)malloc(bytesToWrite);
        if (arg->buffer == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            return -1;
        }

        size_t nBytes = fread(arg->buffer, 1, bytesToWrite, fp);
        if (nBytes != bytesToWrite) {
            // Error in reading
            perror("Error reading file: ");
            return -1;
        }

        pthread_t thd;
        int status = pthread_create(&thd, NULL, thread, (void *) arg);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
    }

    // Don't let this thread end? 2.75 hours
    sleep(10000);

    return 0;
}

