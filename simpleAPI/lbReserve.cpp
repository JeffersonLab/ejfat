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
 * @file Send a single data buffer (full of random data) repeatedly
 * to an ejfat router (FPGA-based or simulated) which then passes it
 * to a receiving program (e.g. packetBlastee.cc).
 * </p>
 * <p>
 * This program uses the new ejfat API, the libejfat_simple.so library
 * and the EjfatProducer class.
 */


#include <unistd.h>
#include <cstdlib>
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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]\n",

            "        [-host <CP IP address>]",
            "        [-port <CP port)>]\n",

            "        [-name  <name to give LB instance>]",
            "        [-until <end time of LB reservation (e.g. 2024-03-28T23:59:22Z / RFC 3339 / +4 hours EDT, +5 EST)>]\n",

            "        admin_token");

    fprintf(stderr, "        For the specified host/port, reserve an LB assigned the given name until the given time.\n");
    fprintf(stderr, "        A URL, containing LB connection info is printed out and also stored in the EJFAT_URI env var.\n");
    fprintf(stderr, "        This URL can be parsed and used by both sender and receiver, and is in the format:\n");
    fprintf(stderr, "            ejfat://<INSTANCE_token>@<cp_host>:<cp_port>/<data_host>:<data_port>/<sync_host>:<sync_port>/lb/<lb_id>\n");
}



static void parseArgs(int argc, char **argv,
                      uint16_t* port,
                      bool* debug, bool* useIPv6,
                      char* host, char* name,
                      char* until, char* adminToken) {

    int c, i_tmp;
    int64_t tmp;
    float f_tmp;
    bool help = false;

    /* multiple character command-line options */
    static struct option long_options[] =
            {{"port",     1, nullptr, 1},
             {"host",     1, nullptr, 2},
             {"name",     1, nullptr, 3},
             {"until",    1, nullptr, 4},
             {"ipv6",     0, nullptr, 5},
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
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -host, host name is too long\n");
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
                // UNTIL TIME
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -until, time string is too long\n");
                    exit(-1);
                }
                strcpy(until, optarg);
                break;


            case 5:
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
    else {
        fprintf(stderr, "Admin token must be specified\n\n");
        printHelp(argv[0]);
        exit(-1);
    }
}



//#include <iostream>
//#include <sstream>
//#include <iomanip>
//#include <ctime>
//
//std::time_t parseRfc3339(const std::string& dateTimeString) {
//    std::tm tm = {};
//    std::istringstream iss(dateTimeString);
//    std::string timezone;
//
//    // Parse the time string without the timezone first
//    if (!(iss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S"))) {
//        // Parsing failed
//        return -1;
//    }
//
//    // Check if there's a timezone offset
//    if (!(iss >> timezone)) {
//        // No timezone offset, use UTC
//        return std::mktime(&tm);
//    }
//
//    // Parse the timezone offset
//    int hours = 0, minutes = 0;
//    if (std::sscanf(timezone.c_str(), "%d:%d", &hours, &minutes) != 2) {
//        // Invalid timezone format
//        return -1;
//    }
//
//    // Adjust the time by subtracting the timezone offset
//    std::time_t epochTime = std::mktime(&tm);
//    epochTime -= hours * 3600 + minutes * 60;
//    return epochTime;
//}
//
//int main() {
//    std::string dateTimeString = "2024-03-07T12:30:45-05:00";
//    std::time_t epochSeconds = parseRfc3339(dateTimeString);
//    if (epochSeconds != -1) {
//        std::cout << "Parsed RFC 3339 string into epoch seconds: " << epochSeconds << std::endl;
//    } else {
//        std::cout << "Failed to parse RFC 3339 string" << std::endl;
//    }
//    return 0;
//}



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

    char cp_host[INPUT_LENGTH_MAX];
    memset(cp_host, 0, INPUT_LENGTH_MAX);

    char name[INPUT_LENGTH_MAX];
    memset(name, 0, INPUT_LENGTH_MAX);

    char until[INPUT_LENGTH_MAX];
    memset(until, 0, INPUT_LENGTH_MAX);

    char adminToken[INPUT_LENGTH_MAX];
    memset(adminToken, 0, INPUT_LENGTH_MAX);


    parseArgs(argc, argv, &cp_port, &debug, &useIPv6,
              cp_host, name, until, adminToken);

    if (strlen(name) == 0) {
        strcpy(name, "Default_LB");
    }


    int64_t untilSeconds, nowSeconds;

    // How many seconds past epoch are we right now?
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    nowSeconds = now.tv_sec;


    if (strlen(until) == 0) {
        // If ending time not set, set it to 2 hours from now
        untilSeconds = nowSeconds + 2*60*60;
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
                badTime = false;
            }
        }

        if (badTime) {
            // If until is bad, set for 2 hours from now
            untilSeconds = nowSeconds + 2*60*60;
            fprintf(stdout, "until is a bad time, set to = %d\n", (int)(untilSeconds));
        }
    }


    LbReservation reservation(cp_host, cp_port, name, adminToken, untilSeconds);
    int err = reservation.ReserveLoadBalancer();

    fprintf(stdout, "err = %d\n", err);



    return 0;
}
