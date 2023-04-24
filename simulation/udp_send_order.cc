//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Send file (read or piped to) to an ejfat LB
 * which then passes it to a program to reassemble (udp_rcv_order.cc).
 */


#include "ejfat_packetize.hpp"

using namespace ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] ",
            "        [-host <destination host (defaults to 127.0.0.1)>]",
            "        [-p <destination UDP port>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
            "        [-mtu <desired MTU size>]",
            "        [-t <tick>]",
            "        [-ver <version>]",
            "        [-id <data id>]",
            "        [-e <entropy>]",
            "        [-pro <protocol>]",
            "        [-c <# of times to read thru file or out-of-order (test) data w/ increasing tick>]",
            "        [-d <delay in millisec between packets>]",
            "        [<input file name>]");

    fprintf(stderr, "        This is an EJFAT UDP packet sender which packetizes and sends the given file\n");
}



static void parseArgs(int argc, char **argv, int* mtu, int *protocol,
                      int *version, uint16_t *id, uint16_t* port, int *entropy,
                      uint64_t* tick, uint32_t* delay, uint32_t *cycleCount,
                      bool *debug, char *fileName, char* host, char *interface) {

    *mtu = 0;
    int c, i_tmp;
    int64_t tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"mtu",   1, nullptr, 1},
             {"host",  1, nullptr, 2},
             {"ver",   1, nullptr, 3},
             {"id",    1, nullptr, 4},
             {"pro",   1, nullptr, 5},
             {nullptr,       0, nullptr,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:i:t:d:c:e:", long_options, nullptr)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 't':
                // TICK
                tmp = strtoll(optarg, nullptr, 0);
                if (tmp > -1) {
                    *tick = tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -t, tick > 0\n");
                    exit(-1);
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

            case 'd':
                // DELAY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *delay = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -d, packet delay > 0\n");
                    exit(-1);
                }
                break;

            case 'e':
                // ENTROPY
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 0) {
                    *entropy = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -e, entropy >= 0\n");
                    exit(-1);
                }
                break;

            case 'c':
                // CYCLE COUNT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 0) {
                    *cycleCount = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -c, cycle count > 0\n");
                    exit(-1);
                }
                break;

            case 'i':
                // OUTGOING INTERFACE NAME / IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "interface address is bad\n");
                    exit(-1);
                }
                strcpy(interface, optarg);
                break;

            case 1:
                // MTU
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 100) {
                    fprintf(stderr, "Invalid argument to -mtu. MTU buffer size must be > 100\n");
                    exit(-1);
                }
                *mtu = i_tmp;
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
                // VERSION
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 31) {
                    fprintf(stderr, "Invalid argument to -ver. Version must be >= 0 and < 32\n");
                    exit(-1);
                }
                *version = i_tmp;
                break;

            case 4:
                // DATA_ID
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0 || i_tmp > 65535) {
                    fprintf(stderr, "Invalid argument to -id. Id must be >= 0 and < 65536\n");
                    exit(-1);
                }
                *id = i_tmp;
                break;

            case 5:
                // PROTOCOL
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp < 0) {
                    fprintf(stderr, "Invalid argument to -pro. Protocol must be >= 0\n");
                    exit(-1);
                }
                *protocol = i_tmp;
                break;

            case 'v':
                // VERBOSE
                *debug = true;
                break;

            case 'h':
                fprintf(stderr, "Set argument -h to help\n");
                help = true;
                break;

            default:
                printHelp(argv[0]);
                exit(2);
        }

    }

    // Grab any default args not in option list
    if (  !optarg
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



/**
 * Doing things this way is like reading a buffer bit-by-bit
 * and passing it off to the parser bit-by-bit
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {

    uint32_t offset = 0, delay = 0, delayPrescale = 1;
    uint32_t cycleCount=1;  // # of times to cycle through sending same data but with advancing tick
    uint16_t port = 0x4c42; // FPGA port is default
    uint64_t tick = 1;
    int mtu, version = 2, protocol = 1, entropy = 0;
    uint16_t dataId = 1;
    bool debug = false;

    char fileName[INPUT_LENGTH_MAX], host[INPUT_LENGTH_MAX], interface[16];

    memset(host, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    memset(fileName, 0, INPUT_LENGTH_MAX);

    // Default to sending to local host
    strcpy(host, "127.0.0.1");
    strcpy(interface, "lo0");

    parseArgs(argc, argv, &mtu, &protocol, &version, &dataId, &port, &entropy, &tick,
              &delay, &cycleCount, &debug, fileName, host, interface);


    // Break data into multiple packets of max MTU size.
    // If the mtu was not set on the command line, get it progamatically
    if (mtu == 0) {
        mtu = getMTU(interface, debug);
    }

    // Jumbo (> 1500) ethernet frames are 9000 bytes max.
    // Don't exceed this limit.
    if (mtu > 9000) {
        mtu = 9000;
    }

    // 60 bytes = max IPv4 packet header, 8 bytes = max UDP packet header
    // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
    int maxUdpPayload = mtu - 60 - 8 - HEADER_BYTES;

    // Create UDP socket
    int clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Try to increase send buf size to 25 MB
    socklen_t size = sizeof(int);
    int sendBufBytes = 25000000;
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
    sendBufBytes = 0; // clear it
    getsockopt(clientSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, &size);
    fprintf(stderr, "UDP socket send buffer = %d bytes\n", sendBufBytes);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr(host);
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

    int err = connect(clientSocket, (const sockaddr *)&serverAddr, sizeof(struct sockaddr_in));
    if (err < 0) {
        if (debug) perror("Error connecting UDP socket:");
        close(clientSocket);
        return err;
    }

    // use filename provided as 1st argument (stdin by default)
    bool readingFromFile = false;
    size_t fileSize = 0L;
    // For most efficient use of UDP packets, make our buffer a multiple of maxUdpPayload
    int bufsize = (100000 / maxUdpPayload + 1) * maxUdpPayload;

    // Read from either file or stdin, or create test data
    FILE *fp;
    if (strlen(fileName) > 0) {
        fprintf(stderr, "File name = %s\n", fileName);
        // Open and read file
        fp = fopen( fileName, "r");
        if (fp == nullptr) {
            fprintf(stderr, "\n ******* file %s, does not exist or cannot open\n\n", fileName);
            return -1;
        }
        readingFromFile = true;

        // find the size of the file
        fseek(fp, 0L, SEEK_END);
        fileSize = ftell(fp);
        if (fileSize > UINT32_MAX) {
            fprintf(stderr, "\n ******* file %s, too large to send as a \"single\" buffer, %zu \n\n", fileName, fileSize);
            return -1;
        }

        rewind(fp);
    }
    else {
        fp = stdin;
    }

    // validate file open for reading
    if (!fp) {
        perror ("file open failed");
        return 1;
    }

    if (debug) fprintf(stderr, "Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

    char buf[bufsize];
    int64_t packetsSent = 0;
    size_t nBytes, totalBytes = 0;
    bool firstBuffer = true;
    bool lastBuffer  = false;
    uint32_t delayCounter = delayPrescale;


    while (true) {
        nBytes = fread(buf, 1, bufsize, fp);

        // Check for error
        if (ferror(fp)) {
            fprintf(stderr, "\n ******* Last read returned error, ret val = %lu, err = %s\n\n", nBytes, strerror(errno));
            break;
        }

        // if done reading stdin
        if (nBytes == 0) {
            fprintf(stderr, "\n ******* Last read returned 0, END reading\n\n");
            break;
        }

        totalBytes += nBytes;

        if (readingFromFile) {
            if (totalBytes >= fileSize) {
                lastBuffer = true;
            }
        }
        else {
            // if using stdin
            if (feof(fp)) {
                // We've reached the EOF with last read
                fprintf(stderr, "\n ******* FOUND EOF for reading from stdin, just read in %lu bytes\n\n", nBytes);
                lastBuffer = true;
            }
        }

//fprintf(stderr, "Sending offset = %u, tick = %" PRIu64 "\n", offset, tick);
        err = sendPacketizedBufferFastNew(buf, nBytes, maxUdpPayload, clientSocket,
                                          tick, protocol, entropy, version, dataId, fileSize,
                                          &offset, delay, delayPrescale, &delayCounter,
                                          firstBuffer, lastBuffer, debug, &packetsSent);

//        err = sendPacketizedBufferFast(buf, nBytes, maxUdpPayload, clientSocket,
//                                       tick, protocol, entropy, version, dataId, &offset,
//                                       delay, delayPrescale, &delayCounter,
//                                       firstBuffer, lastBuffer, debug, &packetsSent);
        if (err < 0) {
            // Should be more info in errno
            fprintf(stderr, "\nsendPacketizedBuffer: %s\n\n", strerror(errno));
            exit(1);
        }

        firstBuffer = false;
        if (lastBuffer) {
            // If we're reading a file and sending it multiple times with increasing tick ...
            if (readingFromFile && (--cycleCount > 0)) {
                fprintf(stderr, "CYCLING AROUND AGAIN\n");
                firstBuffer = true;
                lastBuffer  = false;
                offset = 0;
                totalBytes = 0;
                tick++;
                rewind(fp);
                continue;
            }
            fprintf(stderr, "\n ******* last buffer sent, END reading\n\n");
            break;
        }
    }

    fprintf(stderr, "\n ******* Sent a total of %lu data bytes\n\n", totalBytes);

    if (nBytes == -1) {
        perror("read: ");
        exit(1);
    }

    if (readingFromFile && (fclose(fp) == -1)) {
        perror("close");
        exit(1);
    }

    return 0;
}
