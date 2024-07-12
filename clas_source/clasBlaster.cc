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
#include <vector>
#include <sstream>
#include <fstream>
#include <regex>


#include "ejfat.hpp"
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



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        -f <filename>",
            "        [-h] [-v]",
            "        [-r <# repeat read-file cycles>]",
            "        [-nc (no connect on socket)]\n",

            "        [-uri  <URI containing info for sending to LB/CP (default "")>]",
            "        [-file <file with URI (default /tmp/ejfat_uri)>]\n",

            "        [-direct <ip_addr:port>]\n",

            "        [-sock <# of UDP sockets, 16 max>]",
            "        [-mtu <desired MTU size, 9000 default/max, 0 system default, else 1200 minimum>]",
            "        [-t <tick, default 0>]",
            "        [-ver <version, default 2>]",
            "        [-id <data id, default 0>]",
            "        [-pro <protocol, default 1>]",
            "        [-e <entropy, default 0>]\n",

            "        [-bufrate <buffers sent per sec>]",
            "        [-s <UDP send buffer size, default 25MB which gets doubled>]\n",

            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... tick increment each buffer sent)>]",
            "        [-dpre <delay prescale (1,2, ... if -d defined, 1 delay for every prescale pkts/bufs)>]",
            "        [-d <delay in microsec between packets>]");

    fprintf(stderr, "        EJFAT CLAS data UDP packet sender that will packetize and send events repeatedly and get stats\n");
    fprintf(stderr, "        Data is read into a buffer and \"send()\" is used (connect is called).\n");
    fprintf(stderr, "        This program cycles thru the use of up to 16 UDP sockets for better switch performance.\n");
    fprintf(stderr, "        There are 2 ways to know how to send data:\n");
    fprintf(stderr, "           1) specify -uri, or\n");
    fprintf(stderr, "           2) specify -file for file that contains URI.\n");
    fprintf(stderr, "        To bypass the LB and send data direct to consumer:\n");
    fprintf(stderr, "           1) specify -direct (and NOT -uri/-file)\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *entropy, int *version, uint16_t *id,
                      uint64_t* tick, uint32_t* delay,
                      uint64_t *bufRate,
                      uint32_t *sendBufSize,
                      uint32_t *delayPrescale, uint32_t *tickPrescale,
                      uint32_t *repeats,
                      int* socks,
                      bool* debug,
                      bool* noConnect,
                      char *direct, char* uri, char* uriFile,
                      char *filename,
                      std::vector<int>& cores) {

    int c, i_tmp;
    int64_t tmp;
    bool help = false;
    bool gotFile = false;

    static struct option long_options[] =
            {{"mtu",       1, nullptr, 1},

             {"ver",       1, nullptr, 3},
             {"id",        1, nullptr, 4},
             {"pro",       1, nullptr, 5},

             {"sock",      1, nullptr, 7},
             {"dpre",      1, nullptr, 9},
             {"tpre",      1, nullptr, 10},

             {"cores",     1, nullptr, 13},
             {"bufrate",   1, nullptr, 14},
             {"nc",        0, nullptr, 15},

             {"uri",       1, nullptr, 16},
             {"file",      1, nullptr, 17},
             {"direct",    1, nullptr, 18},

             {0, 0, 0, 0}
            };


    while ((c = getopt_long_only(argc, argv, "vht:d:s:e:f:r:", long_options, 0)) != EOF) {

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

            case 13:
                // Cores to run on
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n");
                    exit(-1);
                }

                {
                    // split into ints
                    cores.clear();
                    std::string s = optarg;
                    std::istringstream ss(s);
                    std::string token;

                    while (std::getline(ss, token, ',')) {
                        try {
                            int value = std::stoi(token);
                            cores.push_back(value);
                        }
                        catch (const std::exception& e) {
                            fprintf(stderr, "Invalid argument to -cores, need comma-separated list of ints\n");
                            exit(-1);
                        }
                    }
                }
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
                // do we NOT connect socket?
                *noConnect = true;
                break;

            case 16:
                // URI
                if (strlen(optarg) >= 256) {
                    fprintf(stderr, "Invalid argument to -uri, uri name is too long\n");
                    exit(-1);
                }
                strcpy(uri, optarg);
                break;

            case 17:
                // FILE NAME
                if (strlen(optarg) >= 256) {
                    fprintf(stderr, "Invalid argument to -file, file name is too long\n");
                    exit(-1);
                }
                strcpy(uriFile, optarg);
                break;

            case 18:
                // do we send direct to backend? arg is addr:port
                if (strlen(optarg) >= 256) {
                    fprintf(stderr, "Invalid argument to -direct, too long\n");
                    exit(-1);
                }
                strcpy(direct, optarg);
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
        *delayPrescale = 1;
        *delay = 0;
    }

    if (strlen(direct) > 0 && (strlen(uri) > 0 || strlen(uriFile) > 0)) {
        fprintf(stderr, "Specify either -direct OR (-uri and/or -file), but not both\n");
        exit(-1);
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
    uint32_t delay = 0;
    uint64_t bufRate = 0L;

    int syncSocket;

    uint64_t tick = 0;
    int socks = 1;
    int mtu=9000, version = 2, protocol = 1, entropy = 0;
    int rtPriority = 0;
    uint16_t dataId = 0;

    std::vector<int> cores;
    size_t coreCount = 0;

    bool debug = false;
    bool setBufRate = false;
    bool noConnect = false;

    // Direct connection to backend stuff
    char directArg[256];
    memset(directArg, 0, 256);
    std::string directIP;
    uint16_t directPort = 0;
    bool direct = false;
    bool directIpV6 = false;
    //-----------------------------------

    char syncBuf[28];

    char filename[256];
    memset(filename, 0, 256);

    char uri[256];
    memset(uri, 0, 256);

    char uriFile[256];
    memset(uriFile, 0, 256);

    parseArgs(argc, argv, &mtu, &protocol, &entropy, &version,
              &dataId, &tick,
              &delay, &bufRate, &sendBufSize,
              &delayPrescale, &tickPrescale,
              &repeats, &socks, &debug, &noConnect,
              directArg, uri, uriFile, filename, cores);


#ifdef __linux__

    if (cores.size() > 0) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        if (debug) {
            for (int i=0; i < cores.size(); i++) {
                     std::cerr << "core[" << i << "] = " << cores[i] << "\n";
            }
        }

        for (int i=0; i < cores.size(); i++) {
            std::cerr << "Run sending thread on core " << cores[i] << "\n";
            CPU_SET(cores[i], &cpuset);
        }
        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
        }
    }

    std::cerr << "Initially running on cpu " << sched_getcpu() << "\n";

#endif


    //----------------------------------------------
    // Parse the URI (directly given or in file().
    // This gives CP connection info.
    //----------------------------------------------

    // Set default file name
    if (strlen(uriFile) < 1) {
        strcpy(uriFile, "/tmp/ejfat_uri");
    }

    ejfatURI uriInfo;
    bool haveEverything = false;

    // First see if the uri arg is defined, if so, parse it
    if (strlen(uri) > 0) {
        bool parsed = parseURI(uri, uriInfo);
        if (parsed) {
            // URI is in correct format
            if (!(uriInfo.haveData && uriInfo.haveSync)) {
                std::cerr << "no LB/CP info in URI" << std::endl;
            }
            else {
                haveEverything = true;
            }
        }
    }

    // If no luck with URI, look into file
    if (!haveEverything && strlen(uriFile) > 0) {

        std::ifstream file(uriFile);
        if (file.is_open()) {
            std::string uriLine;
            if (std::getline(file, uriLine)) {
                bool parsed = parseURI(uriLine, uriInfo);
                if (parsed) {
                    if (!(uriInfo.haveData && uriInfo.haveSync)) {
                        std::cerr << "no LB/CP info in file" << std::endl;
                        file.close();
                        return 1;
                    }
                    else {
                        haveEverything = true;
                    }
                }
            }

            file.close();
        }
    }

    //printUri(std::cerr, uriInfo);

    // Perhaps -direct was specified. parseArgs ensures this is not defined
    // if either -uri or -file is defined.
    if (!haveEverything && strlen(directArg) > 0) {
        direct = true;

        // Let's parse the arg with regex (arg = ipaddr:port where ipaddr can be ipv4 or ipv6)
        // Note: the pattern (\[?[a-fA-F\d:.]+\]?) matches either IPv6 or IPv4 addresses
        // in which the addr may be surrounded by [] and thus is stripped off.
        std::regex pattern(R"regex((\[?[a-fA-F\d:.]+\]?):(\d+))regex");

        std::smatch match;
        // change char* to string
        std::string dArg = directArg;

        if (std::regex_match(dArg, match, pattern)) {
            // We're here if directArg is in the proper format ...

            // Remove square brackets from address if present
            directIP = match[1];
            if (!directIP.empty() && directIP.front() == '[' && directIP.back() == ']') {
                directIP = directIP.substr(1, directIP.size() - 2);
            }

            directPort = std::stoi(match[2]);

            if (isIPv6(directIP)) {
                directIpV6 = true;
            }
            haveEverything = true;
        }
    }


    if (!haveEverything) {
        std::cerr << "no LB/CP info in uri or file" << std::endl;
        return 1;
    }

    std::string dataAddr;
    std::string syncAddr;

    bool useIPv6Data = false;
    bool useIPv6Sync = false;

    uint16_t dataPort = 0;
    uint16_t syncPort = 0;

    if (direct) {
        dataAddr    = directIP;
        dataPort    = directPort;
        useIPv6Data = directIpV6;
        std::cerr << "Send directly to ipaddr = " << dataAddr << ", port = " << dataPort << std::endl;
    }
    else {
        // data address and port
        if (uriInfo.useIPv6Data) {
            dataAddr = uriInfo.dataAddrV6;
            useIPv6Data = true;
        }
        else {
            dataAddr = uriInfo.dataAddrV4;
        }

        // sync address and port
        if (uriInfo.useIPv6Sync) {
            syncAddr = uriInfo.syncAddrV6;
            useIPv6Sync = true;
        }
        else {
            syncAddr = uriInfo.syncAddrV4;
        }

        dataPort = uriInfo.dataPort;
        syncPort = uriInfo.syncPort;
    }


    fprintf(stderr, "Delay = %u microsec\n", delay);
    fprintf(stderr, "Using MTU = %d\n", mtu);
    fprintf(stderr, "Sending data to = %s, port %hu\n", dataAddr.c_str(), dataPort);
    if (!direct) fprintf(stderr, "Sending sync to = %s, port %hu\n", syncAddr.c_str(), syncPort);

    if (bufRate > 0) {
        setBufRate = true;
        fprintf(stderr, "Try to regulate buffer output rate\n");
    }


    // 20 bytes = normal IPv4 packet header (60 is max), 8 bytes = max UDP packet header
    // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
    int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;


    // This host for IPv4 and 6
    struct sockaddr_in  serverAddr_sync;
    struct sockaddr_in6 serverAddr6_sync;


    // Socket for sending sync message to CP
    if (!direct) {
        if (useIPv6Sync) {

            if ((syncSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 sync socket");
                return -1;
            }

            memset(&serverAddr6_sync, 0, sizeof(serverAddr6_sync));
            serverAddr6_sync.sin6_family = AF_INET6;
            serverAddr6_sync.sin6_port = htons(syncPort);
            inet_pton(AF_INET6, syncAddr.c_str(), &serverAddr6_sync.sin6_addr);

            if (!noConnect) {
                int err = connect(syncSocket, (const sockaddr *) &serverAddr6_sync, sizeof(struct sockaddr_in6));
                if (err < 0) {
                    perror("Error connecting UDP sync socket:");
                    close(syncSocket);
                    exit(1);
                }
            }
        }
        else {

            if ((syncSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 sync socket");
                return -1;
            }

            memset(&serverAddr_sync, 0, sizeof(serverAddr_sync));
            serverAddr_sync.sin_family = AF_INET;
            serverAddr_sync.sin_port = htons(syncPort);
            serverAddr_sync.sin_addr.s_addr = inet_addr(syncAddr.c_str());
            memset(serverAddr_sync.sin_zero, '\0', sizeof serverAddr_sync.sin_zero);

            if (!noConnect) {
                int err = connect(syncSocket, (const sockaddr *) &serverAddr_sync, sizeof(struct sockaddr_in));
                if (err < 0) {
                    perror("Error connecting UDP sync socket:");
                    close(syncSocket);
                    return err;
                }
            }
        }
    }


    // Create UDP maxSocks sockets for efficient switch operation
    const int maxSocks = 16;
    int portIndex = 0, lastIndex = -1;
    int clientSockets[maxSocks];

    // This host for IPv4 and 6
    struct sockaddr_in  serverAddr;
    struct sockaddr_in6 serverAddr6;

    if (useIPv6Data) {
        // Configure settings in address struct
        // Clear it out
        memset(&serverAddr6, 0, sizeof(serverAddr6));
        // it is an INET address
        serverAddr6.sin6_family = AF_INET6;
        // the port we are going to send to, in network byte order
        serverAddr6.sin6_port = htons(dataPort);
        // the server IP address, in network byte order
        inet_pton(AF_INET6, dataAddr.c_str(), &serverAddr6.sin6_addr);
    }
    else {
        // Configure settings in address struct
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(dataPort);
        serverAddr.sin_addr.s_addr = inet_addr(dataAddr.c_str());
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);
    }


    for (int i = 0; i < socks; i++) {

        if (useIPv6Data) {
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

            if (!noConnect) {
                int err = connect(clientSockets[i], (const sockaddr *) &serverAddr6, sizeof(struct sockaddr_in6));
                if (err < 0) {
                    perror("Error connecting UDP socket:");
                    for (int j = 0; j < lastIndex + 1; j++) {
                        close(clientSockets[j]);
                    }
                    exit(1);
                }
            }
        }
        else {
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

            if (!noConnect) {
                fprintf(stderr, "Connection socket to host %s, port %hu\n", dataAddr.c_str(), dataPort);
                int err = connect(clientSockets[i], (const sockaddr *) &serverAddr, sizeof(struct sockaddr_in));
                if (err < 0) {
                    perror("Error connecting UDP socket:");
                    for (int j = 0; j < lastIndex + 1; j++) {
                        close(clientSockets[j]);
                    }
                    return err;
                }
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

        if (noConnect) {
            if (useIPv6Data) {
                err = sendPacketizedBufferSendNew(buf, byteSize, maxUdpPayload,
                                                  clientSockets[portIndex],
                                                  tick, protocol, entropy, version, dataId,
                                                  (uint32_t) byteSize, &offset,
                                                  0, 1, nullptr,
                                                  firstBuffer, lastBuffer, debug,
                                                  direct, noConnect,
                                                  &packetsSent, 0,
                                                  (sockaddr * ) & serverAddr6, sizeof(struct sockaddr_in6));
            }
            else {
                err = sendPacketizedBufferSendNew(buf, byteSize, maxUdpPayload,
                                                  clientSockets[portIndex],
                                                  tick, protocol, entropy, version, dataId,
                                                  (uint32_t) byteSize, &offset,
                                                  0, 1, nullptr,
                                                  firstBuffer, lastBuffer, debug,
                                                  direct, noConnect,
                                                  &packetsSent, 0,
                                                  (sockaddr * ) & serverAddr, sizeof(struct sockaddr_in));
            }
        }
        else {
            err = sendPacketizedBufferSendNew(buf, byteSize, maxUdpPayload,
                                              clientSockets[portIndex],
                                              tick, protocol, entropy, version, dataId,
                                              (uint32_t) byteSize, &offset,
                                              0, 1, nullptr,
                                              firstBuffer, lastBuffer, debug, direct, &packetsSent);
        }


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
        tick += tickPrescale;

        //---------------------------------------
        // send the sync
        //---------------------------------------
        if (!direct) {
            clock_gettime(CLOCK_MONOTONIC, &tEnd);
            syncTime = 1000000000UL * (tEnd.tv_sec - tStart.tv_sec) + (tEnd.tv_nsec - tStart.tv_nsec);

            // if >= 1 sec ...
            if (syncTime >= 1000000000UL) {
                // Calculate buf or event rate in Hz
                evtRate = bufsSent / (syncTime / 1000000000);

                // Send sync message to same destination
                if (debug) fprintf(stderr, "send tick %" PRIu64 ", evtRate %u\n\n", tick, evtRate);
                setSyncData(syncBuf, version, dataId, tick, evtRate, syncTime);

                if (!noConnect) {
                    //fprintf(stderr, "send sync, connected\n");
                    err = send(syncSocket, syncBuf, 28, 0);
                }
                else {
                    if (useIPv6Sync) {
                        //fprintf(stderr, "send sync, no connect ipv6\n");
                        err = sendto(syncSocket, syncBuf, 28, 0, (sockaddr * ) & serverAddr6_sync,
                                     sizeof(struct sockaddr_in6));
                    }
                    else {
                        //fprintf(stderr, "send sync, no connect ipv4\n");
                        err = sendto(syncSocket, syncBuf, 28, 0, (sockaddr * ) & serverAddr_sync,
                                     sizeof(struct sockaddr_in));
                    }
                }

                if (err == -1) {
                    fprintf(stderr, "\nclasBlaster: error sending sync, errno = %d, %s\n\n", errno, strerror(errno));
                    return (-1);
                }

                tStart = tEnd;
                bufsSent = 0;
            }
        }
        //---------------------------------------

        portIndex = (portIndex + 1) % socks;

        // delay if any
        if (delay) {
            if (--delayCounter < 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
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
