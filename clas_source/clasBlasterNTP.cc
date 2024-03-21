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
 * to the receiving program - possibly packetBlasteeEtFifoClient.cc .
 * Try /daqfs/java/clas_005038.1231.hipo on the DAQ group disk.
 * </p>
 * <p>
 * This program creates 1 to 16 output UDP sockets and rotates between them when
 * sending each event/buffer. This is to facilitate efficient switch operation.
 * The variation in port numbers gives the switch more "entropy",
 * according to ESNET, since each connection is defined by source & host IP and port #s
 * and the switching algorithm is stateless - always relying on these 4 parameters.
 * This makes 16 possibilities or 4 bits of entropy in which ports must be different
 * but not necessarily sequential.
 * </p>
 *</p>
 * Modified clasBlaster.cc that uses time as a tick. This will allow running
 * parallel streams where ticks will be synchronized by time.
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
#include <string>
#include <cstring>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
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

#define INPUT_LENGTH_MAX 256

struct ntp_packet {
    uint8_t li_vn_mode;
    uint8_t stratum;
    uint8_t poll;
    uint8_t precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t ref_id;
    uint32_t ref_timestamp_secs;
    uint32_t ref_timestamp_frac;
    uint32_t orig_timestamp_secs;
    uint32_t orig_timestamp_frac;
    uint32_t recv_timestamp_secs;
    uint32_t recv_timestamp_frac;
    uint32_t trans_timestamp_secs;
    uint32_t trans_timestamp_frac;
};

time_t synchronizeWithNtpServer() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        std::cerr << "Error creating socket" << std::endl;
        exit(1);
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(123);
    server_addr.sin_addr.s_addr = inet_addr("129.57.90.1"); // JLAB NTP server

    ntp_packet packet = {0};
    memset(&packet, 0, sizeof(ntp_packet));
    packet.li_vn_mode = 0x1b;

    if (sendto(sockfd, (char*)&packet, sizeof(ntp_packet), 0, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Error sending packet" << std::endl;
        exit(1);
    }

    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    if (recvfrom(sockfd, (char*)&packet, sizeof(ntp_packet), 0, (struct sockaddr*)&from_addr, &from_len) < 0) {
        std::cerr << "Error receiving packet" << std::endl;
        exit(1);
    }

    close(sockfd); // Close the socket after use

    return ntohl(packet.trans_timestamp_secs) - 2208988800U;
}

static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        -f <filename>",
            "        [-r <# repeat read-file cycles>]",
            "        [-h] [-v] [-ip6] [-sync] [-direct]",
            "        [-bufdelay] (delay between each buffer, not packet)\n",

            "        [-host <destination host (defaults to 127.0.0.1)>]",
            "        [-p <destination UDP port (default 19522)>]",

            "        [-cp_addr <CP IP address (default = none & no CP comm)>]",
            "        [-cp_port <CP port for sync msgs (default 19523)>]",

            "        [-sock <# of UDP sockets, 16 max>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]\n",

            "        [-mtu <desired MTU size, 9000 default/max, 0 system default, else 1200 minimum>]",
            "        [-t <tick, default 0>]",
            "        [-ver <version, default 2>]",
            "        [-id <data id, default 0>]",
            "        [-pro <protocol, default 1>]",
            "        [-e <entropy, default 0>]\n",

            "        [-bufrate <buffers sent per sec>]",
            "        [-bufsize <if setting bufrate, AVERAGE byte size of a single buffer>]",
            "        [-s <UDP send buffer size, default 25MB which gets doubled>]\n",

            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... tick increment each buffer sent)>]",
            "        [-dpre <delay prescale (1,2, ... if -d defined, 1 delay for every prescale pkts/bufs)>]",
            "        [-d <delay in microsec between packets>]");

    fprintf(stderr, "        EJFAT CLAS data UDP packet sender that will packetize and send buffer repeatedly and get stats\n");
    fprintf(stderr, "        By default, data is copied into buffer and \"send()\" is used (connect is called).\n");
    fprintf(stderr, "        The -sync option will send a UDP message to LB every second with last tick sent.\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *entropy, int *version, uint16_t *id,
                      uint16_t* port, uint16_t* cp_port,
                      uint64_t* tick, uint32_t* delay,
                      uint64_t *bufRate,
                      uint64_t *avgBufSize, uint32_t *sendBufSize,
                      uint32_t *delayPrescale, uint32_t *tickPrescale,
                      uint32_t *repeats,
                      int *socks,
                      int *cores,
                      bool *debug,
                      bool *useIPv6, bool *bufDelay,
                      bool *sendSync, bool *direct,
                      char* host, char *cp_host,
                      char *interface, char *filename) {

    *mtu = 0;
    int c, i_tmp;
    int64_t tmp;
    bool help = false;
    bool gotFile = false;

    static struct option long_options[] =
            {{"mtu",       1, nullptr, 1},
             {"host",      1, nullptr, 2},
             {"ver",       1, nullptr, 3},
             {"id",        1, nullptr, 4},
             {"pro",       1, nullptr, 5},
             {"sync",      0, nullptr, 6},
             {"sock",      1, nullptr, 7},
             {"dpre",      1, nullptr, 9},
             {"tpre",      1, nullptr, 10},
             {"ipv6",      0, nullptr, 11},
             {"bufdelay",  0, nullptr, 12},
             {"cores",     1, nullptr, 13},
             {"bufrate",   1, nullptr, 14},
             {"bufsize",   1, nullptr, 15},
             {"cp_port",   1, nullptr, 16},
             {"cp_addr",   1, nullptr, 17},
             {"direct",    0, nullptr, 18},
             {0, 0, 0, 0}
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
                // LB PORT
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
                if (i_tmp == 0) {
                    // setting this to zero means use system default
                    *mtu = 0;
                }
                else if (i_tmp < 1200 || i_tmp > MAX_EJFAT_MTU) {
                    fprintf(stderr, "Invalid argument to -mtu. MTU buffer size must be >= 1200 and <= %d\n", MAX_EJFAT_MTU);
                    exit(-1);
                }
                else {
                    *mtu = i_tmp;
                }
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

            case 6:
                // do we send sync messages to LB?
                *sendSync = true;
                break;

            case 7:
                // # of UDP sockets used to send data
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0 && i_tmp < 17) {
                    *socks = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -sock, # sockets must be > 0 and < 17\n\n");
                    printHelp(argv[0]);
                    exit(-1);
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

            case 16:
                // CP PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *cp_port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -cp_port, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 17:
                // CP HOST for sync messages
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -cp_host, name is too long\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cp_host, optarg);
                break;

            case 18:
                // do we bypass the LB and go directly to backend?
                *direct = true;
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
static volatile uint64_t totalBytes=0, totalPackets=0, totalEvents=0;


// Thread to send to print out rates
static void *thread(void *arg) {

    uint64_t packetCount, byteCount, eventCount;
    uint64_t prevTotalPackets, prevTotalBytes, prevTotalEvents;
    uint64_t currTotalPackets, currTotalBytes, currTotalEvents;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;

    double rate, avgRate, totalRate, totalAvgRate, evRate, avgEvRate;
    uint64_t absTime;
    int64_t totalT = 0, time;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        prevTotalBytes   = totalBytes;
        prevTotalPackets = totalPackets;
        prevTotalEvents  = totalEvents;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        // Epoch time in milliseconds
        absTime = 1000L*(t2.tv_sec) + (t2.tv_nsec)/1000000L;
        // time diff in microseconds
        time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalT = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        currTotalBytes   = totalBytes;
        currTotalPackets = totalPackets;
        currTotalEvents  = totalEvents;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalPackets > 0) {
                skipFirst = false;
            }
            firstT = t1 = t2;
            totalT = totalBytes = totalPackets = totalEvents = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount   = currTotalBytes   - prevTotalBytes;
        packetCount = currTotalPackets - prevTotalPackets;
        eventCount  = currTotalEvents  - prevTotalEvents;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = totalPackets = totalEvents = 0;
            firstT = t1 = t2;
            continue;
        }

        // Packet rates
        rate = 1000000.0 * ((double) packetCount) / time;
        avgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        printf("Packets:       %3.4g Hz,    %3.4g Avg, time: diff = %" PRId64 " usec, abs = %" PRIu64 " epoch msec\n",
               rate, avgRate, time, absTime);

        // Data rates (with NO header info)
        rate = ((double) byteCount) / time;
        avgRate = ((double) currTotalBytes) / totalT;
        // Data rates (with RE header info)
        totalRate = ((double) (byteCount + RE_HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + RE_HEADER_BYTES*currTotalPackets)) / totalT;
        printf("Data (+hdrs):  %3.4g (%3.4g) MB/s,  %3.4g (%3.4g) Avg\n", rate, totalRate, avgRate, totalAvgRate);

        // Event rates
        evRate = 1000000.0 * ((double) eventCount) / time;
        avgEvRate = 1000000.0 * ((double) currTotalEvents) / totalT;
        printf("Events:        %3.4g Hz,  %3.4g Avg, total %" PRIu64 "\n\n", evRate, avgEvRate, totalEvents);

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

    uint16_t port    = 19522; // FPGA port is default
    uint16_t cp_port = 19523; // default for CP port getting sync messages from sender
    int syncSocket;

    uint64_t tick = 0;
    int cores[10];
    int socks = 1;
    int mtu=9000, version = 2, protocol = 1, entropy = 0;
    int rtPriority = 0;
    uint16_t dataId = 1;
    bool debug = false;
    bool useIPv6 = false, bufDelay = false;
    bool setBufRate = false;
    bool sendSync = true;
    bool direct = false;

    char syncBuf[28];
    char host[INPUT_LENGTH_MAX], cp_host[INPUT_LENGTH_MAX], interface[16], filename[256];
    memset(host, 0, INPUT_LENGTH_MAX);
    memset(cp_host, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    memset(filename, 0, 256);
    strcpy(host, "127.0.0.1");
    strcpy(interface, "lo0");

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, &mtu, &protocol, &entropy, &version,
            &dataId, &port, &cp_port, &tick,
              &delay, &bufRate, &avgBufSize, &sendBufSize,
              &delayPrescale, &tickPrescale,  &repeats, &socks, cores, &debug,
              &useIPv6, &bufDelay, &sendSync, &direct,
              host, cp_host, interface, filename);

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

    if (strlen(cp_host) < 1) {
        sendSync = false;
    }

    // Break data into multiple packets of max MTU size.
    // If the mtu was not set on the command line, get it progamatically
    if (mtu == 0) {
        mtu = getMTU(interface, true);
    }

    fprintf(stderr, "Using MTU = %d\n", mtu);

    // 20 bytes = normal IPv4 packet header (60 is max), 8 bytes = max UDP packet header
    // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
    int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;


    // Socket for sending sync message to CP
    if (sendSync) {
        if (useIPv6) {
            struct sockaddr_in6 serverAddr6;

            if ((syncSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 sync socket");
                return -1;
            }

            memset(&serverAddr6, 0, sizeof(serverAddr6));
            serverAddr6.sin6_family = AF_INET6;
            serverAddr6.sin6_port = htons(cp_port);
            inet_pton(AF_INET6, cp_host, &serverAddr6.sin6_addr);

            int err = connect(syncSocket, (const sockaddr *) &serverAddr6, sizeof(struct sockaddr_in6));
            if (err < 0) {
                if (debug) perror("Error connecting UDP sync socket:");
                close(syncSocket);
                exit(1);
            }
        } else {
            struct sockaddr_in serverAddr;

            if ((syncSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 sync socket");
                return -1;
            }

            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(cp_port);
            serverAddr.sin_addr.s_addr = inet_addr(cp_host);
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            int err = connect(syncSocket, (const sockaddr *) &serverAddr, sizeof(struct sockaddr_in));
            if (err < 0) {
                if (debug) perror("Error connecting UDP sync socket:");
                close(syncSocket);
                return err;
            }
        }
    }


    // Create UDP maxSocks sockets for efficient switch operation
    const int maxSocks = 16;
    int portIndex = 0, lastIndex = -1;
    int clientSockets[maxSocks];

    for (int i = 0; i < socks; i++) {
        if (useIPv6) {
            struct sockaddr_in6 serverAddr6;

            /* create a DGRAM (UDP) socket in the INET/INET6 protocol */
            if ((clientSockets[i] = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                return -1;
            }

            socklen_t size = sizeof(int);
            int sendBufBytes = 0;
#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            sendBufBytes = sendBufSize <= 0 ? 25000000 : sendBufSize;
            setsockopt(clientSockets[i], SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif
            sendBufBytes = 0; // clear it
            getsockopt(clientSockets[i], SOL_SOCKET, SO_SNDBUF, &sendBufBytes, &size);
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

            int err = connect(clientSockets[i], (const sockaddr *) &serverAddr6, sizeof(struct sockaddr_in6));
            if (err < 0) {
                if (debug) perror("Error connecting UDP socket:");
                for (int j = 0; j < lastIndex + 1; j++) {
                    close(clientSockets[j]);
                }
                exit(1);
            }
        }
        else {
            struct sockaddr_in serverAddr;

            // Create UDP socket
            if ((clientSockets[i] = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                return -1;
            }

            // Try to increase send buf size to 25 MB
            socklen_t size = sizeof(int);
            int sendBufBytes = 0;
#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            sendBufBytes = sendBufSize <= 0 ? 25000000 : sendBufSize;
            setsockopt(clientSockets[i], SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif
            sendBufBytes = 0; // clear it
            getsockopt(clientSockets[i], SOL_SOCKET, SO_SNDBUF, &sendBufBytes, &size);
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
            int err = connect(clientSockets[i], (const sockaddr *) &serverAddr, sizeof(struct sockaddr_in));
            if (err < 0) {
                if (debug) perror("Error connecting UDP socket:");
                for (int j = 0; j < lastIndex + 1; j++) {
                    close(clientSockets[j]);
                }
                return err;
            }
        }

        lastIndex = i;
    }

    // set the don't fragment bit
#ifdef __linux__
    {
        int val = IP_PMTUDISC_DO;
        for (int i = 0; i < maxSocks; i++) {
            setsockopt(clientSockets[i], IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
        }
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
    clock_gettime(CLOCK_MONOTONIC, &tStart);

    // Sync time with the NTP server VG 03/20/24
    time_t ntpTime = synchronizeWithNtpServer();
    std::cout << "NTP serber synchronized time: " << ctime(&ntpTime) << std::endl;

    // Get NTP time in seconds. Note that is the pressision
    auto ntpTimeChrono = std::chrono::system_clock::from_time_t(ntpTime);

    // Get the local time in seconds
    auto localTime = std::chrono::high_resolution_clock::now();
    auto localTimeInSec = std::chrono::time_point_cast<std::chrono::seconds>(localTime);
    time_t localTime_t = std::chrono::system_clock::to_time_t(localTimeInSec);

    // Calculate the difference, presented in nanoseconds
    auto timeDifference = std::chrono::duration_cast<std::chrono::nanoseconds>(ntpTimeChrono - localTimeInSec);

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

        err = sendPacketizedBufferSendNew(buf, byteSize,
                                       maxUdpPayload, clientSockets[portIndex],
                                       tick, protocol, entropy, version, dataId,
                                       (uint32_t) byteSize, &offset,
                                       packetDelay, delayPrescale, &delayCounter,
                                       firstBuffer, lastBuffer, debug, direct, &packetsSent);

        if (err < 0) {
            // Should be more info in errno
            fprintf(stderr, "\nclasBlaster: errno = %d, %s\n\n", errno, strerror(errno));
            exit(1);
        }

        bufsSent++;
        totalBytes += byteSize;
        totalPackets += packetsSent;
        totalEvents++;
        offset = 0;
        //tick += tickPrescale; VG 03/20/24

        auto now = std::chrono::high_resolution_clock::now(); // Get the current time point
        auto adjustedLocalTime = now + timeDifference;
        auto adjustedLocalTime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(adjustedLocalTime.time_since_epoch()); // Convert to nanoseconds
        time_t adjustedLocalTime_ns_t = std::chrono::system_clock::to_time_t(adjustedLocalTime_ns);

        // tick as time that is sync with the NTP server
        tick = static_cast<uint64_t>(adjustedLocalTime_t);

        std::cout << "Event-ID: " << tick << std::endl;

        if (sendSync) {
            clock_gettime(CLOCK_MONOTONIC, &tEnd);
            syncTime = 1000000000UL * (tEnd.tv_sec - tStart.tv_sec) + (tEnd.tv_nsec - tStart.tv_nsec);

            // if >= 1 sec ...
            if (syncTime >= 1000000000UL) {
                // Calculate buf or event rate in Hz
                evtRate = bufsSent/(syncTime/1000000000);

                // Send sync message to same destination
if (debug) fprintf(stderr, "send tick %" PRIu64 ", evtRate %u\n\n", tick, evtRate);
                setSyncData(syncBuf, version, dataId, tick, evtRate, syncTime);
                err = send(syncSocket, syncBuf, 28, 0);
                if (err == -1) {
                    fprintf(stderr, "\nclasBlaster: error sending sync, errno = %d, %s\n\n", errno, strerror(errno));
                    return (-1);
                }

                tStart = tEnd;
                bufsSent = 0;
            }
        }

        portIndex = (portIndex + 1) % socks;

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
