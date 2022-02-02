//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * <p>
 * @file Send file (read or piped to) to an ejfat router (FPGA-based or simulated)
 * which then passes it to a program to reassemble (possibly udp_rcv_order.cc).
 * This sender, by default, prepends an LB header to the data in order
 * to test it with the receiver. This can be removed in the ejfat_packetize.hpp
 * file by commenting out:
 * </p>
 * <b>#define ADD_LB_HEADER 1</b>
 */



#include "ejfat_packetize.hpp"

using namespace ersap::ejfat;

#define INPUT_LENGTH_MAX 256



static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] ",
            "        [-host <destination host (defaults to 127.0.0.1)>]",
            "        [-p <destination UDP port>]",
            "        [-i <outgoing interface name (e.g. eth0, currently only used to find MTU)>]",
            "        [-mtu <desired MTU size>]",
            "        [<input file name (or \"test\")>]");

    fprintf(stderr, "        This is an EJFAT UDP packet sender.\n");
}



static void parseArgs(int argc, char **argv, int* mtu, uint16_t* port,
                      char *fileName, char* host, char *interface) {

    *mtu = 0;
    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {{"mtu",  1, NULL, 1},
             {"host",  1, NULL, 2},
             {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:i:", long_options, 0)) != EOF) {

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
                    fprintf(stderr, "Invalid argument to -mtu. MTU buffer size must be > 100.\n");
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

            case 'v':
                // VERBOSE
                debug = true;
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
    if(   !optarg
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

    uint32_t offset = 0;
    uint16_t port = 0x4c42; // FPGA port is default
    uint16_t tick = 0xc0da;
    int mtu;

    char fileName[INPUT_LENGTH_MAX], host[INPUT_LENGTH_MAX], interface[16];

    memset(host, 0, INPUT_LENGTH_MAX);
    memset(interface, 0, 16);
    memset(fileName, 0, INPUT_LENGTH_MAX);

    // Default to sending to local host
    strcpy(host, "127.0.0.1");
    strcpy(interface, "lo0");

    parseArgs(argc, argv, &mtu, &port, fileName, host, interface);


    // Break data into multiple packets of max MTU size.
    // If the mtu was not set on the command line, get it progamatically
    if (mtu == 0) {
        mtu = getMTU(interface);
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

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr(host);
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);


    // use filename provided as 1st argument (stdin by default)
    bool readingFromFile = false;
    bool testOutOfOrder = false;
    size_t fileSize = 0L;
    // For most efficient use of UDP packets, make our buffer a multiple of maxUdpPayload
    int bufsize = (100000 / maxUdpPayload + 1) * maxUdpPayload;

    // For sending out-of-order
    uint32_t *order;
    int packetCounter = 0;
    const int testPacketCount = 15;


    // Read from either file or stdin, or create test data
    FILE *fp = nullptr;
    if (strlen(fileName) > 0) {
        printf("File name = %s\n", fileName);

        if (strncmp(fileName, "test", 4) == 0) {
            // Use test data generated right here
            testOutOfOrder = true;
            maxUdpPayload = 100;
            bufsize = mtu = maxUdpPayload + 60 + 8 + HEADER_BYTES;
            uint32_t myOrder[testPacketCount] {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
            //uint32_t myOrder[testPacketCount] {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
            //uint32_t myOrder[testPacketCount] {1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 14};
            order = myOrder;
        }
        else {
            // Open and read file
            fp = fopen( fileName, "r");
            readingFromFile = true;
            // find the size of the file
            fseek(fp, 0L, SEEK_END);
            fileSize = ftell(fp);
            rewind(fp);
        }
    }
    else {
        fp = stdin;
    }

    // validate file open for reading
    if (!testOutOfOrder && !fp) {
        perror ("file open failed");
        return 1;
    }

    if (debug) printf("Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

    char buf[bufsize];
    size_t nBytes, totalBytes = 0;
    bool firstBuffer = false;
    bool lastBuffer  = false;

    while (true) {
        if (!testOutOfOrder) {
            nBytes = fread(buf, 1, bufsize, fp);

            // Done
            if (nBytes == 0) {
                printf("\n ******* Last read returned 0, END reading\n\n");
                break;
            }

            // Error
            if (ferror(fp)) {
                printf("\n ******* Last read returned error, nBytes = %lu\n\n", nBytes);
                break;
            }

            totalBytes += nBytes;
        }

        if (readingFromFile) {
            if (totalBytes == fileSize) {
                lastBuffer = true;
            }
        }
        else if (testOutOfOrder) {
            // Sending test data created here

            offset = order[packetCounter];
            lastBuffer = false;
            firstBuffer = false;

            // At the end of data
            if (packetCounter > (testPacketCount - 1)) {
                break;
            }

            // Last packet (when properly ordered) to send
            if (offset == (testPacketCount - 1)) {
                lastBuffer = true;
            }

            // First packet (when properly ordered) to send
            if (offset == 0) {
                firstBuffer = true;
            }

            // Put in some fake data
            memset(buf, offset, bufsize);
            nBytes = 100;
            totalBytes += nBytes;
            packetCounter++;
        }
        else {
            // if using stdin
            if (feof(fp)) {
                // We've reached the EOF with last read
                printf("\n ******* FOUND EOF for reading from stdin, just read in %lu bytes\n\n", nBytes);
                lastBuffer = true;
            }
        }

        sendPacketizedBuffer(buf, nBytes, maxUdpPayload, clientSocket, &serverAddr,
                             tick, &offset, firstBuffer, lastBuffer);
        firstBuffer = false;
        if (testOutOfOrder) {
            if (packetCounter == testPacketCount) {
                printf("\n ******* last buffer send, END reading\n\n");
                break;
            }
        }
        else {
            if (lastBuffer) {
                printf("\n ******* last buffer send, END reading\n\n");
                break;
            }
        }
    }

    printf("\n ******* Sent a total of %lu data bytes\n", totalBytes);


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
/*
 * will egress traffic out with the lowest numbered interface:
 */

//    server_addr.sin_addr.s_addr = INADDR_ANY;

/* 
 * But if you need to explicitely assign your IP address, then you will have to create another sin.addr construct and give it the IP in network byte order then: 
 */

//    destination.sin_addr.s_addr = inet_addr("10.10.10.100");

/* 
 * Then call your sendto to use the struct with the above declaration. Please note that the sendto does have an extra struct argument that allows for this.
 * Added sockaddr *myDestination here as an example 
 */

//   sendto(int fd,const void *msg, int len, unsigned int flags, const struct sockaddr *myDestination, int tolen);
