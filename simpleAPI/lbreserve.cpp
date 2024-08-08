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
#include <set>
#include <getopt.h>

// GRPC stuff
#include "lb_cplane.h"
#include <google/protobuf/util/time_util.h>


//using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]",
            "         -host <CP IP address>",
            "        [-port <CP port, 18347 default)>]",

            "        [-name  <name to give LB instance, \"Default_LB\" default>]",
            "        [-file  <file to store URI, /tmp/ejfat_uri default>]",
            "        [-until <end time of LB reservation, 10 min from now default>]",
            "                 RFC 3339 format, e.g. 2024-03-28T23:59:22Z (+4 hours EDT, +5 EST)\n",

            "        [-senders <comma-separated list of allowed sender IP addrs>]",
            "        [-token <admin token, udplbd_default_change_me = default>");

    fprintf(stderr, "        For the specified host/port, reserve an LB assigned the given name until the given time.\n");
    fprintf(stderr, "        A URI, containing LB connection info is printed to stdout and written into a file.\n");
    fprintf(stderr, "        This URI can be parsed and used by both sender and receiver, and is in the format:\n");
    fprintf(stderr, "            ejfat://[<token>@]<cp_host>:<cp_port>/lb/<lb_id>[?data=<data_host>:<data_port>][&sync=<sync_host>:<sync_port>]\n");
}



static void parseArgs(int argc, char **argv,
                      uint16_t* port,
                      bool* debug, bool* useIPv6,
                      char* host, char* name, char *file,
                      char* until, char* adminToken,
                      std::set<std::string>& senders) {

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
             {"token",    1, nullptr, 7},
             {"senders",  1, nullptr, 8},
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

            case 7:
                // ADMIN TOKEN
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -token, too long\n");
                    exit(-1);
                }
                strcpy(adminToken, optarg);
                break;


            case 8:
                // allowed senders
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -senders, need comma-separated list of ip addrs\n");
                    exit(-1);
                }

                {
                    senders.clear();
                    std::string s = optarg;
                    std::stringstream ss(s);
                    std::string token;

                    while (std::getline(ss, token, ',')) {
                        try {
                            boost::asio::ip::make_address(token);
                        }
                        catch (const boost::system::system_error& e) {
                            std::cout << "skip bad ip addr, " << token << std::endl;
                            continue;
                        }
                        senders.insert(token);
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

    bool debug   = false;
    bool useIPv6 = false;

    char cp_host[61];
    memset(cp_host, 0, 61);

    char name[INPUT_LENGTH_MAX];
    memset(name, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    char until[INPUT_LENGTH_MAX];
    memset(until, 0, INPUT_LENGTH_MAX);

    char adminToken[INPUT_LENGTH_MAX];
    memset(adminToken, 0, INPUT_LENGTH_MAX);

    std::set<std::string> senders;

    parseArgs(argc, argv, &cp_port, &debug, &useIPv6,
              cp_host, name, fileName, until, adminToken, senders);

    if (strlen(name) == 0) {
        strcpy(name, "127.0.0.1:5000");
    }


    int64_t untilSeconds, nowSeconds;

    // How many seconds past epoch are we right now?
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    nowSeconds = now.tv_sec;


    if (strlen(until) == 0) {
        // If ending time not set, set it to last forever
        //std::strcpy(until, "1970-01-01T00:00:00Z");
        untilSeconds = 0;
        //std::cout << "until time NOT set on command line, set to 0" << std::endl;
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
        }

        //std::cout << "until time in epoch sec = " << untilSeconds << std::endl;

        if (badTime) {
            // If until has elapsed, set for forever
            untilSeconds = 0;
            //std::cout << "Time is already elapsed, so set \"until time\" = 0" << std::endl;
        }
    }


    if (strlen(adminToken) == 0) {
        std::strcpy(adminToken, "udplbd_default_change_me");
    }


    std::string uri = LbAdmin::ReserveLoadBalancer(cp_host, cp_port, name, adminToken, senders, untilSeconds, useIPv6);

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
