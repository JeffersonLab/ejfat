//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive locally generated data sent by udp_send.c program.
 * This program handles sequentially numbered packets that may arrive out-of-order.
 * This assumes there is an emulator or FPGA between this and the sending program.
 */

#include <string.h>
#include "ejfat_assemble_ersap_et.hpp"

using namespace ersap::ejfat;


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
            "        [-h] [-v] ",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-et <ET file name>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc        arg count from main().
 * @param argv        arg list from main().
 * @param port        filled with UDP port to listen on.
 * @param debug       filled with debug flag.
 * @param fileName    filled with output file name.
 * @param listenAddr  filled with IP address to listen on.
 */
static void parseArgs(int argc, char **argv, uint16_t* port, bool *debug,
                      char *etFileName, char *listenAddr) {

    int c, i_tmp;
    bool help=false, err=false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {
                    {"et", 1, NULL, 1},
                    {0,    0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:a:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 1:
                if (strlen(optarg) >= INPUT_LENGTH_MAX) {
                    fprintf(stderr, "ET file name is too long\n");
                    exit(-1);
                }
                strcpy(etFileName, optarg);
                break;

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
                    exit(-1);
                }
                break;

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n");
                    exit(-1);
                }
                strcpy(listenAddr, optarg);
                break;

            case 'v':
                // VERBOSE
                *debug = true;
                break;

            case 'h':
                help = true;
                break;

            default:
                fprintf(stderr, "default error, switch = %c\n", c);
                printHelp(argv[0]);
                exit(2);
        }

    }

    if (strlen(etFileName) < 1) {
        err = true;
    }

    if (help || err) {
        printHelp(argv[0]);
        exit(2);
    }
}


int main(int argc, char **argv) {

    int udpSocket;
    // Set this to max expected data size
    uint16_t port = 7777;
    bool debug = true;

    char etFileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(etFileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    parseArgs(argc, argv, &port, &debug, etFileName, listeningAddr);

    // Create UDP socket
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (strlen(listeningAddr) > 0) {
        serverAddr.sin_addr.s_addr = inet_addr(listeningAddr);
    }
    else {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    }
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

    // Bind socket with address struct
    int err = bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
    if (err != 0) {
        // TODO: handle error properly
        if (debug) fprintf(stderr, "bind socket error\n");
    }

    // Connect to ET system, by default connect to local sys

    et_sys_id etid;
    et_fifo_id fid;
    et_openconfig openconfig;

    et_open_config_init(&openconfig);
    if (et_open(&etid, etFileName, openconfig) != ET_OK) {
        printf("et_open problems\n");
        exit(1);
    }

    // Tell ET-based FIFO what source ids are expected
    //    int idCount = 3;
    //    int bufIds[idCount];
    //    bufIds[0]=1;
    //    bufIds[1]=2;
    //    bufIds[2]=77;

        int idCount = 2;
        int bufIds[idCount];
        bufIds[0]=1;
        bufIds[1]=2;

//    int idCount = 1;
//    int bufIds[idCount];
//    bufIds[0]=1;

    // We're a producer of FIFO data
    err = et_fifo_openProducer(etid, &fid, bufIds, idCount);
    if (err != ET_OK) {
        et_perror(err);
        exit(1);
    }

    // Call routine that reads packets, puts data into fifo entry, places entry into ET in a loop
    getBuffers(udpSocket, fid, debug);

    return 0;
}

