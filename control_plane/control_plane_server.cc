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
 * Simulate a load balancer's control plane by receiving gRPC messages from an ERSAP (simulated) backend --
 * packetBlasteeEtFifoClient.c and control_plane_tester.c programs.
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
#include <random>
#include <map>
#include <unordered_map>

#ifdef __linux__
#ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif


#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/loadbalancer.grpc.pb.h"
#else
#include "loadbalancer.grpc.pb.h"
#endif

#include "lb_cplane.h"

using namespace std;
using namespace std::chrono;

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
            "\nusage: %s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v]",
            "        [-p <grpc server port>]",
            "        [-cores <comma-separated list of cores to run on>]");

    fprintf(stderr, "        This is a gRPC server getting requests/data from an ERSAP reasembly backend's gRPC client.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param cores         array of core ids on which to run assembly thread.
 * @param port          filled with port of gRPC server ERSAP reassembly backend).
 * @param debug         filled with debug flag.
 */
static void parseArgs(int argc, char **argv,
                      int *cores, uint16_t* port,
                      bool *debug) {

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
    BackendReportServiceImpl *pGrpcService;
} threadStruct;


// Thread to monitor all the info coming in from backends and update the control plane
static void *controlThread(void *arg) {

    threadStruct *targ = static_cast<threadStruct *>(arg);
    BackendReportServiceImpl *service = targ->pGrpcService;
    int status, fillPercent;
    bool debug = true;

    int64_t totalT = 0, time;
    struct timespec t1, t2, firstT;

    // random device class instance, source of 'true' randomness for initializing random seed
    std::random_device rd;
    // Mersenne twister PRNG, initialized with seed from previous random device instance
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> dist(0.0, 1.0);

    // Control (PID error) values from node. Key is session token
    std::map<std::string, float> control;
    // Schedule density for node> Key is session token
    std::map<std::string, float> sched;
    uint64_t epoch = 0; //for now

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;






    while (true) {

        // Delay 2 seconds between data points
        std::this_thread::sleep_for(std::chrono::seconds(2));

        // Record local time so we can see when backend data was last updated in comparison
        int64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

        // This needs to be called each loop since it gets a COPY of the current data (for thread safety).
        // We need to guarantee that the map is not changed while we iterate and between iterations.
        // Again, since we use a copy and since we don't change the map in this routine, we're OK.
        std::shared_ptr<std::unordered_map<std::string, BackEnd>> pDataMap = service->getBackEnds();

        // Number of backends giving feed back this reporting interval (and possibly only previous intervals
        // but have not been declared dead yet).
        size_t num_bes = pDataMap->size();

        // The tricky part is that N may be registered, but < N may have sent their data if at least
        // one registration was recent. So account for this or the weight of that new, unreported,
        // channel will be tiny and it won't be able to crawl out of that hole.

        // Another problem is that the Mth entry may no longer correspond with the proper backend
        // if one of the existing BEs has disappeared.

        // Loop over all backends
        uint16_t n = 0;
        float nrm_sum = 0;

        for (auto &entry: *(pDataMap.get())) {
            BackEnd &backend = entry.second;
            std::string key = backend.getSessionToken();

            // When was the last LOCAL time this was updated? In millisec since epoch.
            int64_t msec = backend.getLocalTime();
            int64_t timeDiff = std::abs(now - msec);

            // If it's been over 2.2 seconds (reporting period = 2 sec) since new data came in or
            // if data has not come in yet, don't include it in calculations
            if (timeDiff > 2200 || msec < 1) {
                // Even tho there is an entry, no current activity
                backend.setIsActive(false);
                break;
            }
            backend.setIsActive(true);

            // read node feedback: an array of health metrics
            control[key] = backend.getPidError();
            // if node not previously active, schedule it's fair share to start with (queues are probably empty)
            float oldSched = sched[key] = sched[key] == 0 ? 1./num_bes : sched[key];
            // update weighting for node from control signal
            sched[key] *= (1.0f + control[key]);
            // sum for normalizing schedule density
            nrm_sum += sched[key];

            if (debug) cout << key << ": piderr " << ", " << control[key] << ", sched den " << oldSched << ", --> " << sched[key] << " ...\n";
            n++;
        }

        // normalize schedule density
        nrm_sum = nrm_sum == 0 ? 1 : nrm_sum;
//        if (debug) { cout << "nrm_sum = " << nrm_sum << '\n'; }

        if (debug) cout << "density: ";
        for (const auto &entry: *(pDataMap.get())) {
            const BackEnd &backend = entry.second;
            if (!backend.getIsActive()) {
                continue;
            }
            std::string key = backend.getSessionToken();
            sched[key] /= nrm_sum;
            if (debug) cout << sched[key] << '\t';
        }
        if (debug) cout << '\n';

//        if (debug) { cout << "write revised tick schedule ...\n"; }
        // write revised tick schedule
        std::map<uint16_t, uint32_t> lb_calendar_table;

        for (uint16_t t = 0; t < 512; t++) {
            // random # between 0 & 1
            float r = dist(gen);
//            if (debug) { cout << "sample = " << r << '\n'; }

            // cumulative distribution from iterating over sched weights
            float cd = 0.f;
            n = 0;

            for (const auto &entry: *(pDataMap.get())) {
                const BackEnd &backend = entry.second;
                if (!backend.getIsActive()) {
                    continue;
                }
                cd += sched[backend.getSessionToken()];
                if (r <= cd) break;
                n++;
            }

//
//            for (size_t ni = 0; ni < num_bes; ni++) {
//                cd += sched[ni];
//                if (r <= cd) break;
//                n++;
//            }

            lb_calendar_table[t] = n;

            if (debug) {
//                cout << "sampled index = " << n << '\n';
//                cout << "table_add load_balance_calendar_table do_assign_member 0x" << std::hex << epoch << " 0x"
//                     << std::hex << t << " => 0x" << std::hex << n << '\n';
            }
        }
    }

    return (nullptr);
}


int main(int argc, char **argv) {

    ssize_t nBytes;
    uint16_t port = 7654;
    int cores[10];
    bool debug = false;

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, cores, &port, &debug);

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

    BackendReportServiceImpl service;
    BackendReportServiceImpl *pGrpcService = &service;

    // Start thread to do run pid loop
    threadStruct *targ = (threadStruct *)calloc(1, sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }

    targ->pGrpcService = pGrpcService;

    pthread_t thd1;
    int status = pthread_create(&thd1, NULL, controlThread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating PID thread ********\n\n");
        return -1;
    }

    while (true) {
        std::cout << "About to run GRPC server on port " << port << std::endl;
        pGrpcService->runServer(port, pGrpcService);
        std::cout << "Should never print this message!!!" << std::endl;
    }

    return 0;
}


