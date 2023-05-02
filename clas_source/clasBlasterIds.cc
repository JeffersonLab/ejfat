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
 * @file Read the given HIPO data file and send each event in it
 * to an ejfat router (FPGA-based or simulated) which then passes it
 * to the receiving program - possibly packetBlastee.cc but more likely
 * packetBlasteeEtFifoClient.cc  or udp_rcv_et.cc  .
 * Try /daqfs/java/clas_005038.1231.hipo on the DAQ group disk.
 *
 * This differs from clasBlaster.cc in that it will send exactly the same
 * CLAS event to the same destination but with different data source ids for each.
 * This simulates multiple sources that are in sync as they all produces the same
 * tick in succession. This allows testing of udp_rcv_et.c for example, which handles
 * multiple sources - but they must be in sync.
 * To use this feature, specify a comma-separated list of ids with the "-ids" cmd line option.
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

// HIPO reading
#include "reader.h"


#ifdef __linux__
#ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif


using namespace ejfat;

// Max # of data oputput sources
#define MAX_SOURCES 16

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        -f <filename>",
            "        [-r <# repeat read-file cycles>]",
            "        [-h] [-v] [-ip6] [-sync]",
            "        [-bufdelay] (delay between each buffer, not packet)",
            "        [-host <destination host (defaults to 127.0.0.1)>]",
            "        [-p <destination UDP port>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
            "        [-mtu <desired MTU size>]",
            "        [-t <tick>]",
            "        [-ver <version>]",
            "        [-ids <comma-separated list of outgoing source ids - 16 max>]",
            "        [-pro <protocol>]",
            "        [-e <entropy>]",
            "        [-bufrate <buffers sent per sec>]",
            "        [-bufsize <if setting bufrate, AVERAGE byte size of a single buffer>]",
            "        [-s <UDP send buffer size>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... tick increment each buffer sent)>]",
            "        [-dpre <delay prescale (1,2, ... if -d defined, 1 delay for every prescale pkts/bufs)>]",
            "        [-d <delay in microsec between packets>]");

    fprintf(stderr, "        EJFAT UDP packet sender that will packetize and send buffer repeatedly and get stats\n");
    fprintf(stderr, "        By default, data is copied into buffer and \"send()\" is used (connect is called).\n");
    fprintf(stderr, "        The -sync option will send a UDP message to LB every second with last tick sent.\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *entropy, int *version, uint16_t* port,
                      uint64_t* tick, uint32_t* delay, int *sourceIds,
                      uint64_t *bufRate,
                      uint64_t *avgBufSize, uint32_t *sendBufSize,
                      uint32_t *delayPrescale, uint32_t *tickPrescale,
                      uint32_t *repeats,
                      int *cores,
                      bool *debug,
                      bool *useIPv6, bool *bufDelay, bool *sendSync,
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
             {"pro",   1, NULL, 5},
             {"sync",   0, NULL, 6},
             {"ids",  1, NULL, 7},
             {"dpre",  1, NULL, 9},
             {"tpre",  1, NULL, 10},
             {"ipv6",  0, NULL, 11},
             {"bufdelay",  0, NULL, 12},
             {"cores",  1, NULL, 13},
             {"bufrate",  1, NULL, 14},
             {"bufsize",  1, NULL, 15},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:i:t:d:s:e:f:r:", long_options, 0)) != EOF) {

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

            case 'f':
                // file to read
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "filename is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(filename, optarg);
                gotFile = true;
                break;

            case 'r':
                // repeat
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *repeats = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -r, repeats >= 0\n\n");
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

            case 'd':
                // DELAY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *delay = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -d, packet delay > 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
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

            case 6:
                // do we send sync messages to LB?
                *sendSync = true;
                break;

            case 7:
                // Outcoming source ids
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

            case 9:
                // Delay prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *delayPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -dpre, dpre >= 1\n\n");
                    printHelp(argv[0]);
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
                    fprintf(stderr, "Invalid argument to -tpre, tpre >= 1\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 11:
                // use IP version 6
                *useIPv6 = true;
                break;

            case 12:
                // delay is between buffers not packets
                *bufDelay = true;
                break;

            case 14:
                // Buffers to be sent per second
                tmp = strtol(optarg, nullptr, 0);
                if (tmp > 0) {
                    *bufRate = tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -bufrate, bufrate > 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 15:
                // Avg # of Bytes in each buf to be sent if fixing buf rate
                tmp = strtol(optarg, nullptr, 0);
                if (tmp < 1) {
                    *avgBufSize = tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -bufSize, must be > 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
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

    // If we specify the byte/buffer send rate, then all delays are removed
    if (*bufRate) {
        fprintf(stderr, "Buf rate set to %" PRIu64 " bytes/sec, all delays removed!\n", *bufRate);
        *bufDelay = false;
        *delayPrescale = 1;
        *delay = 0;
    }

    if (help || !gotFile) {
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

        // Data rates (with RE header info)
        totalRate = ((double) (byteCount + RE_HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + RE_HEADER_BYTES*currTotalPackets)) / totalT;
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

    uint32_t tickPrescale = 1;
    uint32_t delayPrescale = 1, delayCounter = 0;
    uint32_t offset = 0, sendBufSize = 0, repeats = UINT32_MAX;
    uint32_t delay = 0, packetDelay = 0, bufferDelay = 0;
    uint64_t bufRate = 0L, avgBufSize = 0L;
    uint16_t port = 0x4c42; // FPGA port is default
    uint64_t tick = 0;
    int cores[10];
    int mtu, version = 2, protocol = 1, entropy = 0;
    int rtPriority = 0;
    bool debug = false;
    bool useIPv6 = false, bufDelay = false;
    bool setBufRate = false;
    bool sendSync = false;

    int sourceIds[MAX_SOURCES];
    int sourceCount = 0;

    char syncBuf[28];
    char host[INPUT_LENGTH_MAX], interface[16], filename[256];
    memset(host, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    memset(filename, 0, 256);
    strcpy(host, "127.0.0.1");
    strcpy(interface, "lo0");

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    for (int i = 0; i < MAX_SOURCES; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &mtu, &protocol, &entropy, &version, &port, &tick,
              &delay, sourceIds, &bufRate, &avgBufSize, &sendBufSize,
              &delayPrescale, &tickPrescale,  &repeats, cores, &debug,
              &useIPv6, &bufDelay, &sendSync, host, interface, filename);

    for (int i = 0; i < MAX_SOURCES; i++) {
        if (sourceIds[i] > -1) {
            sourceCount++;
            std::cerr << "Sending data source id " << sourceIds[i] << " in position " << i << std::endl;
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

    if (bufDelay) {
        packetDelay = 0;
        bufferDelay = delay;
        fprintf(stderr, "Set buffer delay to %u\n", bufferDelay);
    }
    else {
        packetDelay = delay;
        bufferDelay = 0;
        fprintf(stderr, "Set packet delay to %u\n", packetDelay);
    }

    if (bufRate > 0) {
        setBufRate = true;
        fprintf(stderr, "Try to regulate buffer output rate\n");
    }
    else {
        fprintf(stderr, "Not trying to regulate buffer output rate\n");
    }

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
    int status = pthread_create(&thd, NULL, thread, (void *) nullptr);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    if (debug) fprintf(stderr, "Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

    int err;
    bool firstBuffer = true;
    bool lastBuffer  = true;
    delayCounter = delayPrescale;

    fprintf(stdout, "delay prescale = %u\n", delayPrescale);

    // HIPO READING PART

    hipo::reader  reader;
    hipo::event   event;
    //  char *buf;
    uint64_t totalBytes2 = 0L;
    int avgBufBytes = 115114;

    std::cerr << "Preparing to open file " <<  filename << std::endl;
    reader.open(filename);

    int counter = 0;
    int byteSize = 0;
    int index = 0, evCount = 0;
    uint32_t loops = repeats;

    bool haveNext = reader.next();
    std::cerr << "File haveNext = " <<  haveNext << std::endl;


    while(reader.next()) {
        reader.read(event);

        char *buf = &event.getEventBuffer()[0];
        int bytes = event.getSize();
        totalBytes2 += bytes;

        counter++;
    }
    reader.gotoEvent(0);

    avgBufBytes = totalBytes2 / counter;
    std::cerr << "processed events = " << counter << ", avg buf size = " << avgBufBytes << std::endl;

    // Statistics & rate setting
    int64_t packetsSent=0;
    int64_t elapsed, microSecItShouldTake;
    uint64_t syncTime;
    struct timespec t1, t2, tStart, tEnd;
    int64_t excessTime, lastExcessTime = 0, buffersAtOnce, countDown;
    uint64_t byteRate = 0L;

    if (setBufRate) {
        // Don't send more than about 500k consecutive bytes with no delays to avoid overwhelming UDP bufs
        int64_t bytesToWriteAtOnce = 500000;

        // Fixed the BUFFER rate since the # of buffers sent need to be identical between those sources
        byteRate = bufRate * avgBufBytes;
        buffersAtOnce = bytesToWriteAtOnce / avgBufBytes;

        fprintf(stderr, "packetBlaster: buf rate = %" PRIu64 ", avg buf size = %d, data rate = %" PRId64 "\n",
                bufRate, avgBufBytes, byteRate);

        countDown = buffersAtOnce;

        // musec to write data at desired rate
        microSecItShouldTake = 1000000L * bytesToWriteAtOnce / byteRate;
        fprintf(stderr,
                "packetBlaster: bytesToWriteAtOnce = %" PRId64 ", byteRate = %" PRId64 ", buffersAtOnce = %" PRId64 ", microSecItShouldTake = %" PRId64 "\n",
                bytesToWriteAtOnce, byteRate, buffersAtOnce, microSecItShouldTake);

        // Start the clock
        clock_gettime(CLOCK_MONOTONIC, &t1);
    }


    uint32_t evtRate;
    uint64_t bufsSent = 0UL;

    while (true) {

        // If we're sending buffers at a constant rate AND we've sent the entire bunch
        if (setBufRate && countDown-- <= 0) {
            // Get the current time
            clock_gettime(CLOCK_MONOTONIC, &t2);
            // Time taken to send bunch of buffers
            elapsed = 1000000L * (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec)/1000L;
            // Time yet needed in order for everything we've sent to be at the correct rate
            excessTime = microSecItShouldTake - elapsed + lastExcessTime;

//fprintf(stderr, "packetBlaster: elapsed = %lld, this excessT = %lld, last excessT = %lld, buffers/sec = %llu\n",
//        elapsed, (microSecItShouldTake - elapsed), lastExcessTime, buffersAtOnce*1000000/elapsed);

            // Do we need to wait before sending the next bunch of buffers?
            if (excessTime > 0) {
                // We need to wait, but it's possible that after the following delay,
                // we will have waited too long. We know this since any specified sleep
                // period is always a minimum.
                // If that's the case, in the next cycle, excessTime will be < 0.
                std::this_thread::sleep_for(std::chrono::microseconds(excessTime));
                // After this wait, we'll do another round of buffers to send,
                // but we need to start the clock again.
                clock_gettime(CLOCK_MONOTONIC, &t1);
                // Check to see if we overslept so correction can be done
                elapsed = 1000000L * (t1.tv_sec - t2.tv_sec) + (t1.tv_nsec - t2.tv_nsec)/1000L;
                lastExcessTime = excessTime - elapsed;
            }
            else {
                // If we're here, it took longer to send buffers than required in order to meet the
                // given buffer rate. So, it's likely that the specified rate is too high for this node.
                // Record any excess previous sleep time so it can be compensated for in next go round
                // if that is even possible.
                lastExcessTime = excessTime;
                t1 = t2;
            }
            countDown = buffersAtOnce - 1;
        }

        if (reader.next()) {
            reader.read(event);
//fprintf(stderr, "packetBlaster: read next event\n");
        }
        else {
//            fprintf(stderr, "again\n");
            reader.gotoEvent(0);
            reader.read(event);
        }

        char *buf = &event.getEventBuffer()[0];
        byteSize = event.getSize();

        for (int i=0; i < sourceCount; i++) {

            err = sendPacketizedBufferSendNew(buf, byteSize,
                                              maxUdpPayload, clientSocket,
                                              tick, protocol, entropy, version, sourceIds[i],
                                              (uint32_t) byteSize, &offset,
                                              packetDelay, delayPrescale, &delayCounter,
                                              firstBuffer, lastBuffer, debug, &packetsSent);

            if (err < 0) {
                // Should be more info in errno
                fprintf(stderr, "\nsendPacketizedBuffer: errno = %d, %s\n\n", errno, strerror(errno));
                exit(1);
            }

            bufsSent++;
            totalBytes   += byteSize;
            totalPackets += packetsSent;
            offset = 0;
        }

        tick += tickPrescale;


        if (sendSync) {
            clock_gettime(CLOCK_MONOTONIC, &tEnd);
            syncTime = 1000000000UL * (tEnd.tv_sec - tStart.tv_sec) + (tEnd.tv_nsec - tStart.tv_nsec);

            // if >= 1 sec ...
            if (syncTime >= 1000000000UL) {
                // Calculate buf or event rate in Hz
                evtRate = bufsSent/(syncTime/1000000000);

                // Send sync message to same destination - one for each source
                for (int i=0; i < sourceCount; i++) {
                    if (debug) fprintf(stderr, "send tick %" PRIu64 ", evtRate %u\n\n", tick, evtRate);
                    setSyncData(syncBuf, version, sourceIds[i], tick, evtRate, syncTime);
                    err = send(clientSocket, syncBuf, 28, 0);
                    if (err == -1) {
                        fprintf(stderr, "\npacketBlasterNew: error sending sync, errno = %d, %s\n\n", errno,
                                strerror(errno));
                        return (-1);
                    }
                }

                tStart = tEnd;
                bufsSent = 0;
            }
        }

        // delay if any
        if (bufDelay) {
            if (--delayCounter < 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(bufferDelay));
                delayCounter = delayPrescale;
            }
        }

        if (--loops < 1) {
            fprintf(stderr, "\nclasBlaster: finished %u loops reading & sending buffers from file\n\n", repeats);
            break;
        }
    }

    return 0;
}
