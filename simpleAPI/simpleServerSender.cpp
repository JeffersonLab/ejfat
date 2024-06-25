//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 6/05/24.
//



/**
 * <p>
 * @file
 * This program talks to a simpleServer which is based on the EjfatServer class.
 * The server interfaces with the CP and talks gRPC so this sender doesn't
 * have to. All events sent by this program gets sent to the simple server
 * which, in turn, directs them to the LB associated with the server.
 * </p>
 */


#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <thread>
#include <iostream>
#include <cinttypes>
#include <chrono>
#include <regex>
#include <getopt.h>

#include <sstream>
#include <vector>


#include "serverProducer.h"


using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]\n",

            "        -server <IP address of simple server>",
            "        [-port <port of simple server (default 19500)>]\n",

            "        [-direct <ip_addr:port>]\n",

            "        [-nc  (no connect on socket)]",
            "        [-mtu <desired MTU size, 9000 default/max, 0 system default, 1200 min>]",
            "        [-id <data id (default 0)>]\n",

            "        [-b     <buffer size, (default ~100kB)>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-dpre  <delay prescale (1,2, ... N) if -d defined, 1 delay for every Nth event)>]",
            "        [-d     <delay in microsec between events>]");

    fprintf(stderr, "        EJFAT UDP packet sender, using the new simple API, that will\n");
    fprintf(stderr, "        packetize and send buffers repeatedly and keep stats.\n");
    fprintf(stderr, "        There is 1 way to know how to send data:\n");
    fprintf(stderr, "           -- specify -addr for the server IP address and -port for its UDP port\n");
    fprintf(stderr, "        To bypass the LB and send data direct to consumer:\n");
    fprintf(stderr, "           -- specify -direct\n");
}


/**
 * Parse all command line options.
 *
 * @param argc            arg count from main().
 * @param argv            arg list from main().
 * @param mtu             Max Transmission Unit (max bytes per UDP packet).
 * @param entropy         set this = id (# to add to destination port for this source).
 * @param id              id of source (0,...). The experimenter's first source should
 *                        start at 0, then increment by 1 for each successive source.
 * @param delay           delay in microsec after each event sent.
 * @param bufSize         size in bytes of events to send.
 * @param delayPrescale   (1,2, ... N), if -d defined, 1 delay for every Nth event.
 * @param cores           vector to be filled with core numbers to run on.
 * @param debug           true for debug output.
 * @param useIPv6         use ipV6 for ...
 * @param noConnect       true means connect() NOT called on sending sockets.
 * @param direct          consumerIP:consumerPort for sending directly to consumer and bypassing LB.
 * @param uri             URI containing LB/CP connection info.
 * @param filename        name of file in which to read URI.
 */
static void parseArgs(int argc, char **argv, int* mtu, int *entropy,
                      uint16_t *id, uint16_t *port, int* delay,
                      uint64_t *bufSize,
                      int *delayPrescale, std::vector<int> &cores,
                      bool *debug, bool *useIPv6, bool *noConnect,
                      char *direct, char *addr) {

    int c, i_tmp;
    int64_t tmp;
    float f_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"mtu",      1, nullptr, 1},
             {"id",       1, nullptr, 3},
             {"dpre",     1, nullptr, 4},
             {"ipv6",     0, nullptr, 5},
             {"cores",    1, nullptr, 6},
             {"nc",       0, nullptr, 7},
             {"server",   1, nullptr, 8},
             {"port",     1, nullptr, 9},
             {"direct",   1, nullptr, 10},
             {nullptr,    0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhd:b:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'b':
                // BUFFER SIZE
                tmp = strtol(optarg, nullptr, 0);
                if (tmp >= 500) {
                    *bufSize = tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -b, buf size >= 500\n");
                    exit(-1);
                }
                break;

//            case 'e':
//                // ENTROPY
//                i_tmp = (int) strtol(optarg, nullptr, 0);
//                if (i_tmp < 0) {
//                    fprintf(stderr, "Invalid argument to -e. Entropy must be >= 0\n");
//                    exit(-1);
//                }
//                *entropy = i_tmp;
//                break;

            case 'd':
                // DELAY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *delay = i_tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -d, packet delay > 0\n");
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
                // DATA_ID
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 65536) {
                    fprintf(stderr, "Invalid argument to -id. Id must be >= 0 and < 65536\n");
                    exit(-1);
                }
                *id = i_tmp;
                *entropy = i_tmp;
                break;

            case 4:
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

            case 5:
                // use IP version 6
                *useIPv6 = true;
                break;

            case 6:
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

            case 7:
                // do we NOT connect socket?
                *noConnect = true;
                break;

            case 8:
                // Server IP address
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -addr, too long\n");
                    exit(-1);
                }
                strcpy(addr, optarg);
                break;

            case 9:
                // Server Port
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 1024 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -port\n");
                    exit(-1);
                }
                else {
                    *port = i_tmp;
                }
                break;

            case 10:
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

    if (strlen(addr) < 0) {
        fprintf(stderr, "Must specify -addr\n");
        exit(-1);
    }

    if (help) {
        printHelp(argv[0]);
        exit(2);
    }
}



/**
 * Doing things this way is like reading a buffer bit-by-bit
 * and passing it off to the parser bit-by-bit
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {

    int delay = 0, delayPrescale = 1;
    uint64_t tick = 0L;
    uint64_t bufSize = 0L;

    int mtu = 9000, version = 2, protocol = 1, entropy = 0;
    uint16_t dataId = 0;
    uint16_t port = 19500;

    bool debug = false;
    bool useIPv6 = false;
    bool noConnect = false;

    // Direct connection to consumer stuff
    char directArg[256];
    memset(directArg, 0, 256);
    std::string directIP;
    uint16_t directPort = 0;
    bool direct = false;
    //-----------------------------------

    char addr[INPUT_LENGTH_MAX];
    memset(addr, 0, INPUT_LENGTH_MAX);

    std::vector<int> cores;
    size_t coreCount = 0;


    parseArgs(argc, argv, &mtu, &entropy,
              &dataId, &port, &delay, &bufSize,
              &delayPrescale, cores, &debug,
              &useIPv6, &noConnect,
              directArg, addr);

#ifdef __linux__

    coreCount = cores.size();

    if (coreCount > 0) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        for (int i=0; i < coreCount; i++) {
            if (cores[i] >= 0) {
                std::cerr << "Run main thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
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

    // Perhaps -direct was specified. parseArgs ensures this is not defined
    // if either -uri or -file is defined.
    if (strlen(directArg) > 0) {
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
        }
        else {
            fprintf(stdout, "\n-direct option has badly formatted arg, should be ipAddr:port\n");
            exit(-1);
        }
    }


    fprintf(stderr, "Direct         = %s\n", btoa(direct));
    fprintf(stderr, "Delay          = %u microsec\n", delay);
    fprintf(stderr, "No connect     = %s\n", btoa(noConnect));
    fprintf(stderr, "Using MTU      = %d\n", mtu);
    fprintf(stdout, "delay prescale = %u\n", delayPrescale);


    // Create the producer. Used shared_ptr since we need to use different constructors
    std::shared_ptr<serverProducer> producer;

    if (direct) {
        producer = std::make_shared<serverProducer>(std::string(directIP), directPort, direct,
                                                    debug, dataId, entropy, delay, delayPrescale,
                                                    !noConnect, mtu, cores);
    }
    else {
        producer = std::make_shared<serverProducer>(std::string(addr), port, direct,
                                                    debug, dataId, entropy, delay, delayPrescale,
                                                    !noConnect, mtu, cores);
    }


    //--------------------------------------------
    // Create data to send
    //--------------------------------------------

    // For most efficient use of UDP packets, make our buffer size
    // a multiple of maxUdpPayload, roughly around 100kB.
    int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;
    if (bufSize == 0) {
        bufSize = (100000 / maxUdpPayload + 1) * maxUdpPayload;
        fprintf(stderr, "internally setting buffer to %" PRIu64 " bytes\n", bufSize);
    }

    fprintf(stderr, "Max packet payload = %d bytes, MTU = %d, packets/buf = %d\n",
            maxUdpPayload, mtu, (int)(bufSize/maxUdpPayload + (bufSize % maxUdpPayload != 0)));

    // Create buffer
    char *buf = (char *) malloc(bufSize);
    if (buf == nullptr) {
        fprintf(stderr, "cannot allocate internal buffer memory of %" PRIu64 " bytes\n", bufSize);
        return -1;
    }

    // Write successive ints so we can check transmission on receiving end
    auto *p = reinterpret_cast<uint32_t *>(buf);
    for (uint32_t i=0; i < bufSize/4; i++) {
        p[i] = htonl(i);
    }

    //--------------------------------------------

    bool blocking = true;

    while (true) {
        if (blocking) {
            // Blocking send (slightly faster than the internal queue method below).
            //producer->sendEvent(buf, bufSize, tick++);
            producer->sendEvent(buf, bufSize);
        }
        else {
            // Non-blocking placement of event on queue
            // which an internal thread dequeues and sends.
            bool added = producer->addToSendQueue(buf, bufSize);
            if (!added) {
                // If not added to queue, because it's full, delay and try again later
                std::this_thread::sleep_for(std::chrono::nanoseconds(200));
            }
        }
    }

    return 0;
}
