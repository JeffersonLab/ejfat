//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 3/28/24.
//



/**
 * <p>
 * @file Gets the connection info or URI for a given (already existing) load balancer .
 * This program is part of new ejfat API.
 * </p>
 */


#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]",
            "         -lbid <LB's id>",
            "         -host <CP IP address>",
            "        [-port <CP port, 18347 default)>]",
            "        [-file  <file to store URI, /tmp/ejfat_uri default>]",

            "        [<admin_token> udplbd_default_change_me = default]");

    fprintf(stderr, "        For the specified control plane's host/port, get the URI or connection information\n");
    fprintf(stderr, "        for the specified LB.\n");
    fprintf(stderr, "        A URI, containing LB connection info is printed to stdout and written into a file.\n");
    fprintf(stderr, "        This URI can be parsed and used by both sender and receiver, and is in the format:\n");
    fprintf(stderr, "            ejfat://[<token>@]<cp_host>:<cp_port>/lb/<lb_id>[?data=<data_host>:<data_port>][&sync=<sync_host>:<sync_port>]\n");
}



static void parseArgs(int argc, char **argv,
                      uint16_t *port,
                      bool *debug, bool *useIPv6,
                      char *host, char *lbid, char *file,
                      char *adminToken) {

    int c, i_tmp;
    bool help = false;

    /* multiple character command-line options */
    static struct option long_options[] =
            {{"port",     1, nullptr, 1},
             {"host",     1, nullptr, 2},
             {"lbid",     1, nullptr, 3},
             {"file",     1, nullptr, 4},
             {"ipv6",     0, nullptr, 6},
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


            case 3:
                // LB id
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -lbid, id is too long\n");
                    exit(-1);
                }
                strcpy(lbid, optarg);
                break;


            case 4:
                // FILE NAME
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -file, file name is too long\n");
                    exit(-1);
                }
                strcpy(file, optarg);
                break;


            case 6:
                // use IP version 6
                *useIPv6 = true;
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

    // Process remaining arguments (non-options)
    if (optind < argc) {
        strcpy(adminToken, argv[optind]);
        std::cout << "First non-option argument: " << argv[optind] << std::endl;
    }
//    else {
//        fprintf(stderr, "Admin token must be specified\n\n");
//        printHelp(argv[0]);
//        exit(-1);
//    }
}




/**
 * Main.
 * @param argc
 * @param argv
 * @return 0 if OK, 1 if error.
 */
int main(int argc, char **argv) {

    uint16_t cp_port = 18347; // default port for talking to CP

    bool debug   = false;
    bool useIPv6 = false;

    char cp_host[61];
    memset(cp_host, 0, 61);

    char lbid[INPUT_LENGTH_MAX];
    memset(lbid, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    char adminToken[INPUT_LENGTH_MAX];
    memset(adminToken, 0, INPUT_LENGTH_MAX);



    parseArgs(argc, argv, &cp_port, &debug, &useIPv6,
              cp_host, lbid, fileName, adminToken);


    if (strlen(adminToken) == 0) {
        std::strcpy(adminToken, "udplbd_default_change_me");
    }

    std::string uri = LbAdmin::GetLbUri(cp_host, cp_port, lbid, adminToken, useIPv6);

    // If the returned string starts with "error" ...
    if (uri.compare(0, 5, "error") == 0) {
        std::cout << "reserve err = " << uri << std::endl;
    }
    else {
        // This output can be captured into an environmental variable
        std::cout << uri << std::endl;

        // Write it to a file
        if (std::strlen(fileName) < 1) {
            // Default file name
            std::strcpy(fileName, "/tmp/ejfat_uri");
        }

        std::fstream file;
        file.open(fileName, std::ios::trunc | std::ios::out);
        if (!file.fail()) {
            // Write this uri into a file (without the EJFAT_URI=")
            file.write(uri.c_str(), uri.size());
            file.write("\n", 1);
            file.close();
        }
    }

    return 0;
}
