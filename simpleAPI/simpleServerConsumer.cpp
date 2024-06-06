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
 * @file Receive events from an ejfat load balancer.
 * This program talks to a simpleServer which is based on the EjfatServer class.
 * The server interfaces with the CP and talks gRPC so this consumer doesn't
 * have to. It directs events coming thru the LB associated with the server
 * to go to this consumer.
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
#include <sstream>
#include <vector>
#include <string>


#include "serverConsumer.h"


using namespace ejfat;

#define INPUT_LENGTH_MAX 256

// admin token = udplbd_default_change_me


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-jointstats]\n",

            "         -addr <IP address of simple server>",
            "        [-port <port of simple server (default 19500)>]\n",

            "        [-direct]\n",

            "         -a <data receiving address to register w/ CP>",
            "        [-p <starting UDP port (default 17750)>]\n",

            "        [-ids <comma-separated list of incoming source ids>]",
            "        [-core  <starting core # for read thds>]",
            "        [-coreCnt <# of cores for each read thd>]\n");

    fprintf(stderr, "        This is an EJFAT consumer, using the new simple API,\n");
    fprintf(stderr, "        able to receive from multiple data sources.\n\n");
    fprintf(stderr, "        There are 2 ways to receive data:\n");
    fprintf(stderr, "           1) specify -addr for the server IP address and -port for its UDP port\n");
    fprintf(stderr, "           2) bypass the LB and receive directly from sender by specifying -direct\n");
    fprintf(stderr, "              in addition to -addr and port\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param core          starting core id on which to run pkt reading threads.
 * @param coreCnt       number of cores on which to run each pkt reading thread.
 * @param port          filled with UDP port to listen on to send to CP.
 * @param serverPort    filled with UDP port of simple server.
 * @param direct        filled with direct flag.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param jointStats    display stats of all sources joined together.
 * @param serverAddr    IP addr of simple server.
 * @param file          name of file in which to read URI.
 * @param dataAddr      data destination IP address to send to CP.
 * @param ids           vector to be filled with data source id numbers.
 */
static void parseArgs(int argc, char **argv,
                      int* core, int* coreCnt,
                      uint16_t* port, uint16_t* serverPort,
                      bool* direct, bool* debug, bool* useIPv6, bool* jointStats,
                      char* serverAddr, char* dataAddr,
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
                          {"addr",        1, nullptr, 5},
                          {"port",        1, nullptr, 6},
                          {"direct",      0, nullptr, 7},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:a:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // data receiving PORT (to report to CP)
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65536) {
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
                // simple server IP addr
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -addr, too long\n");
                    exit(-1);
                }
                strcpy(serverAddr, optarg);
                break;

            case 6:
                // simple server PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65536) {
                    *serverPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -port (%d), 1023 < port < 65536\n", i_tmp);
                    exit(-1);
                }
                break;


            case 4:
                // print stats of all sources joined together
                *jointStats = true;
                break;

            case 7:
                // Direct, bypass LB
                *direct = true;
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

    if (strlen(serverAddr) < 0) {
        fprintf(stderr, "Must specify -addr (for server address)\n");
        exit(-1);
    }

    if (strlen(dataAddr) < 0) {
        fprintf(stderr, "Must specify -a (for this consumer's data address)\n");
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


    uint16_t dataPort   = 17750; // UDP port on which to receive data
    uint16_t serverPort = 18300; // UDP port on which consumers talk to simple server

    int core = -1;
    int coreCnt = 1;

    bool debug = false;
    bool direct = false;
    bool useIPv6 = false;
    bool jointStats = false;

    char serverAddr[INPUT_LENGTH_MAX];
    memset(serverAddr, 0, INPUT_LENGTH_MAX);

    char host[INPUT_LENGTH_MAX];
    memset(host, 0, INPUT_LENGTH_MAX);

    char dataAddr[INPUT_LENGTH_MAX];
    memset(dataAddr, 0, INPUT_LENGTH_MAX);

    std::vector<int> ids;


    // Parse command line args
    parseArgs(argc, argv, &core, &coreCnt,
              &dataPort, &serverPort, &direct, &debug, &useIPv6, &jointStats,
              serverAddr, dataAddr, ids);

    // If not sources given on cmd line, assume 1 src, id=0
    if (ids.size() == 0) {
        ids.push_back(0);
    }

    // Create the consumer
    std::shared_ptr<serverConsumer> consumer;

    if (direct) {
        printf("Creating client to receive DIRECT from producer\n");
        consumer = std::make_shared<serverConsumer>(dataPort, ids,
                                                    debug, jointStats,
                                                    core, coreCnt);
    }
    else {
        bool connect = false;
        consumer = std::make_shared<serverConsumer>(std::string(serverAddr), std::string(dataAddr),
                                                    serverPort, dataPort,
                                                    ids, debug, jointStats, connect,
                                                    core, coreCnt);
    }

    char*    event;
    size_t   bytes;
    uint16_t srcId;
    uint64_t eventNum;

    while (true) {
        // Non-blocking call to get a single event
        bool gotEvent = consumer->getEvent(&event, &bytes, &eventNum, &srcId);

        if (gotEvent) {
            if (debug) {
                printf("Got event #%" PRIu64 " with %d bytes from src %hu\n", eventNum, (int)bytes, srcId);
            }
        }
        else {
            // Nothing in queue, sleep?
            std::this_thread::sleep_for(std::chrono::nanoseconds(500));
        }

    }

    return 0;
}
