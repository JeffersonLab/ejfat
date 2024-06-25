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
 * @file Run as a standalone server. This takes a stream of UDP packets
 * from an event sender, passes them through to an EJFAT load balancer (LB).
 * Every second, for every data source/sender, it generates and sends a sync message
 * to the LB's control plane (CP).
 * For data consumers, it acts as a broker when communicating with the CP.
 * So the consumer will register with this server which will pass that on to the CP.
 * Likewise, updates sent from consumer to this server will be passed on the the CP.
 * Data will do directly from the LB to each consumer.
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
#include <regex>
#include <getopt.h>

#include <sstream>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>

#include "EjfatServer.h"

using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]\n",

            "        [-uri  <URI containing info for sending to LB/CP (default "")>]",
            "        [-file <file with URI (default /tmp/ejfat_uri)>]\n",

            "        [-dport <incoming data port of this server (default 19500)>]",
            "        [-cport <incoming consumer msg port of this server (default 18300)>]\n",

            "        [-ids <comma-separated list of incoming source ids>]",
            "        [-nc  (no connect on socket)]",
            "        [-core    <starting core # for send thd>]",
            "        [-coreCnt <# of cores for send thd>]");

    fprintf(stderr, "        EJFAT simple server that acts as a broker between an LB/CP and data senders\n");
    fprintf(stderr, "        as well as a broker between the LB/CP and data consumers.\n");
    fprintf(stderr, "        Only specially programmed senders and consumers can communicate with this server.\n");
    fprintf(stderr, "        There are 2 ways to specify the LB/CP being used:\n");
    fprintf(stderr, "           1) specify -uri, or\n");
    fprintf(stderr, "           2) specify -file for file that contains URI.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc            arg count from main().
 * @param argv            arg list from main().
 * @param core            starting core id on which to run sending thread.
 * @param coreCnt         number of cores on which to run sending thread.
 * @param ids             vector to be filled with sender IDs.
 * @param dataPort        port to listen on for incoming events.
 * @param consumerPort    port to listen on for msgs from consumers.
 * @param debug           true for debug output.
 * @param useIPv6         use ipV6 for ...
 * @param noConnect       true means connect() NOT called on sending sockets.
 * @param uri             URI containing LB/CP connection info.
 * @param filename        name of file in which to read URI.
 */
static void parseArgs(int argc, char **argv,
                      int* core, int* coreCnt,
                      std::set<int>& ids,
                      uint16_t *dataPort , uint16_t *consumerPort,
                      bool *debug, bool *useIPv6, bool *noConnect,
                      char *uri, char *file) {

    int c, i_tmp;
    int64_t tmp;
    float f_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"core",     1, nullptr, 1},
             {"coreCnt",  1, nullptr, 2},
             {"ids",      1, nullptr, 3},
             {"ipv6",     0, nullptr, 5},
             {"nc",       0, nullptr, 7},
             {"uri",      1, nullptr, 8},
             {"file",     1, nullptr, 9},
             {"dport",    1, nullptr, 10},
             {"cport",    1, nullptr, 11},

             {nullptr,    0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vh", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 1:
                // Starting core # to run on for packet sending thd
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
                // Number of cores to run on for packet sending thd
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
                    fprintf(stderr, "Invalid argument to -ids, need comma-separated set of ids\n");
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
                            ids.insert(value);
                        }
                        catch (const std::exception& e) {
                            fprintf(stderr, "Invalid argument to -ids, need comma-separated set of ints\n");
                            exit(-1);
                        }
                    }
                }

                break;


            case 5:
                // use IP version 6
                *useIPv6 = true;
                break;

            case 7:
                // do we NOT connect socket?
                *noConnect = true;
                break;

            case 8:
                // URI
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -uri, uri name is too long\n");
                    exit(-1);
                }
                strcpy(uri, optarg);
                break;

            case 9:
                // FILE NAME
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -file, file name is too long\n");
                    exit(-1);
                }
                strcpy(file, optarg);
                break;

            case 10:
                // Incoming event/data Port
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 1024 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -dport\n");
                    exit(-1);
                }
                else {
                    *dataPort = i_tmp;
                }
                break;

            case 11:
                // Incoming consumer msg Port
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 1024 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -cport\n");
                    exit(-1);
                }
                else {
                    *consumerPort = i_tmp;
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

    if ((strlen(uri) < 1) && (strlen(file) < 1)) {
        fprintf(stderr, "Specify -uri and/or -file\n");
        exit(-1);
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

    uint16_t dataPort = 19500;
    uint16_t consumerPort = 18300;

    bool debug = false;
    bool useIPv6 = false;
    bool noConnect = false;

    int core = -1;
    int coreCount = 1;

    char uri[INPUT_LENGTH_MAX];
    memset(uri, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    std::set<int> ids;


    parseArgs(argc, argv, &core, &coreCount, ids, &dataPort, &consumerPort,
              &debug, &useIPv6, &noConnect, uri, fileName);


    fprintf(stderr, "No connect = %s\n", btoa(noConnect));


    //--------------------------------------------
    EjfatServer server(uri, fileName, ids, dataPort, consumerPort, useIPv6, !noConnect, debug, core, coreCount);
    fprintf(stderr, "Past server creation\n");


    while (true) {
        // Delay 4 seconds
        std::this_thread::sleep_for(std::chrono::seconds(4));
    }

    return 0;
}
