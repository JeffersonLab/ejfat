//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 7/15/24.
//



/**
 * <p>
 * @file Remove senders from list of those allowed to send to the specified LB.
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
#include "ejfat.hpp"
#include <google/protobuf/util/time_util.h>


using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v]",
            "         -host <CP IP address>",
            "        [-port <CP port, 18347 default)>]",

            "        [-lbid  <LB instance id, 1 default>]",
            "        [-file  <file that stores URI (instead of using -lbid)>]",

            "        [-senders <comma-separated list of allowed sender IP addrs>]",
            "        [-token <admin token, udplbd_default_change_me = default>");

    fprintf(stderr, "        For the specified CP's LB, remove from list of approved senders\n");
    fprintf(stderr, "        Specify LB with -lbid OR -file (containing uri of LB).\n");
}



static void parseArgs(int argc, char **argv,
                      uint16_t* port,
                      bool* debug,
                      char* host, char* lbid, char *file,
                      char* adminToken,
                      std::set<std::string>& senders) {

    int c, i_tmp;
    bool help = false;
    bool haveFile = false;
    bool haveLBid = false;

    /* multiple character command-line options */
    static struct option long_options[] =
            {{"port",     1, nullptr, 1},
             {"host",     1, nullptr, 2},
             {"lbid",     1, nullptr, 3},
             {"file",     1, nullptr, 4},
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
                // LB ID
                if (haveFile) {
                    fprintf(stderr, "Cannot specify both -lbid and -file\n");
                    exit(-1);
                }

                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -lbid, LB name is too long\n");
                    exit(-1);
                }
                strcpy(lbid, optarg);
                haveLBid = true;
                break;


            case 4:
                // FILE NAME
                if (haveLBid) {
                    fprintf(stderr, "Cannot specify both -lbid and -file\n");
                    exit(-1);
                }

                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -file, file name is too long\n");
                    exit(-1);
                }
                strcpy(file, optarg);
                haveFile = true;
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

    if (!haveFile && !haveLBid) {
        fprintf(stderr, "Need to specify either -lbid or -file\n\n");
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

    bool debug = false;

    char cp_host[61];
    memset(cp_host, 0, 61);

    char lbid[INPUT_LENGTH_MAX];
    memset(lbid, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    char adminToken[INPUT_LENGTH_MAX];
    memset(adminToken, 0, INPUT_LENGTH_MAX);

    std::set<std::string> senders;

    parseArgs(argc, argv, &cp_port, &debug,
              cp_host, lbid, fileName, adminToken, senders);


    if (strlen(lbid) == 0) {
        // In this case. fileName is defined (enforced above).
        // Try to get lbid from uri in file

        ejfatURI uriInfo;
        bool haveLBid = false;

        std::ifstream file(fileName);
        if (file.is_open()) {
            std::string uriLine;
            if (std::getline(file, uriLine)) {
                bool parsed = parseURI(uriLine, uriInfo);
                if (parsed) {
                    haveLBid = true;
                }
            }
            file.close();
        }

        if (!haveLBid) {
            std::cerr << "no LB id in file" << std::endl;
            return 1;
        }

        std::string lbId = uriInfo.lbId;

    }

    if (strlen(adminToken) == 0) {
        std::strcpy(adminToken, "udplbd_default_change_me");
    }


    int err = LbAdmin::RemoveSenders(cp_host, cp_port, lbid, adminToken, senders);



    return 0;
}
