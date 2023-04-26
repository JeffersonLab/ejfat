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
 * Receive data sent by source ids given on command line.
 * Place those in an ET system acting as a fifo.
 * You'll have to coordinate the number of data sources, with the setting up of the ET system.
 * For example, for 3 sources, run the ET with something like:
 *
 * <code>
 *   et_start_fifo -f /tmp/fifoEt -d -s 150000 -n 3 -e 1000
 * </code>
 *
 * You can then run this program like:
 *
 * <code>
 *   udp_rcv_et -et /tmp/fifoEt -ids 1,3,76 -p 17750
 * </code>
 *
 * This expects data sources 1,3, and 76. There will be room in each ET fifo entry to have
 * 3 buffers (ET events), one for each source. There will be 1000 entries. Each buffer will
 * be 150kB. Max # of sources is 16 (can change that below).
 */

#include <iostream>
#include "ejfat_assemble_ersap_et.hpp"

using namespace ejfat;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------

// Max # of data input sources
#define MAX_SOURCES 16

#define INPUT_LENGTH_MAX 256


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] ",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-ids <comma-separated list of incoming source ids>]",
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
static void parseArgs(int argc, char **argv, uint16_t* port, bool *debug, int *sourceIds,
                      char *etFileName, char *listenAddr) {

    int c, i_tmp;
    bool help=false, err=false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {
                    {"et", 1, NULL, 1},
                    {"ids",  1, NULL, 2},
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

            case 2:
                // Incoming source ids
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ids\n");
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
                        sourceIds[index] = (int) strtol(token.c_str(), &endptr, 0);

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
                        sourceIds[index] = (int) strtol(s.c_str(), nullptr, 0);
                        if (errno == EINVAL || errno == ERANGE) {
                            fprintf(stderr, "Invalid argument to -ids, need comma-separated list of ids\n");
                            exit(-1);
                        }
                        index++;
                        //std::cout << s << std::endl;
                    }

                    if (index > MAX_SOURCES) {
                        fprintf(stderr, "Too many sources specified in -ids, max %d\n", MAX_SOURCES);
                        exit(-1);
                    }
                }

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

    int sourceIds[MAX_SOURCES];
    int sourceCount = 0;

    char etFileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(etFileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    for (int i = 0; i < MAX_SOURCES; i++) {
        sourceIds[i] = -1;
    }

    parseArgs(argc, argv, &port, &debug, sourceIds, etFileName, listeningAddr);

    for (int i = 0; i < MAX_SOURCES; i++) {
        if (sourceIds[i] > -1) {
            sourceCount++;
            std::cerr << "Expecting source " << sourceIds[i] << " in position " << i << std::endl;
        }
        else {
            break;
        }
    }

    if (sourceCount < 1) {
        sourceIds[0] = 0;
        sourceCount = 1;
        std::cerr << "Defaulting to (single) source id = 0" << std::endl;
    }

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

    // We're a producer of FIFO data
    err = et_fifo_openProducer(etid, &fid, sourceIds, sourceCount);
    if (err != ET_OK) {
        et_perror(err);
        exit(1);
    }

    // Call routine that reads packets, puts data into fifo entry, places entry into ET in a loop
    getBuffers(udpSocket, fid, debug);

    return 0;
}

