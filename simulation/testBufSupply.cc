//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Test the BufferSupply and BufferSupplyItem classes.
 */

#include <pthread.h>
#include <time.h>
#include <iostream>
#include <chrono>
#include <thread>
#include "BufferSupply.h"
#include "BufferSupplyItem.h"

#include "ejfat_assemble_ersap.hpp"

using namespace ejfat;


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
            "\nusage: %s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-d (dual)]",
            "        [-b <internal buffer byte size>]");

    fprintf(stderr, "        Program to test a C++ BufferSupply based on the disruptor\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param bufSize       filled with buffer size.
 * @param recvBufSize   filled with UDP receive buffer size.
 * @param startingPort  filled with the first UDP port to listen on.
 * @param idCount       filled with number of data sources.
 * @param debug         filled with debug flag.
 * @param repeat        filled with repeat flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param fileName      filled with output file name.
 * @param listenAddr    filled with IP address to listen on.
 */
static void parseArgs(int argc, char **argv, int* bufSize, bool *dual, bool *debug) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhdb:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'b':
                // BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 10000) {
                    *bufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -b, internal buf size >= 10kB\n");
                    exit(-1);
                }
                break;

            case 'v':
                // VERBOSE
                *debug = true;
                break;

            case 'd':
                // Dual users
                *dual = true;
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


// Quantities to be shared by each sending thread
static bool debug = false;



// Arg to pass to threads
struct threadArg {
    std::shared_ptr<ejfat::BufferSupply> supply;
    int bufferSize;
};


// Thread to send to get, fill, and publish a buffer from the supply
static void *thread(void *arg) {

    std::cout << "THREAD STARTED" << std::endl;

    struct threadArg *tArg = (struct threadArg *) arg;
    std::shared_ptr<ejfat::BufferSupply> supply = tArg->supply;
    int bufSize = tArg->bufferSize;
    fprintf(stderr, "1A\n");

    for (int i=0; i < 320; i++) {
        // Get buffer from supply
        auto item = supply->get();

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Fill with data
        auto bb = item->getBuffer();
        bb->putInt(i);

        // Send it to single consumer
        supply->publish(item);
        //std::cout << "  producer published item #" << i << " = " << i << std::endl;
    }


    return (NULL);
}


int main(int argc, char **argv) {

    int ringSize = 16;
    int bufSize = 1000;
    bool dual = false;

    parseArgs(argc, argv, &bufSize, &dual, &debug);

    fprintf(stderr, "Create BB supply, ring size = %d, buf size = %d\n", ringSize, bufSize);

    std::shared_ptr<ejfat::BufferSupply> supply = std::make_shared<ejfat::BufferSupply>(ringSize, bufSize);

    if (dual) {

        fprintf(stderr, "1\n");
        // Thread to send part of buffer to gather application
        struct threadArg *arg = (threadArg *) malloc(sizeof(struct threadArg));
        if (arg == NULL) {
            fprintf(stderr, "\n ******* ran out of memory\n\n");
            exit(1);
        }

        fprintf(stderr, "2\n");
        arg->supply = supply;
        arg->bufferSize = bufSize;

        pthread_t thd;
        int status = pthread_create(&thd, NULL, thread, (void *) arg);
        if (status != 0) {
            fprintf(stderr, "\n ******* error creating thread\n\n");
            return -1;
        }
        fprintf(stderr, "3\n");

        for (int i = 0; i < 320; i++) {
            // Get buffer from supply that producer wrote data into
            auto item = supply->consumerGet();

            // Read data
            auto byteBuf = item->getBuffer();
            int val = byteBuf->getInt(0);
            std::cout << "       client got item " << i << ", data = " << val << std::endl;

            // Send it to back to supply for reuse
            supply->release(item);
        }

        sleep(10);
    }
    else {
        for (int i=0; i < 320; i++) {
            // Get buffer from supply
            auto item = supply->get();

            std::cout << "\nProducer got item #" << i << std::endl;
            sleep(1);

            // Send it to back to supply for reuse
            supply->release(item);
            std::cout << "  producer released item #" << i << std::endl;
        }
    }


    return 0;
}

