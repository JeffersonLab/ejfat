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
 * @file Reserve a load balancer for future use.
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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]",
            "         -host <CP IP address>",
            "        [-port <CP port, 18347 default)>]",
            "        [-name  <name to give LB instance, \"Default_LB\" default>]",
            "        [-file  <file to store URI, /tmp/ejfat_uri default>]",
            "        [-until <end time of LB reservation, 20 min from now default>]",
            "                 RFC 3339 format, e.g. 2024-03-28T23:59:22Z (+4 hours EDT, +5 EST)\n",

            "        [<admin_token> udplbd_default_change_me = default]");

    fprintf(stderr, "        For the specified host/port, reserve an LB assigned the given name until the given time.\n");
    fprintf(stderr, "        A URI, containing LB connection info is printed out, stored in the EJFAT_URI env var,\n");
    fprintf(stderr, "        and written into a file.\n");
    fprintf(stderr, "        This URI can be parsed and used by both sender and receiver, and is in the format:\n");
    fprintf(stderr, "            ejfat://[<token>@]<cp_host>:<cp_port>/lb/<lb_id>[?data=<data_host>:<data_port>][&sync=<sync_host>:<sync_port>]\n");
}



static void parseArgs(int argc, char **argv,
                      uint16_t* port,
                      bool* debug, bool* useIPv6,
                      char* host, char* name, char *file,
                      char* until, char* adminToken) {

    int c, i_tmp;
    bool help = false;

    /* multiple character command-line options */
    static struct option long_options[] =
            {{"port",     1, nullptr, 1},
             {"host",     1, nullptr, 2},
             {"name",     1, nullptr, 3},
             {"file",     1, nullptr, 4},
             {"until",    1, nullptr, 5},
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
                if (strlen(optarg) >= 15) {
                    fprintf(stderr, "Invalid argument to -host, host name must be dotted-decimal format\n");
                    exit(-1);
                }
                strcpy(host, optarg);
                break;


            case 3:
                // LB NAME
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -name, LB name is too long\n");
                    exit(-1);
                }
                strcpy(name, optarg);
                break;


            case 4:
                // FILE NAME
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -file, file name is too long\n");
                    exit(-1);
                }
                strcpy(file, optarg);
                break;


            case 5:
                // UNTIL TIME
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -until, time string is too long\n");
                    exit(-1);
                }
                strcpy(until, optarg);
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

    uint16_t cp_port = 18347; // default port for talking to CP for reservations

    bool debug   = false;
    bool useIPv6 = false;

    char cp_host[16];
    memset(cp_host, 0, 16);

    char name[INPUT_LENGTH_MAX];
    memset(name, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    char until[INPUT_LENGTH_MAX];
    memset(until, 0, INPUT_LENGTH_MAX);

    char adminToken[INPUT_LENGTH_MAX];
    memset(adminToken, 0, INPUT_LENGTH_MAX);
//
//    char url[INPUT_LENGTH_MAX];
//    memset(url, 0, INPUT_LENGTH_MAX);



    parseArgs(argc, argv, &cp_port, &debug, &useIPv6,
              cp_host, name, fileName, until, adminToken);

    if (strlen(name) == 0) {
        strcpy(name, "127.0.0.1:5000");
    }


    int64_t untilSeconds, nowSeconds;

    // How many seconds past epoch are we right now?
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    nowSeconds = now.tv_sec;


    if (strlen(until) == 0) {
        // If ending time not set, set it to 10 min from now
        untilSeconds = nowSeconds + 10*60;
    }
    else {
        // Convert date to epoch seconds.
        // Parse the RFC 3339 format date (2024-03-28T16:48:45Z) with TimeUtil
        google::protobuf::Timestamp timestamp;

        bool badTime = true;
        if (google::protobuf::util::TimeUtil::FromString(until, &timestamp)) {
            std::time_t epochSeconds = timestamp.seconds();

            // Check to see if time already elapsed
            if (epochSeconds > nowSeconds) {
                untilSeconds = epochSeconds;
                badTime = false;
            }
            else {
//                fprintf(stdout, "until time lapsed\n");
            }
        }

        if (badTime) {
            // If until is bad, set for 10 min from now
            untilSeconds = nowSeconds + 10*60;
//            fprintf(stdout, "until is a bad time/format, set to = %d\n", (int)(untilSeconds));
        }
    }

    //std::string instanceToken = "e3216cb5c7f927b88d0b8dcfc7377e12905edfa4e1f21ef2d9bf54607d4d0239";
    //std::string lbid = "1";

    if (strlen(adminToken) == 0) {
        std::strcpy(adminToken, "udplbd_default_change_me");
    }

//    fprintf(stdout, "cp_host = %s, port = %hu, name = %s, until = %d\nadmin token = %s\n", cp_host, cp_port, name, (int)(untilSeconds), adminToken);


//    LbReservation res(cp_host, cp_port, name, adminToken, untilSeconds);

//    int err = res.FreeLoadBalancer();
//    fprintf(stdout, "free err = %d\n", err);

    std::string url = LbReservation::ReserveLoadBalancer(cp_host, cp_port, name, adminToken, untilSeconds, useIPv6);

    // If the returned string starts with "error" ...
    if (url.compare(0, 5, "error") == 0) {
        std::cout << "reserve err = " << url << std::endl;
    }
    else {
        // Set environment variable
//        sprintf(url, "EJFAT_URI=ejfat://%s@%s:%hu/lb/%s?data=%s:%hu&sync=%s:%hu",
//                res.getInstanceToken().c_str(),
//                cp_host, cp_port, res.getLbId().c_str(),
//                res.getDataAddrV4().c_str(), res.getDataPort(),
//                res.getSyncAddr().c_str(), res.getSyncPort());

        //    // Set environment variable
        //    sprintf(url, "export EJFAT_URI=ejfat://%s@%s:%hu?data=%s:%hu&sync=%s:%hu",
        //            "instance_token", cp_host, cp_port,
        //            "129.57.155.5", 19522,
        //            "129.57.177.135", 19523);

        std::cout << url << std::endl;

        // Write it to a file

        if (std::strlen(fileName) < 1) {
            // Default file name
            std::strcpy(fileName, "/tmp/ejfat_uri");
        }

        std::fstream file;
        file.open(fileName, std::ios::trunc | std::ios::out);
        if (!file.fail()) {
            // Write this url into a file (without the EJFAT_URI=")
            file.write(url.c_str(), url.size());
            file.write("\n", 1);
            file.close();
        }
    }

    return 0;
}
