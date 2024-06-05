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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]\n",

            "        [-uri  <URI containing info for sending to LB/CP (default "")>]",
            "        [-file <file with URI (default /tmp/ejfat_uri)>]\n",

            "        [-nc  (no connect on socket)]",
            "        [-cores <comma-separated list of cores to run on>]");

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
 * @param cores           vector to be filled with core numbers to run on.
 * @param debug           true for debug output.
 * @param useIPv6         use ipV6 for ...
 * @param noConnect       true means connect() NOT called on sending sockets.
 * @param uri             URI containing LB/CP connection info.
 * @param filename        name of file in which to read URI.
 */
static void parseArgs(int argc, char **argv,
                      std::vector<int> &cores,
                      bool *debug, bool *useIPv6, bool *noConnect,
                      char *uri, char *file) {

    int c, i_tmp;
    int64_t tmp;
    float f_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"ipv6",     0, nullptr, 5},
             {"cores",    1, nullptr, 6},
             {"nc",       0, nullptr, 7},
             {"uri",      1, nullptr, 8},
             {"file",     1, nullptr, 9},
             {nullptr,    0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vh", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

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

    if ((strlen(uri) < 1) && (strlen(file) < 1)) {
        fprintf(stderr, "Specify -uri and/or -file\n");
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

    bool debug = false;
    bool useIPv6 = false;
    bool noConnect = false;

    char uri[INPUT_LENGTH_MAX];
    memset(uri, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    std::vector<int> cores;
    size_t coreCount = 0;


    parseArgs(argc, argv, cores, &debug, &useIPv6, &noConnect, uri, fileName);

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


    fprintf(stderr, "No connect = %s\n", btoa(noConnect));


    //--------------------------------------------
    EjfatServer();

    while (true) {
        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));
    }

    return 0;
}
