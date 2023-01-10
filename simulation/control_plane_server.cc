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
#include "examples/protos/lbControlPlane.grpc.pb.h"
#else
#include "lbControlPlane.grpc.pb.h"
#endif

#include "lb_control_plane.h"


using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

using lbControlPlane::BackEnd;
using lbControlPlane::ServerReply;
using lbControlPlane::RegistrationRequest;
using lbControlPlane::UnRegistrationRequest;
using lbControlPlane::CurrentState;


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



// structure for passing args to thread
typedef struct threadStruct_t {
    uint16_t grpcServicePort;
    BackendStateServiceImpl *pGrpcService;
} threadStruct;



///** Class implementing logic and data behind the control plane / server's behavior. */
//class BackendReportServiceImpl final : public BackendReport::Service {
//
//public:
//
//    Status SendState  (ServerContext* context, const CurrentState* state, ServerReply* reply);
//    Status Register   (ServerContext* context, const RegistrationRequest* request, ServerReply* reply);
//    Status UnRegister (ServerContext* context, const UnRegistrationRequest* request, ServerReply* reply);
//
//    std::shared_ptr<std::unordered_map<std::string, BackEnd>> getBackEnds();
//
//    static void runServer(uint16_t port, BackendReportServiceImpl *service);
//
//private:
//
//    // Another instance when java is soooo much easier, C++ has no thread-safe containers :(
//    // Since the control plane will be accessing this map while potentially multiple threads are
//    // writing to it SIMULTANEOUSLY, we'll need to protect its access.
//    // Easiest to protect writing into it (only in the SendState, Register, and UnRegister methods
//    // above) with a mutex. For the control plane reading it, we can return a copy when asked for
//    // it in getBackEnds()
//
//    // Store data reported from backends to this server
//    std::unordered_map<std::string, BackEnd> data;
//    std::mutex map_mutex;
//};




// Thread to monitor all the info coming in from backends and update the control plane
static void *controlThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);
    BackendStateServiceImpl *service = targ->pGrpcService;
    int status, fillPercent;

    int64_t totalT = 0, time;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        // Delay 1 seconds between data points
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // This needs to be called each loop since it gets a COPY of the current data (for thread safety)
        auto pDataMap = service->getData();

        for (const auto& n : pDataMap) {
            n.second.printBackendState();
        }
    }

    return (nullptr);
}



// Thread to listen to requests from the back end and respond
static void *grpcServerThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);
    BackendReportServiceImpl *pGrpcService = targ->pGrpcService;

    std::cout << "About to run GRPC server in its own thread on port " << targ->grpcServicePort << std::endl;
    pGrpcService->runServer(targ->grpcServicePort, pGrpcService);
    std::cout << "Should never print this message!!!" << std::endl;

    return (nullptr);
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

    BackendStateServiceImpl service;
    BackendStateServiceImpl *pGrpcService = &service;

    // Start thread to do run pid loop
    threadStruct *targ = (threadStruct *)calloc(1, sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }

    targ->grpcServicePort = 50051;
    targ->pGrpcService = pGrpcService;

    pthread_t thd1;
    int status = pthread_create(&thd1, NULL, controlThread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating PID thread ********\n\n");
        return -1;
    }

    // Start up grpc server listening thread
    threadStruct *targ2 = (threadStruct *)calloc(1, sizeof(threadStruct));
    if (targ2 == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }

    pthread_t thd2;
    status = pthread_create(&thd2, NULL, grpcServerThread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating GRPC server thread ********\n\n");
        return -1;
    }


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


