//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

/**
 * @file
 * Simulate a load balancer's control plane by sending gRPC messages and
 * receiving responses sent by the packetBlasteeEtFifo.c program.
 */

#include <memory>
#include <string>

#include <cstdlib>
#include <iostream>
#include <time.h>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <errno.h>
#include <cinttypes>
#include <getopt.h>

#ifdef __linux__
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif


#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/loadBalancerControl.grpc.pb.h"
#else
#include "loadBalancerControl.grpc.pb.h"
#endif

#include "lbcontrol.h"


using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using loadBalancerControl::BackendState;
using loadBalancerControl::StateReply;
using loadBalancerControl::StateRequest;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------


#define INPUT_LENGTH_MAX 256


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v]",
            "        [-a <backend IP address>]",
            "        [-p <backend port>]",
            "        [-cores <comma-separated list of cores to run on>]");

    fprintf(stderr, "        This is a gRPC client sending requests for status to a reasembly backend's gRPC server.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param cores         array of core ids on which to run assembly thread.
 * @param port          filled with port of gRPC server ERSAP reassembly backend).
 * @param debug         filled with debug flag.
 * @param ipAddr        filled with IP address of gRPC server.
 */
static void parseArgs(int argc, char **argv,
                      int *cores, uint16_t* port,
                      bool *debug, char *ipAddr) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"cores",  1, NULL, 1},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:f:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(ipAddr, optarg);
                break;

            case 1:
                // Cores to run on
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }


                {
                    // split into ints
                    std::string s = optarg;
                    std::string delimiter = ",";

                    size_t pos = 0;
                    std::string token;
                    char *endptr;
                    int index = 0;
                    bool oneMore = true;

                    while ((pos = s.find(delimiter)) != std::string::npos) {
                        //fprintf(stderr, "pos = %llu\n", pos);
                        token = s.substr(0, pos);
                        errno = 0;
                        cores[index] = (int) strtol(token.c_str(), &endptr, 0);

                        if ((token.c_str() - endptr) == 0) {
                            //fprintf(stderr, "two commas next to eachother\n");
                            oneMore = false;
                            break;
                        }
                        index++;
                        //std::cout << token << std::endl;
                        s.erase(0, pos + delimiter.length());
                        if (s.length() == 0) {
                            //fprintf(stderr, "break on zero len string\n");
                            oneMore = false;
                            break;
                        }
                    }

                    if (oneMore) {
                        errno = 0;
                        cores[index] = (int) strtol(s.c_str(), nullptr, 0);
                        if (errno == EINVAL || errno == ERANGE) {
                            fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n\n");
                            printHelp(argv[0]);
                            exit(-1);
                        }
                        index++;
                        //std::cout << s << std::endl;
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
}


int main(int argc, char **argv) {

    ssize_t nBytes;
    uint16_t port = 50051;
    int cores[10];
    bool debug = false;

    char ipAddr[16];
    memset(ipAddr, 0, 16);

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, cores, &port, &debug, ipAddr);

#ifdef __linux__

    if (cores[0] > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        if (debug) {
            for (int i=0; i < 10; i++) {
                     std::cerr << "core[" << i << "] = " << cores[i] << "\n";
            }
        }

        for (int i=0; i < 10; i++) {
            if (cores[i] >= 0) {
                std::cerr << "Run reassembly thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
            }
            else {
                break;
            }
        }
        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << std::endl;
        }
    }

#endif


    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint16_t dataId;
    bool firstLoop = true;


    LoadBalancerControlClient client(ipAddr, port, "carlsControlPlane");

    while (true) {

        int32_t err = client.GetState();
        if (err != 0) {
            std::cerr << "Error calling GetState()" << std::endl;
        }
        client.printBackendState();

        // Delay 2 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    return 0;
}



// Getters
//std::string LoadBalancerControlClient::getIP()        const   {return ipAddr;}
//uint16_t LoadBalancerControlClient::getPort()         const   {return port;}

//std::string LoadBalancerControlClient::getName()      const   {return backendName;}
//int32_t  LoadBalancerControlClient::getBufferCount()  const   {return bufferCount;}
//int32_t  LoadBalancerControlClient::getBufferSize()   const   {return bufferSize;}
//int32_t  LoadBalancerControlClient::getFillPercent()  const   {return fillPercent;}
//int32_t  LoadBalancerControlClient::getPidError()     const   {return pidError;}


