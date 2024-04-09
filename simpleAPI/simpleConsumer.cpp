//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 3/26/24.
//



/**
 * <p>
 * @file Receive events from an ejfat load balancer.
 * This program uses the new ejfat API, the libejfat_simple.so library
 * and the EjfatConsumer class.
 * </p>
 */


#include <unistd.h>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <thread>
#include <pthread.h>
#include <iostream>
#include <cinttypes>
#include <chrono>
#include <sstream>
#include <vector>
#include <string>

#include <arpa/inet.h>
#include <net/if.h>

#include "EjfatConsumer.h"


using namespace ejfat;

#define INPUT_LENGTH_MAX 256



/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-jointstats]\n",

            "        [-uri  <URI containing info for sending to LB/CP (default "")>]",
            "        [-file <file with URI (default /tmp/ejfat_uri)>]\n",

            "        [-a <data receiving address to register w/ CP>]",
            "        [-p <starting UDP port (default 17750)>]",

            "        [-gaddr <CP IP address (no default)>]",
            "        [-gport <CP port (default 18347)>]",

            "        [-ids <comma-separated list of incoming source ids>]\n",

            "        [-core    <starting core # for read thds>]",
            "        [-coreCnt <# of cores for each read thd>]\n");

    fprintf(stderr, "        This is an EJFAT consumer, using the new simple API,\n");
    fprintf(stderr, "        able to receive from multiple data sources.\n\n");
    fprintf(stderr, "        There are 2 ways to know how to receive data: 1) specify -uri, or\n");
    fprintf(stderr, "           2) specify -file for file that contains URI.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param core          starting core id on which to run pkt reading threads.
 * @param coreCnt       number of cores on which to run each pkt reading thread.
 * @param port          filled with UDP port to listen on to send to CP.
 * @param cpPort        filled with main control plane port.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param jointStats    display stats of all sources joined together.
 * @param uri           URI containing LB/CP connection info.
 * @param filename      name of file in which to read URI.
 * @param dataAddr      data destination IP address to send to CP.
 * @param cpAddr        control plane IP address.
 * @param ids           vector to be filled with data source id numbers.
 */
static void parseArgs(int argc, char **argv,
                      int* core, int* coreCnt,
                      uint16_t* port, uint16_t* cpPort,
                      bool* debug, bool* useIPv6, bool* jointStats,
                      char* uri, char* filename,
                      char* dataAddr, char* cpAddr,
                      std::vector<int>& ids) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"core",        1, nullptr, 1},
                          {"coreCnt",     1, nullptr, 2},
                          {"ids",         1, nullptr, 3},
                          {"jointstats",  0, nullptr, 4},
                                // Control Plane
                          {"gaddr",       1, nullptr, 5},
                          {"gport",       1, nullptr, 6},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:a:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // data receiving PORT (to report to CP)
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p (%d), 1023 < port < 65536\n", i_tmp);
                    exit(-1);
                }
                break;

            case 'a':
                // data receiving IP ADDRESS (to report to CP)
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "Invalid argument to -a, data receiving IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(dataAddr, optarg);
                break;

            case 1:
                // Starting core # to run on for packet reading thds
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *core = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -core, need starting core #\n");
                    exit(-1);
                }

                break;


            case 2:
                // Number of cores to run on for packet reading thd
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *coreCnt = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -coreCnt, need # of read cores (min 1)\n");
                    exit(-1);
                }

                break;

            case 3:
                // Incoming source ids
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ids\n");
                    exit(-1);
                }

                {
                    ids.clear();
                    std::string s = optarg;
                    std::stringstream ss(s);
                    std::string token;

                    while (std::getline(ss, token, ',')) {
                        try {
                            int value = std::stoi(token);
                            ids.push_back(value);
                        }
                        catch (const std::exception& e) {
                            fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ints\n");
                            exit(-1);
                        }
                    }
                }

                break;

            case 5:
                // control plane IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7 || !isIPv4(optarg)) {
                    fprintf(stderr, "grpc server IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cpAddr, optarg);
                break;


            case 6:
                // control plane PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *cpPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -gport, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 4:
                // print stats of all sources joined together
                *jointStats = true;
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

    if (*dump && *lump) {
        fprintf(stderr, "Only one of -dump or -lump may be specified\n");
        exit(-1);
    }

    // If wanting to register with a control plane ...
    if (std::strlen(cpAddr) > 0) {
        if (std::strlen(dataAddr) < 1) {
            fprintf(stderr, "If connecting to CP, must also specify -addr\n");
            exit(-1);
        }
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


    uint16_t port   = 17750; // UDP port on which to receive data
    uint16_t cpPort = 18347; // default grpc TCP port to talk to CP

    int core = -1;
    int coreCnt = 1;

    bool debug = false;
    bool useIPv6 = false;
    bool jointStats = false;

    char uri[INPUT_LENGTH_MAX];
    memset(uri, 0, INPUT_LENGTH_MAX);

    char host[INPUT_LENGTH_MAX];
    memset(host, 0, INPUT_LENGTH_MAX);

    char cpAddr[INPUT_LENGTH_MAX];
    memset(cpAddr, 0, INPUT_LENGTH_MAX);

    char dataAddr[INPUT_LENGTH_MAX];
    memset(dataAddr, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    std::vector<int> ids;


    // Parse command line args
    parseArgs(argc, argv, &core, &coreCnt,
              &port, &cpPort, &debug, &useIPv6, &jointStats,
              uri, fileName, dataAddr, cpAddr, ids);

    // Create the consumer
    EjfatConsumer consumer(std::string(dataAddr), std::string(cpAddr),
                           ids, uri, fileName, dataPort, cpPort,
                           core, coreCnt, port, cp_port);

    char*    event;
    size_t   bytes;
    uint16_t srcId;
    uint64_t eventNum;

    while (true) {
        // Non-blocking call to get a single event
        bool gotEvent = consumer.getEvent(&event, &bytes, &eventNum, &srcId);

        if (gotEvent) {
            if (debug) {
                printf("Got event #%" PRIu64 " with %d bytes from src %hu", eventNum, (int)bytes, srcId);

            }
        }
        else {
            // Nothing in queue, sleep?
            std::this_thread::sleep_for(std::chrono::nanoseconds(500));
        }

    }

    return 0;
}
