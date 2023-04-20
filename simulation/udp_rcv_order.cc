//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive locally generated data sent by udp_send_order.c program.
 * This program handles sequentially numbered packets that may arrive out-of-order.
 * This assumes there is an emulator or FPGA between this and the sending program.
 */

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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] ",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer size in bytes]",
            "        <output file name>");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc        arg count from main().
 * @param argv        arg list from main().
 * @param bufSize     filled with buffer size.
 * @param port        filled with UDP port to listen on.
 * @param debug       filled with debug flag.
 * @param fileName    filled with output file name.
 * @param listenAddr  filled with IP address to listen on.
 */
static void parseArgs(int argc, char **argv, int* bufSize, uint16_t* port, bool *debug,
                      char *fileName, char *listenAddr) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:", long_options, 0)) != EOF) {

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
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
                    exit(-1);
                }
                break;

            case 'b':
                // BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 10000) {
                    *port = 10000;
                    fprintf(stderr, "Set buffer to minimum size of 10000 bytes\n");
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
                printHelp(argv[0]);
                exit(2);
        }

    }

    // Grab any default args not in option list
    if (   !optarg
          && optind < argc // make sure optind is valid
          && nullptr != argv[optind] // make sure it's not a null string
          && '\0'    != argv[optind][0] // ... or an empty string
          && '-'     != argv[optind][0] // ... or another option
            ) {

        strcpy(fileName, argv[optind]);
        fprintf(stderr, "Copy optional arg, file = %s\n", fileName);
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
    int bufSize = 100000;
    uint16_t port = 7777;
    bool debug = false;

    char fileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(fileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    parseArgs(argc, argv, &bufSize, &port, &debug, fileName, listeningAddr);

    // Create UDP socket
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);

    // Try to increase recv buf size to 25 MB
    socklen_t size = sizeof(int);
    int recvBufBytes = 25000000;
    setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
    recvBufBytes = 0; // clear it
    getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
    fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

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

    // use filename provided as 1st argument (stdout by default)
    bool writingToFile = false;

    FILE *fp = nullptr;
    if (strlen(fileName) > 0) {
        fp = fopen (fileName, "w");
        writingToFile = true;
    }
    else {
        fp = stdout;
    }

    // validate file open for writing
    if (!fp) {
        fprintf(stderr, "file open failed: %s\n", strerror(errno));
        return 1;
    }

    size_t totalRead = 0;
    bool last, firstRead = true;
    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint32_t offset = 0;
    char dataBuf[bufSize];
    uint32_t bytesPerPacket, packetCount;

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;


    while (true) {
        nBytes = getPacketizedBuffer(dataBuf, bufSize, udpSocket,
                                     debug, firstRead, &last, &tick, &offset,
                                     &bytesPerPacket, &packetCount, outOfOrderPackets);
        if (nBytes < 0) {
            if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
            break;
        }
        totalRead += nBytes;
        firstRead = false;

        //printBytes(dataBuf, nBytes, "buffer ---->");

        // Write out what was received
        writeBuffer(dataBuf, nBytes, fp, debug);

        if (last) {
            if (debug) fprintf(stderr, "Read last packet from incoming data, quit\n");
            break;
        }

        if (debug) fprintf(stderr, "Read %ld bytes from incoming reassembled packet\n", nBytes);
    }

    if (debug) fprintf(stderr, "Read %ld incoming data bytes\n", totalRead);

    if (writingToFile && (fclose(fp) == -1)) {
        if (debug) fprintf(stderr, "fclose: %s\n", strerror(errno));
        exit(1);
    }

    return 0;
}

