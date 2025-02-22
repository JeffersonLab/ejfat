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
 * @file Send small packets to a packetBlastee in order to test the LB.
 * Check with tcpdump how many packets are sent to LB and how many to the blastee.
 * Used to find runt TCP frame bug in LB.
 * </p>
 */


#include <unistd.h>
#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <cmath>
#include <thread>
#include <pthread.h>
#include <iostream>
#include <cinttypes>
#include "ejfat_packetize.hpp"


#ifdef __linux__
#ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif


using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-host <destination host (defaults to 127.0.0.1)>]",
            "        [-p <destination UDP port>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
            "        [-mtu <desired MTU size>]",
            "        [-t <tick>]",
            "        [-ver <version>]",
            "        [-id <data id>]",
            "        [-pro <protocol>]",
            "        [-e <entropy>]",
            "        [-s <UDP send buffer size>]",
            "        [-cores <comma-separated list of cores to run on>]");

    fprintf(stderr, "        EJFAT UDP packet sender that will packetize and send a series of small packets once\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *entropy, int *version, uint16_t *id, uint16_t* port,
                      uint64_t* tick, uint32_t *sendBufSize,
                      int *cores, bool *debug, bool *useIPv6,
                      char* host, char *interface, char *filename) {

    *mtu = 0;
    int c, i_tmp;
    int64_t tmp;
    bool help = false;
    bool gotFile = false;

    static struct option long_options[] =
            {{"mtu",   1, NULL, 1},
             {"host",  1, NULL, 2},
             {"ver",   1, NULL, 3},
             {"id",    1, NULL, 4},
             {"pro",   1, NULL, 5},
             {"ipv6",  0, NULL, 11},
             {"cores",  1, NULL, 13},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:i:t:s:e:", long_options, 0)) != EOF) {

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
                    fprintf(stderr, "Invalid argument to -t, tick > 0\n\n");
                    printHelp(argv[0]);
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
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
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
                    fprintf(stderr, "Invalid argument to -s, UDP send buf size >= 100kB\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'e':
                // ENTROPY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0) {
                    fprintf(stderr, "Invalid argument to -e. Entropy must be >= 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *entropy = i_tmp;
                break;

            case 'i':
                // OUTGOING INTERFACE NAME / IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "interface address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(interface, optarg);
                break;

            case 1:
                // MTU
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 100) {
                    fprintf(stderr, "Invalid argument to -mtu. MTU buffer size must be > 100\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *mtu = i_tmp;
                break;

            case 2:
                // DESTINATION HOST
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -host, host name is too long\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(host, optarg);
                break;

            case 3:
                // VERSION
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 31) {
                    fprintf(stderr, "Invalid argument to -ver. Version must be >= 0 and < 32\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *version = i_tmp;
                break;

            case 4:
                // DATA_ID
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -id. Id must be >= 0 and < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *id = i_tmp;
                break;

            case 5:
                // PROTOCOL
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0) {
                    fprintf(stderr, "Invalid argument to -pro. Protocol must be >= 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                *protocol = i_tmp;
                break;

            case 11:
                // use IP version 6
                *useIPv6 = true;
                break;

            case 13:
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


// Thread to send to print out rates
static void *thread(void *arg) {

    uint64_t packetCount, byteCount;
    uint64_t prevTotalPackets, prevTotalBytes;
    uint64_t currTotalPackets, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    double rate, avgRate, totalRate, totalAvgRate;
    int64_t totalT = 0, time;
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

        // Packet rates
        rate = 1000000.0 * ((double) packetCount) / time;
        avgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        printf(" Packets:  %3.4g Hz,    %3.4g Avg, time = %" PRId64 " microsec\n", rate, avgRate, time);

        // Data rates (no header info)
        rate = ((double) byteCount) / time;
        avgRate = ((double) currTotalBytes) / totalT;
        // Must print out t to keep it from being optimized away
        printf(" Data:    %3.4g MB/s,  %3.4g Avg\n", rate, avgRate);

        // Data rates (with header info)
        totalRate = ((double) (byteCount + HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + HEADER_BYTES*currTotalPackets)) / totalT;
        printf(" Total:    %3.4g MB/s,  %3.4g Avg\n\n", totalRate, totalAvgRate);


        t1 = t2;
    }

    return (nullptr);
}






/**
 * Doing things this way is like reading a buffer bit-by-bit
 * and passing it off to the parser bit-by-bit
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {

    uint32_t offset = 0, sendBufSize = 0;
    uint16_t port = 0x4c42; // FPGA port is default
    uint64_t tick = 0;
    int cores[10];
    int mtu, version = 2, protocol = 1, entropy = 0;
    uint16_t dataId = 1;
    bool debug = false;
    bool useIPv6 = false;

    char host[INPUT_LENGTH_MAX], interface[16], filename[256];
    memset(host, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    memset(filename, 0, 256);
    strcpy(host, "127.0.0.1");
    strcpy(interface, "lo0");

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, &mtu, &protocol, &entropy, &version, &dataId, &port, &tick,
              &sendBufSize, cores, &debug, &useIPv6, host, interface, filename);

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
                std::cerr << "Run sending thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
            }
            else {
                break;
            }
        }
        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }

    std::cerr << "Initially running on cpu " << sched_getcpu() << "\n";

#endif



    // Break data into multiple packets of max MTU size.
    // If the mtu was not set on the command line, get it progamatically
    if (mtu == 0) {
        mtu = getMTU(interface, true);
    }

    // Jumbo (> 1500) ethernet frames are 9000 bytes max.
    // Don't exceed this limit.
    if (mtu > 9000) {
        mtu = 9000;
    }

    fprintf(stderr, "Using MTU = %d\n", mtu);

    // 20 bytes = normal IPv4 packet header (60 is max), 8 bytes = max UDP packet header
    // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
    int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;

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
        fprintf(stderr, "UDP socket send buffer = %d bytes\n", sendBufBytes);

        // Configure settings in address struct
        // Clear it out
        memset(&serverAddr6, 0, sizeof(serverAddr6));
        // it is an INET address
        serverAddr6.sin6_family = AF_INET6;
        // the port we are going to send to, in network byte order
        serverAddr6.sin6_port = htons(port);
        // the server IP address, in network byte order
        inet_pton(AF_INET6, host, &serverAddr6.sin6_addr);

        int err = connect(clientSocket, (const sockaddr *) &serverAddr6, sizeof(struct sockaddr_in6));
        if (err < 0) {
            if (debug) perror("Error connecting UDP socket:");
            close(clientSocket);
            exit(1);
        }
    }
    else {
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
        fprintf(stderr, "UDP socket send buffer = %d bytes\n", sendBufBytes);

        // Configure settings in address struct
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        //if (debug) fprintf(stderr, "Sending on UDP port %hu\n", lbPort);
        serverAddr.sin_port = htons(port);
        //if (debug) fprintf(stderr, "Connecting to host %s\n", lbHost);
        serverAddr.sin_addr.s_addr = inet_addr(host);
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

        fprintf(stderr, "Connection socket to host %s, port %hu\n", host, port);
        int err = connect(clientSocket, (const sockaddr *) &serverAddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            if (debug) perror("Error connecting UDP socket:");
            close(clientSocket);
            return err;
        }
    }

    // set the don't fragment bit
#ifdef __linux__
    {
            int val = IP_PMTUDISC_DO;
            setsockopt(clientSocket, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
    }
#endif

    // Start thread to do rate printout
    pthread_t thd;
    int status = pthread_create(&thd, nullptr, thread, (void *) nullptr);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    fprintf(stderr, "Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);



    int err;
    bool firstBuffer = true;
    bool lastBuffer  = true;


    // For testing
    int bufCount = 40;
    char **bufArray = (char **) calloc(bufCount, sizeof(char *));
    if (bufArray == nullptr) {
        fprintf(stderr, "cannot allocate internal array of buffers\n");
        return -1;
    }

    size_t sizes[bufCount];
    for (int i=0; i < bufCount; i++) {
        bufArray[i] = (char *) malloc(i+1 + 16);
        sizes[i] = i+1 + 16;
        if (bufArray[i] == nullptr) {
            fprintf(stderr, "cannot allocate buffer\n");
            return -1;
        }
    }

    // Statistics & rate setting
    int64_t packetsSent=0;
    uint32_t delayCounter = 0;


    for (int i=0; i < bufCount; i++) {

//        err = sendPacketizedBufferSend(bufArray[i], sizes[i],
//                                       maxUdpPayload, clientSocket,
//                                       tick, protocol, entropy, version, dataId, &offset,
//                                       0, 1, &delayCounter,
//                                       firstBuffer, lastBuffer, debug, &packetsSent);

        // Write LB meta data into buffer
        setLbMetadata(bufArray[i], tick, version, protocol, entropy);

        // Send message to receiver
        err = send(clientSocket, bufArray[i], sizes[i], 0);
        if (err == -1) {
            if (errno == EMSGSIZE) {
                // The UDP packet is too big, so we need to reduce it.
                fprintf(stderr, "UDP packet is too big\n");
            }
            else {
                // All other errors are unrecoverable
                fprintf(stderr, "error: errno = %d, %s\n", errno, strerror(errno));
                return (-1);
            }
        }

        if (err != (sizes[i])) {
            fprintf(stderr, "sent partial buffer\n");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));


        totalBytes   += sizes[i];
        totalPackets += packetsSent;
        offset = 0;
        tick += 1;
    }

    return 0;
}
