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

#include "time.h"

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
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-fast] [-recvmsg]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer byte size]",
            "        [-r <UDP receive buffer byte size]",
            "        <output file name>");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver.\n");
    fprintf(stderr, "        The -fast option uses recvfrom & minimizes data copying by reading into final buf\n");
    fprintf(stderr, "        The -recvmsg option uses recvmsg to read directly into final buf\n");
    fprintf(stderr, "        Specifying neither flag uses recvfrom to read data, then copies into final buf\n");
}


/**
 * Parse all command line options.
 *
 * @param argc        arg count from main().
 * @param argv        arg list from main().
 * @param bufSize     filled with buffer size.
 * @param recvBufSize filled with UDP receive buffer size.
 * @param port        filled with UDP port to listen on.
 * @param debug       filled with debug flag.
 * @param fileName    filled with output file name.
 * @param listenAddr  filled with IP address to listen on.
 * @param fast        filled with true if reading with recvfrom and minimizing data copy.
 * @param recvmsg     filled with true if reading with recvmsg.
 */
static void parseArgs(int argc, char **argv, int* bufSize, int *recvBufSize, uint16_t* port, bool *debug,
                      char *fileName, char *listenAddr, bool *fast, bool *recvmsg) {

    int c, i_tmp;
    bool help = false, useFast = false, useRecvMsg = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"fast",  0, NULL, 1},
             {"recvmsg",  0, NULL, 2},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:", long_options, 0)) != EOF) {

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
                if (i_tmp >= 10000) {
                    *bufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -b, internal buf size >= 10kB\n");
                    exit(-1);
                }
                break;

            case 'r':
                // UDP RECEIVE BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 220000) {
                    *recvBufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -r, UDP recv buf size >= 220kB\n");
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

            case 1:
                // use "recvfrom" to receive UDP packets and avoid data copying
                if (useRecvMsg) {
                    fprintf(stderr, "Can only have EITHER -recvmsg OR -fast\n");
                    exit(-1);
                }
                fprintf(stdout, "Use \"recvfrom w/ minimal copy\"\n");
                useFast = true;
                *fast = true;
                break;

            case 2:
                // use "recvmsg" to receive UDP packets
                if (useFast) {
                    fprintf(stderr, "Can only have EITHER -recvmsg OR -fast\n");
                    exit(-1);
                }
                fprintf(stdout, "Use \"recvmsg\"\n");
                useRecvMsg = true;
                *recvmsg = true;
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
    int bufSize = 1020000;
    int recvBufSize = 0;
    uint16_t port = 7777;

    bool debug = false;
    bool useFast = false;
    bool useRecvmsg = false;
    bool useRecvfrom = false;

    char fileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(fileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    parseArgs(argc, argv, &bufSize, &recvBufSize, &port, &debug, fileName, listeningAddr, &useFast, &useRecvmsg);

    if (!(useFast || useRecvmsg)) {
      useRecvfrom = true;
    }

    // Create UDP socket
    udpSocket = socket(AF_INET, SOCK_DGRAM, 0);

#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif
    setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
    // Read back what we supposedly set
    socklen_t size = sizeof(int);
    int recvBufBytes = 0;
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
        fprintf(stderr, "bind socket error\n");
        return -1;
    }

    bool last, firstRead = true, firstLoop = true;
    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint32_t offset = 0;
    // If bufSize gets too big, it exceeds stack limits, so lets malloc it!

    char *dataBuf = (char *) malloc(bufSize);
    if (dataBuf == NULL) {
        fprintf(stderr, "cannot allocate internal buffer memory of %d bytes\n", bufSize);
        return -1;
    }

    fprintf(stderr, "Internal buffer size = %d bytes\n", bufSize);

    uint32_t bytesPerPacket;

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;

    // Statistics
    uint32_t packetCount=0;
    int64_t packetsRead=0, byteCount=0, totalBytes=0, totalPackets=0;
    double rate = 0.0, avgRate = 0.0;
    int64_t totalT = 0, time, time1, time2;
    struct timespec t1, t2;

    // Get the current time
    clock_gettime(CLOCK_REALTIME, &t1);
    time1 = 1000L*t1.tv_sec + t1.tv_nsec/1000000L; // milliseconds

    while (true) {
        if (useFast) {
            if (debug) fprintf(stderr, "calling getPacketizerBufferFast\n");
            nBytes = getPacketizedBufferFast(dataBuf, bufSize, udpSocket,
                                             debug, firstRead, &last, &tick, &offset,
                                             &bytesPerPacket, &packetCount, outOfOrderPackets);
        }
        else {
            nBytes = getPacketizedBuffer(dataBuf, bufSize, udpSocket,
                                         debug, firstRead, &last, useRecvfrom, &tick, &offset,
                                         &bytesPerPacket, &packetCount, outOfOrderPackets);
        }

        if (nBytes < 0) {
            if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
            break;
        }

        byteCount   += nBytes;
        packetsRead += packetCount;
        offset = 0;

        // stats
        clock_gettime(CLOCK_REALTIME, &t2);
        time2 = 1000L*t2.tv_sec + t2.tv_nsec/1000000L; /* milliseconds */
        time = time2 - time1;

        if (time > 5000) {
            // Ignore the very first counts as blastee starts before blaster and
            // that messes up the stats.

            // reset things if #s rolling over
            if (firstLoop || (totalBytes < 0) || (totalT < 0))  {
                totalT = totalBytes = totalPackets = byteCount = packetsRead = 0;
                time1 = time2;
                firstLoop = false;
                continue;
            }

            /* Packet rates */
            rate = 1000.0 * ((double) packetsRead) / time;
            totalPackets += packetsRead;
            totalT += time;
            avgRate = 1000.0 * ((double) totalPackets) / totalT;
            printf(" Packets:  %3.4g Hz,    %3.4g Avg.\n", rate, avgRate);

            /* Actual Data rates (no header info) */
            rate = ((double) byteCount) / (1000*time);
            totalBytes += byteCount;
            avgRate = ((double) totalBytes) / (1000*totalT);
            printf(" Data:    %3.4g MB/s,  %3.4g Avg.\n\n", rate, avgRate);

            byteCount = packetsRead = 0;

            clock_gettime(CLOCK_REALTIME, &t1);
            time1 = 1000L*t1.tv_sec + t1.tv_nsec/1000000L;
        }

    }

    return 0;
}

