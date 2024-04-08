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
 * @file Send a single data buffer (full of sequential ints) repeatedly
 * to an ejfat router which then passes it to a receiving program.
 * This program uses the new ejfat API, the libejfat_simple.so library
 * and the EjfatProducer class.
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

#include <sstream>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>

#include "ejfat_packetize.hpp"
#include "EjfatException.h"
#include "EjfatProducer.h"


using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ipv6]\n",

            "        [-host <destination host (127.0.0.1 default)>]",
            "        [-p <destination UDP port (19522 default)>]\n",

            "        [-cp_addr <CP IP address (no default)>]",
            "        [-cp_port <CP port for sync msgs (19523 default)>]\n",

            "        [-file <file with URI info for sending syncs to CP>]",
            "        [-nc (no connect on socket)]",
            "        [-mtu <desired MTU size, 9000 default/max, 0 system default, else 1200 minimum>]\n",

            "        [-t <starting event number (also tick), default 0>]",
            "        [-id <data id, default 0>]",
            "        [-e <entropy, default 0>]\n",

            "        [-b <buffer size, ~100kB default>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-dpre <delay prescale (1,2, ... if -d defined, 1 delay for every prescale pkts/bufs)>]",
            "        [-d <delay in microsec between packets or buffers depending on -bufdelay>]");

    fprintf(stderr, "        EJFAT UDP packet sender that will packetize and send buffers repeatedly and keep stats.\n");
    fprintf(stderr, "        There are 3 ways to know to send sync msgs to the CP: 1) specify -cp_addr & -cp_port,\n");
    fprintf(stderr, "           2) read & parse EJFAT_URI env var, 3) read & parse URI info in given file (-file).\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *entropy,
                      uint16_t *id, uint16_t* port, uint16_t* cp_port,
                      uint64_t* tick, int* delay,
                      uint64_t *bufSize,
                      int *delayPrescale, std::vector<int> &cores,
                      bool *debug, bool *useIPv6, bool *noConnect,
                      char* host, char *cp_host, char *file) {

    int c, i_tmp;
    int64_t tmp;
    float f_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"mtu",      1, nullptr, 1},
             {"host",     1, nullptr, 2},
             {"id",       1, nullptr, 3},
             {"dpre",     1, nullptr, 4},
             {"ipv6",     0, nullptr, 5},
             {"cores",    1, nullptr, 6},
             {"nc",       0, nullptr, 7},
             {"cp_port",  1, nullptr, 8},
             {"cp_addr",  1, nullptr, 9},
             {"file"   ,  1, nullptr, 10},
             {nullptr,    0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:t:d:b:e:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 't':
                // TICK
                tmp = strtoll(optarg, nullptr, 0);
                if (tmp > -1) {
                    *tick = tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -t, tick > 0\n");
                    exit(-1);
                }
                break;

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65536) {
                    *port = i_tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
                    exit(-1);
                }
                break;

            case 'b':
                // BUFFER SIZE
                tmp = strtol(optarg, nullptr, 0);
                if (tmp >= 500) {
                    *bufSize = tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -b, buf size >= 500\n");
                    exit(-1);
                }
                break;

            case 'e':
                // ENTROPY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0) {
                    fprintf(stderr, "Invalid argument to -e. Entropy must be >= 0\n");
                    exit(-1);
                }
                *entropy = i_tmp;
                break;

            case 'd':
                // DELAY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *delay = i_tmp;
                } else {
                    fprintf(stderr, "Invalid argument to -d, packet delay > 0\n");
                    exit(-1);
                }
                break;

            case 1:
                // MTU
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp == 0) {
                    // setting this to zero means use system default
                    *mtu = 0;
                }
                else if (i_tmp < 1200 || i_tmp > MAX_EJFAT_MTU) {
                    fprintf(stderr, "Invalid argument to -mtu. MTU buffer size must be >= 1200 and <= %d\n", MAX_EJFAT_MTU);
                    exit(-1);
                }
                else {
                    *mtu = i_tmp;
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
                // DATA_ID
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 65536) {
                    fprintf(stderr, "Invalid argument to -id. Id must be >= 0 and < 65536\n");
                    exit(-1);
                }
                *id = i_tmp;
                break;

            case 4:
                // Delay prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *delayPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -dpre, dpre >= 1\n");
                    exit(-1);
                }
                break;

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
                    std::string s = optarg;
                    std::istringstream ss(s);

                    std::string token;
                    while (std::getline(ss, token, ',')) {
                        cores.push_back(std::stoi(token));
                    }
                }
                break;

            case 7:
                // do we NOT connect socket?
                *noConnect = true;
                break;

            case 8:
                // CP PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65536) {
                    *cp_port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -cp_port, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 9:
                // CP HOST for sync messages
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "Invalid argument to -cp_host, name is too long\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(cp_host, optarg);
                break;

            case 10:
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

    int delayPrescale = 1, delayCounter = 0;
    int delay = 0;
    uint64_t bufSize = 0L;
    uint16_t port    = 19522; // FPGA port is default
    uint16_t cp_port = 19523; // default for CP port getting sync messages from sender
    int syncSocket;

    uint64_t tick = 0;
    int mtu = 9000, version = 2, protocol = 1, entropy = 0;
    uint16_t dataId = 0;

    bool debug = false;
    bool useIPv6 = false;
    bool noConnect = false;

    char host[INPUT_LENGTH_MAX];
    memset(host, 0, INPUT_LENGTH_MAX);
    strcpy(host, "127.0.0.1");

    char cp_host[INPUT_LENGTH_MAX];
    memset(cp_host, 0, INPUT_LENGTH_MAX);

    char fileName[INPUT_LENGTH_MAX];
    memset(fileName, 0, INPUT_LENGTH_MAX);

    std::vector<int> cores;
    size_t coreCount = 0;


    parseArgs(argc, argv, &mtu, &entropy,
              &dataId, &port, &cp_port, &tick,
              &delay, &bufSize,
              &delayPrescale, cores, &debug,
              &useIPv6, &noConnect,
              host, cp_host, fileName);

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

    fprintf(stderr, "Delay      = %u microsec\n", delay);
    fprintf(stderr, "No connect = %s\n", btoa(noConnect));
    fprintf(stderr, "Using MTU  = %d\n", mtu);
    fprintf(stdout, "CP host    = %s\n", cp_host);
    fprintf(stdout, "delay prescale = %u\n", delayPrescale);

    // Create the producer
    EjfatProducer producer(std::string(host), std::string(cp_host),
                           port, cp_port, dataId, entropy,
                           delay, delayPrescale, !noConnect, mtu, cores);

    // Create buffer to send & place some data into it and repeatedly send.
    // For most efficient use of UDP packets, make our buffer a multiple of maxUdpPayload,
    // roughly around 100kB.
    int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;
    if (bufSize == 0) {
        bufSize = (100000 / maxUdpPayload + 1) * maxUdpPayload;
        fprintf(stderr, "internally setting buffer to %" PRIu64 " bytes\n", bufSize);
    }

    fprintf(stderr, "Max packet payload = %d bytes, MTU = %d, packets/buf = %d\n",
            maxUdpPayload, mtu, (int)(bufSize/maxUdpPayload + (bufSize % maxUdpPayload != 0)));

    char *buf = (char *) malloc(bufSize);
    if (buf == nullptr) {
        fprintf(stderr, "cannot allocate internal buffer memory of %" PRIu64 " bytes\n", bufSize);
        return -1;
    }

    // write successive ints so we can check transmission on receiving end
    auto *p = reinterpret_cast<uint32_t *>(buf);
    for (uint32_t i=0; i < bufSize/4; i++) {
        p[i] = htonl(i);
    }

    int spinMax = 30, spinCount = 0;

    while (true) {
        // blocking call which is about .5% faster than the internal queue method below.
        producer.sendEvent(buf, bufSize);

        // We're sending the same buffer over and over.
        // For the non-blocking send,
        // the exact same "buf" can end up on an internal queue, 2047x,
        // but that doesn't matter as data never changes.
//        bool added = producer.addToSendQueue(buf, bufSize);
//        if (!added) {
//            // If not added to queue, because it's full, delay and try again later
//            std::this_thread::sleep_for(std::chrono::nanoseconds(200));
//        }
    }

    return 0;
}
