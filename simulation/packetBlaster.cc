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
 * @file Send file (read or piped to) to an ejfat router (FPGA-based or simulated)
 * which then passes it to a program to reassemble (possibly packetBlastee.cc).
 * This sender, by default, prepends an LB header to the data in order
 * to test it with the receiver. This can be removed in the ejfat_packetize.hpp
 * file by commenting out:
 * </p>
 * <b>#define ADD_LB_HEADER 1</b>
 */


#include <cstdlib>
#include <time.h>
#include <cmath>
#include <thread>
#include "ejfat_packetize.hpp"

using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-sendto] [-sendmsg] [-sendnocp]",
            "        [-host <destination host (defaults to 127.0.0.1)>]",
            "        [-p <destination UDP port>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
            "        [-mtu <desired MTU size>]",
            "        [-t <tick>]",
            "        [-ver <version>]",
            "        [-id <data id>]",
            "        [-pro <protocol>]",
            "        [-e <entropy>]",
            "        [-b <buffer size>]",
            "        [-s <UDP send buffer size>]",
            "        [-tpre <tick prescale (1,2, ... tick increment each buffer sent)>]",
            "        [-dpre <delay prescale (1,2, ... if -d defined, 1 delay for every prescale packets)>]",
            "        [-d <delay in microsec between packets>]");

    fprintf(stderr, "        EJFAT UDP packet sender that will packetize and send buffer repeatedly and get stats\n");
    fprintf(stderr, "        By default, data is copied into buffer and \"send()\" is used (connect is called).\n");
    fprintf(stderr, "        Using -sendto flag, data is copied into buffer and \"sendto()\" is used (connect NOT called)\n");
    fprintf(stderr, "        Using -sendmsg flag, data is sent using \"sendmsg()\" which allows the elimination of a data copy.\n");
    fprintf(stderr, "        Using -sendnocp flag, data is sent using \"send()\" (connect called) and data copy minimized, but original data buffer changed\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *entropy, int *version, uint16_t *id, uint16_t* port,
                      uint64_t* tick, uint32_t* delay,
                      uint32_t *bufsize, uint32_t *sendBufSize,
                      uint32_t *delayPrescale, uint32_t *tickPrescale,
                      bool *debug, bool *sendto, bool *sendmsg, bool *sendnocp,
                      char* host, char *interface) {

    *mtu = 0;
    int c, i_tmp;
    int64_t tmp;
    bool help = false;
    bool useSendto  = false;
    bool useSendmsg = false;
    bool useSendnocp = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"mtu",   1, NULL, 1},
             {"host",  1, NULL, 2},
             {"ver",   1, NULL, 3},
             {"id",    1, NULL, 4},
             {"pro",   1, NULL, 5},
             {"sendto",   0, NULL, 6},
             {"sendmsg",  0, NULL, 7},
             {"sendnocp",  0, NULL, 8},
             {"dpre",  1, NULL, 9},
             {"tpre",  1, NULL, 10},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:i:t:d:b:s:e:", long_options, 0)) != EOF) {

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
                if (i_tmp >= 500) {
                    *bufsize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -b, buf size >= 500\n");
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
                *entropy = i_tmp;
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

            case 'i':
                // OUTGOING INTERFACE NAME / IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "interface address is bad\n");
                    exit(-1);
                }
                strcpy(interface, optarg);
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
                // DESTINATION HOST
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -host, host name is too long\n");
                    exit(-1);
                }
                strcpy(host, optarg);
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
                // DATA_ID
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -id. Id must be >= 0 and < 65536\n");
                    exit(-1);
                }
                *id = i_tmp;
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
                // use "sendto" to send UDP packets
                if (useSendmsg || useSendnocp) {
                    fprintf(stderr, "Can only have EITHER -sendto OR -sendmsg OR -sendnocp\n");
                    exit(-1);
                }
                fprintf(stdout, "Use \"sendto\"\n");
                useSendto = true;
                *sendto = true;
                break;

            case 7:
                // use "sendmsg" to send UDP packets
                if (useSendto || useSendnocp) {
                    fprintf(stderr, "Can only have EITHER -sendto OR -sendmsg OR -sendnocp\n");
                    exit(-1);
                }
                fprintf(stdout, "Use \"sendmsg\"\n");
                useSendmsg = true;
                *sendmsg = true;
                break;

            case 8:
                // use "send" to send UDP packets and copy data as little as possible
                if (useSendto || useSendmsg) {
                    fprintf(stderr, "Can only have EITHER -sendto OR -sendmsg OR -sendnocp\n");
                    exit(-1);
                }
                fprintf(stdout, "Use \"send\" with minimal copying data\n");
                useSendnocp = true;
                *sendnocp = true;
                break;

            case 9:
                // Delay prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *delayPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -dpre, dpre >= 1\n");
                    exit(-1);
                }
                break;

            case 10:
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
        printf(" Packets:  %3.4g Hz,    %3.4g Avg, time = %lld microsec\n", rate, avgRate, time);

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

    uint32_t tickPrescale = 1;
    uint32_t delayPrescale = 1, delayCounter = 0;
    uint32_t offset = 0, delay = 0, bufsize = 0, sendBufSize = 0;
    uint16_t port = 0x4c42; // FPGA port is default
    uint64_t tick = 0;
    int mtu, version = 2, protocol = 1, entropy = 0;
    uint16_t dataId = 1;
    bool debug = false, sendto = false, sendmsg = false, sendnocp = false;

    char host[INPUT_LENGTH_MAX], interface[16];
    memset(host, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    strcpy(host, "127.0.0.1");
    strcpy(interface, "lo0");

    parseArgs(argc, argv, &mtu, &protocol, &entropy, &version, &dataId, &port, &tick,
              &delay, &bufsize, &sendBufSize, &delayPrescale, &tickPrescale, &debug, &sendto, &sendmsg, &sendnocp,
              host, interface);

    bool send = !(sendto || sendmsg || sendnocp);

    fprintf(stderr, "send = %s, sendto = %s, sendmsg = %s, sendnocp = %s\n",
            btoa(send), btoa(sendto), btoa(sendmsg), btoa(sendnocp));

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
    int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;

    // Create UDP socket
    int clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

#ifndef __APPLE__
    // Try to increase send buf size - by default to 25 MB
    sendBufSize = sendBufSize <= 0 ? 25000000 : sendBufSize;
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufSize, sizeof(sendBufSize));
#endif
    // Read back the UDP send buffer size in bytes
    socklen_t size = sizeof(int);
    uint32_t sendBufBytes = 0;
    getsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, &size);
    fprintf(stderr, "UDP socket send buffer = %d bytes\n", sendBufBytes);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr(host);
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);


#ifdef __linux__
    {
        int val = IP_PMTUDISC_DO;
        setsockopt(clientSocket, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
    }
#endif

    if (send || sendnocp) {
        fprintf(stderr, "Connection socket to host %s, port %hu\n", host, port);
        int err = connect(clientSocket, (const sockaddr *) &serverAddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            if (debug) perror("Error connecting UDP socket:");
            close(clientSocket);
            return err;
        }
    }

    // Start thread to do rate printout
    pthread_t thd;
    int status = pthread_create(&thd, NULL, thread, (void *) nullptr);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    if (debug) fprintf(stderr, "Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

    // To avoid having file reads contaminate our performance measurements,
    // place some data into a buffer and repeatedly read it.
    // For most efficient use of UDP packets, make our buffer a multiple of maxUdpPayload,
    // roughly around 1MB.
    if (bufsize == 0) {
        bufsize = (1000000 / maxUdpPayload + 1) * maxUdpPayload;
    }
    //uint32_t bufsize = (10000 / maxUdpPayload + 1) * maxUdpPayload; // 10 KB buffers
    char *buf = (char *) malloc(bufsize);
    if (buf == NULL) {
        fprintf(stderr, "cannot allocate internal buffer memory of %d bytes\n", bufsize);
        return -1;
    }

    std::srand(1);
    for (int i=0; i < bufsize; i++) {
        buf[i] = std::rand();
    }

    int err;
    bool firstBuffer = true;
    bool lastBuffer  = true;
    delayCounter = delayPrescale;


    fprintf(stdout, "delay prescale = %u\n", delayPrescale);

    // Statistics
    int64_t packetsSent=0;

    while (true) {
        if (sendnocp) {
            err = sendPacketizedBufferFast(buf, bufsize,
                                           maxUdpPayload, clientSocket,
                                           tick, protocol, entropy, version, dataId, &offset,
                                           delay, delayPrescale, &delayCounter,
                                           firstBuffer, lastBuffer, debug, &packetsSent);
        }
        else if (send) {
            err = sendPacketizedBufferSend(buf, bufsize, maxUdpPayload, clientSocket,
                                           tick, protocol, entropy, version, dataId, &offset, delay,
                                           firstBuffer, lastBuffer, debug, &packetsSent);
        }
        else if (sendto) {
            err = sendPacketizedBufferSendto(buf, bufsize, maxUdpPayload, clientSocket, &serverAddr,
                                             tick, protocol, entropy, version, dataId, &offset, delay,
                                             firstBuffer, lastBuffer, debug, &packetsSent);
        }
        else {
            err = sendPacketizedBufferSendmsg(buf, bufsize, maxUdpPayload, clientSocket, &serverAddr,
                                              tick, protocol, entropy, version, dataId, &offset, delay,
                                              firstBuffer, lastBuffer, debug, &packetsSent);
        }

        if (err < 0) {
            // Should be more info in errno
            EDESTADDRREQ;
            fprintf(stderr, "\nsendPacketizedBuffer: errno = %d, %s\n\n", errno, strerror(errno));
            exit(1);
        }

        // spin delay

//        // delay if any
//        if (delay > 0) {
//            if (--delayCounter < 1) {
//                std::this_thread::sleep_for(std::chrono::microseconds(delay));
//                delayCounter = delayPrescale;
//            }
//        }

        totalBytes   += bufsize;
        totalPackets += packetsSent;
        offset = 0;
        tick += tickPrescale;
    }

    return 0;
}
