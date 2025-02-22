//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 4/02/24.
//



/**
 * <p>
 * @file Monitor a load balancer and get periodic info about its registered users.
 * This program is part of new ejfat API.
 * </p>
 */


#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <cinttypes>
#include <chrono>
#include <getopt.h>

// GRPC stuff
#include "lb_cplane.h"
#include <google/protobuf/util/time_util.h>


//using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v]",
            "         -host <CP IP address>",
            "        [-port <CP port, 18347 default)>]",
            "        [-sec <seconds between printed updates, 5 default>]",
            "        [-token <admin token, udplbd_default_change_me = default>");

    fprintf(stderr, "        Periodically print status of specified CP.\n");
}



static void parseArgs(int argc, char **argv,
                      uint16_t* port, int *sec, bool* debug,
                      char* host, char* adminToken) {

    int c, i_tmp;
    bool help = false;

    /* multiple character command-line options */
    static struct option long_options[] =
            {{"port",     1, nullptr, 1},
             {"host",     1, nullptr, 2},
             {"sec",      1, nullptr, 4},
             {"token",    1, nullptr, 7},
             {nullptr,    0, 0,       0}
            };


    while ((c = getopt_long_only(argc, argv, "vh", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {


            case 1:
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
                    exit(-1);
                }
                break;


            case 2:
                // DESTINATION HOST
                if (strlen(optarg) >= 60) {
                    fprintf(stderr, "Invalid argument to -host, no more than 60 chars\n");
                    exit(-1);
                }
                strcpy(host, optarg);
                break;


            case 4:
                // PERIOD in seconds
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0 && i_tmp < 61) {
                    *sec = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -sec, 0 < sec < 61\n");
                    exit(-1);
                }

                break;

            case 7:
                // ADMIN TOKEN
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -token, too long\n");
                    exit(-1);
                }
                strcpy(adminToken, optarg);
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

    if (strlen(host) == 0) {
        fprintf(stderr, "Control plane host must be specified\n\n");
        printHelp(argv[0]);
        exit(-1);
    }


}




/**
 * Main.
 * @param argc
 * @param argv
 * @return 0 if OK, 1 if error.
 */
int main(int argc, char **argv) {

    uint16_t cp_port = 18347; // default port for talking to CP for reservations
    int seconds = 5;

    bool debug   = false;
    bool useIPv6 = false;

    char cp_host[61];
    memset(cp_host, 0, 61);


    char adminToken[INPUT_LENGTH_MAX];
    memset(adminToken, 0, INPUT_LENGTH_MAX);


    parseArgs(argc, argv, &cp_port, &seconds, &debug,
              cp_host, adminToken);


    if (strlen(adminToken) == 0) {
        std::strcpy(adminToken, "udplbd_default_change_me");
    }

    std::string indent("  ");

    CpOverview overview(cp_host, cp_port, adminToken);


    while (true) {

        overview.getUpdate();
        overview.printCpStats(std::cout, indent);

        // Delay between printouts
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    }

    return 0;
}
