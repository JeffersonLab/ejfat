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
 * Send simulated requests/data to the control_plane_server program.
 * Behaves like an ERSAP backend.
 */

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
#include <memory>
#include <string>
#include <stdexcept>
#include <random>
#include <getopt.h>


#ifdef __linux__
#ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif

// GRPC stuff
#include "lb_cplane.h"



template<class X>
X pid(          // Proportional, Integrative, Derivative Controller
        const X& setPoint, // Desired Operational Set Point
        const X& prcsVal,  // Measure Process Value
        const X& delta_t,  // Time delta between determination of last control value
        const X& Kp,       // Konstant for Proprtional Control
        const X& Ki,       // Konstant for Integrative Control
        const X& Kd        // Konstant for Derivative Control
)
{
    static X previous_error = 0; // for Derivative
    static X integral_acc = 0;   // For Integral (Accumulated Error)
    X error = setPoint - prcsVal;
    integral_acc += error * delta_t;
    X derivative = (error - previous_error) / delta_t;
    previous_error = error;
    return Kp * error + Ki * integral_acc + Kd * derivative;  // control output
}



//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v]",
            "        [-cp_addr <control plane IP address>]",
            "        [-cp_port <control plane port>]",
            "        [-name <backend name>]",
            "        [-id <backend id#>]",
            "        [-s <PID fifo set point>]");

    fprintf(stderr, "        This is a gRPC program that simulates an ERSAP backend by sending messages to a simulated control plane.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param clientId      filled with id# of this grpc client (backend) to send to control plane.
 * @param setPt         filled with the set point of PID loop used with fifo fill level.
 * @param cpPort        filled with grpc server (control plane) port to info to.
 * @param debug         filled with debug flag.
 * @param cpAddr        filled with grpc server (control plane) IP address to info to.
 * @param clientName    filled with name of this grpc client (backend) to send to control plane.
 */
static void parseArgs(int argc, char **argv,
                      uint32_t *clientId, float *setPt, uint16_t* cpPort,
                      bool *debug, char *cpAddr, char *clientName) {

    int c, i_tmp;
    bool help = false;
    float sp = 0.;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"cp_addr",  1, NULL, 4},
                          {"cp_port",  1, NULL, 5},
                          {"name",  1, NULL, 6},
                          {"id",  1, NULL, 7},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhs:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 5:
                // grpc server/control-plane PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *cpPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -gport, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 's':
                // PID set point for fifo fill
                try {
                    sp = (float) std::stof(optarg, nullptr);
                }
                catch (const std::invalid_argument& ia) {
                    fprintf(stderr, "Invalid argument to -s, 0.0 <= PID set point <= 100.0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }

                if (sp >= 0. && sp <= 100.) {
                    *setPt = sp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -s, 0 <= PID set point <= 100\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 7:
                // grpc client id
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *clientId = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -id, backend id must be >= 0\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 4:
                // GRPC server/control-plane IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "grpc server IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                memset(cpAddr, 0, 16);
                strcpy(cpAddr, optarg);
                break;

            case 6:
                // grpc client name
                if (strlen(optarg) > 30 || strlen(optarg) < 1) {
                    fprintf(stderr, "backend name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(clientName, optarg);
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

    int udpSocket;
    ssize_t nBytes;

    // Set this to max expected data size
    uint32_t clientId = 0;

    float pidError = 0.F;
    float setPoint = 0.5F;   // set fifo to 1/2 full by default

    uint16_t cpPort = 56789;
    bool debug = false;

    char cpAddr[16];
    memset(cpAddr, 0, 16);
    strcpy(cpAddr, "172.19.22.15"); // ejfat-4 by default

    char clientName[31];
    memset(clientName, 0, 31);

    parseArgs(argc, argv, &clientId, &setPoint, &cpPort, &debug, cpAddr,  clientName);

    // give it a default name
    if (strlen(clientName) < 1) {
        std::string name = "backend" + std::to_string(clientId);
        std::strcpy(clientName, name.c_str());
    }

    ////////////////////////////
    /// Control Plane  Stuff ///
    ////////////////////////////
    BackendReportServiceImpl service;
    BackendReportServiceImpl *pGrpcService = &service;

    // random device class instance, source of 'true' randomness for initializing random seed
    std::random_device rd;
    // Mersenne twister PRNG, initialized with seed from previous random device instance
    std::mt19937 gen(rd());

    // Even distribution between 0 & 1
    std::uniform_real_distribution<> d(0.0, 1.0);
    // Gaussian, mean = .5, std dev = .2
    std::normal_distribution<> g(0.5, 0.2);


    // PID loop variables
    const float Kp = 0.5;
    const float Ki = 0.0;
    const float Kd = 0.00;
    const float deltaT = 1.0; // 1 millisec


    // ET system
    float fillPercent;
    uint32_t eventSize = 100000;
    uint32_t numEvents = 1000;

    int loopMax   = 1000;
    int loopCount = loopMax; // 1000 loops of 1 millisec = 1 sec

    LbControlPlaneClient client(cpAddr, cpPort, clientName, clientId, eventSize, numEvents, setPoint);

    // Register this client with the grpc server
    int32_t err = client.Register();
    if (err == -1) {
        printf("GRPC client %s is already registered!\n", clientName);
    }
    else if (err == 1) {
        printf("GRPC client %s communication error with server when registering, exit!\n", clientName);
        exit(1);
    }

    while (true) {

        // Delay 1 milliseconds between data points
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Random # in Gaussian dist, mean .5
        fillPercent = g(gen);

        // PID error
        pidError = pid(setPoint, fillPercent, deltaT, Kp, Ki, Kd);

        // Every "loopMax" loops
        if (--loopCount <= 0) {
            // Update the changing variables
            client.update(fillPercent, pidError);
            // Send to server
            err = client.SendState();
            if (err == -2) {
                printf("GRPC client %s cannot send data since it is not registered with server!\n", clientName);
                break;
            }
            else if (err == 1) {
                printf("GRPC client %s communication error with server during sending of data!\n", clientName);
                break;
            }

            printf("Total cnt %d, %f%% filled, error %f\n", numEvents, fillPercent, pidError);

            loopCount = loopMax;
        }
    }

    // Unregister this client with the grpc server
    err = client.UnRegister();
    if (err == 1) {
        printf("GRPC client %s communication error with server when unregistering, exit!\n", clientName);
    }
    exit(1);


    return 0;
}



