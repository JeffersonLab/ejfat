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


#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <map>
#include <getopt.h>
#include <cinttypes>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#ifdef __APPLE__
#include <cctype>
#endif


/** Union to facilitate unpacking of RE UDP header. */
union reHeader {
    struct __attribute__((packed))re_hdr {
        uint32_t version    : 4;
        uint32_t reserved   : 10;
        uint32_t first      : 1;
        uint32_t last       : 1;
        uint32_t data_id    : 16;
        uint32_t sequence   : 32;
    } reFields;

    uint32_t remWords[2];
};

#define HEADER_BYTES 8


static bool debug = true;
#define btoa(x) ((x)?"true":"false")


enum errorCodes {
    RECV_MSG = -1,
    TRUNCATED_MSG = -2,
    BUF_TOO_SMALL = -3,
    OUT_OF_ORDER = -4,
    BAD_FIRST_LAST_BIT = -5,
    OUT_OF_MEM = -6
};


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------



/**
 * This routine takes a pointer and prints out (to stderr) the desired number of bytes
 * from the given position, in hex.
 *
 * @param data      data to print out
 * @param bytes     number of bytes to print in hex
 * @param label     a label to print as header
 */
static void printBytes(const char *data, uint32_t bytes, const char *label) {

    if (label != nullptr) fprintf(stderr, "%s:\n", label);

    if (bytes < 1) {
        fprintf(stderr, "<no bytes to print ...>\n");
        return;
    }

    int i;
    for (i=0; i < bytes; i++) {
        if (i%10 == 0) {
            fprintf(stderr, "\n  Buf(%3d - %3d) =  ", (i+1), (i + 10));
        }
        else if (i%5 == 0) {
            fprintf(stderr, "  ");
        }

        // Accessing buf in this way does not change position or limit of buffer
        fprintf(stderr, "  0x%02x ", (int)(*((data + i))));
    }

    fprintf(stderr, "\n\n");
}


/**
 * This routine takes a file pointer and prints out (to stderr) the desired number of bytes
 * from the given file, in hex.
 *
 * @param data      data to print out
 * @param bytes     number of bytes to print in hex
 * @param label     a label to print as header
 */
static void printFileBytes(FILE *fp, uint32_t bytes, const char *label) {

    long currentPos = ftell(fp);
    rewind(fp);
    uint8_t byte;


    if (label != nullptr) fprintf(stderr, "%s:\n", label);

    if (bytes < 1) {
        fprintf(stderr, "<no bytes to print ...>\n");
        return;
    }

    int i;
    for (i=0; i < bytes; i++) {
        if (i%10 == 0) {
            fprintf(stderr, "\n  Buf(%3d - %3d) =  ", (i+1), (i + 10));
        }
        else if (i%5 == 0) {
            fprintf(stderr, "  ");
        }

        // Accessing buf in this way does not change position or limit of buffer
        fread(&byte, 1, 1, fp);
        fprintf(stderr, "  0x%02x ", byte);
    }

    fprintf(stderr, "\n\n");
    fseek(fp, currentPos, SEEK_SET);
}


/**
 * <p>
 * Routine to read a single UDP packet into 2 buffers with one system call.
 * The first buffer filled will contain the reassemby header used in the EJFAT project.
 * The second buffer will contain all the rest of the data sent.
 * </p>
 *
 * It's the responsibility of the caller to have at least enough space in the
 * buffer for 1 MTU of data. Otherwise, the caller risks truncating the data
 * of a packet and having error code of TRUNCATED_MSG returned.
 *
 *
 * @param dataBuf   buffer in which to store actual data read (not any headers).
 * @param bufLen    available bytes in dataBuf in which to safely write.
 * @param udpSocket UDP socket to read.
 * @param sequence  to be filled with packet sequence read from RE header.
 * @param dataId    to be filled with data id read from RE header.
 * @param version   to be filled with version read from RE header.
 * @param first     to be filled with "first" bit read from RE header,
 *                  indicating the first packet in a series used to send data.
 * @param last      to be filled with "last" bit id read from RE header,
 *                  indicating the last packet in a series used to send data.
 * @return number of data (not headers!) bytes read from packet.
 *         If there's an error in recvmsg, it will return RECV_MSG.
 *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
 */
int readPacket(char *dataBuf, int bufLen, int udpSocket,
                uint32_t* sequence, int* dataId, int* version,
                bool *first, bool *last) {

    // Storage for RE header
    union reHeader header;

    // Prepare a msghdr structure to receive multiple buffers with one system call.
    // One buffer will contain the header.
    // Second buffer will contain the data being sent.
    // Doing things this way also eliminates having to copy the data.
    struct msghdr msg;
    struct iovec iov[2];

    memset(&msg, 0, sizeof(msg));
    memset(iov, 0, sizeof(iov));

    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    iov[0].iov_base = (void *) header.remWords;
    iov[0].iov_len = HEADER_BYTES;

    iov[1].iov_base = (void *) dataBuf;
    iov[1].iov_len = bufLen;


    int bytesRead = recvmsg(udpSocket, &msg, 0);
    if (bytesRead < 0) {
        if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));

        return(RECV_MSG);
    }
    else if (msg.msg_flags == MSG_TRUNC) {
        // end of datagram discarded as dataBuf not big enough
        if (debug) fprintf(stderr, "recvmsg() discarded valid data, receiving buffer too small, %d bytes\n", (int) bufLen);
        return(TRUNCATED_MSG);
    }

    // Do any necessary swapping
    header.remWords[0] = ntohl(header.remWords[0]);
    header.remWords[1] = ntohl(header.remWords[1]);
    if (debug) fprintf(stderr, "\nRE first word = 0x%x, seq = %u\n", header.remWords[0], header.remWords[1]);

    // Parse header & return values
    if (dataId != nullptr) {
        *dataId = header.reFields.data_id;
    }

    if (version != nullptr) {
        *version = header.reFields.version;
    }

    if (first != nullptr) {
        *first = header.reFields.first;
    }

    if (last != nullptr) {
        *last = header.reFields.last;
    }

    if (sequence != nullptr) {
        *sequence = header.reFields.sequence;
    }

    return bytesRead - HEADER_BYTES;
}



void freeMap(std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {
    for (const auto& n : outOfOrderPackets) {
        // Free allocated buffer holding packet
        free(std::get<0>(n.second));
    }
}


void writePacketData(char *buf, int dataBytes) {
    //memcpy()

}


/**
 * Assemble incoming packets into the given buffer.
 * It will return when the buffer has less space left than it read from the first packet
 * or when the "last" bit is set in a packet.
 * This routine allows for out-of-order packets.
 *
 * @param dataBuf   place to store assembled packets.
 * @param bufLen    byte length of dataBuf.
 * @param udpSocket UDP socket to read.
 * @param veryFirstRead this is the very first time data will be read for a sequence of same-tick packets.
 * @param last      to be filled with "last" bit id read from RE header,
 *                  indicating the last packet in a series used to send data.
 * @param expSequence  value-result parameter which gives the next expected sequence to be
 *                     read from RE header and returns its updated value
 *                     indicating its sequence in the flow of packets.
 * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
 *
 * @return If there's an error in recvmsg, it will return RECV_MSG.
 *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
 *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
 *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
 *              it will return OUT_OF_ORDER.
 *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
 *         If cannot allocate memory, it will return OUT_OF_MEM.
 */
int getPacketizedBuffer(char* dataBuf, int bufLen, int udpSocket,
                        bool veryFirstRead, bool *last, uint32_t *expSequence,
                        std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {

    // TODO: build if sequence is file offset

    // uint32_t packetsInBuf = 0, maxPacketsInBuf = 0;
    uint32_t sequence, expectedSequence = *expSequence;

    bool packetFirst, packetLast, firstReadForBuf = false, tooLittleRoom = false;
    int  dataId, version, nBytes, maxPacketBytes = 0, totalBytesRead = 0;

    char *putDataAt = dataBuf;
    int remainingLen = bufLen;

    if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %d\n", remainingLen);

    while (true) {
        // Read in one packet
        nBytes = readPacket(putDataAt, remainingLen, udpSocket,
                            &sequence, &dataId, &version,
                            &packetFirst, &packetLast);

        // If error
        if (nBytes < 0) {
            return nBytes;
        }

        if (sequence == 0) {
            firstReadForBuf = true;
        }

        if (debug) fprintf(stderr, "Received %d bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
                           nBytes, sequence, btoa(packetLast), btoa(firstReadForBuf));

        // Check to see if packet is in sequence
        if (sequence != expectedSequence) {
            if (debug) fprintf(stderr, "\n    Got seq %u, expecting %u\n", sequence, expectedSequence);

            // If we get one that we already received, ERROR!
            if (sequence < expectedSequence) {
                freeMap(outOfOrderPackets);
                if (debug) fprintf(stderr, "    Already got seq %u once before!\n", sequence);
                return OUT_OF_ORDER;
            }

            // Set a limit on how much we're going to store (100 packets) while we wait
            if (outOfOrderPackets.size() >= 100) {
                freeMap(outOfOrderPackets);
                if (debug) fprintf(stderr, "    Reached limit (100) of stored packets!\n");
                return OUT_OF_ORDER;
            }

            // Since it's out of order, what was written into dataBuf will need to be
            // copied and stored. And that written data will eventually need to be
            // overwritten with the correct packet data.
            char *tempBuf = (char *) malloc(nBytes);
            if (tempBuf == nullptr) {
                freeMap(outOfOrderPackets);
                return OUT_OF_MEM;
            }
            memcpy(tempBuf, putDataAt, nBytes);

            // Put it into map
            if (debug) fprintf(stderr, "    Save and store packet %u, packetLast = %s\n", sequence, btoa(packetLast));
            outOfOrderPackets.emplace(sequence, std::tuple<char *, uint32_t, bool, bool>{tempBuf, nBytes, packetLast, packetFirst});
            // Read next packet
            continue;
        }

        while (true) {
            if (debug) fprintf(stderr, "\nPacket %u in proper order, last = %s\n", sequence, btoa(packetLast));

            // Packet was in proper order. Get ready to look for next in sequence.
            putDataAt += nBytes;
            remainingLen -= nBytes;
            totalBytesRead += nBytes;
            expectedSequence++;
            //packetsInBuf++;

            // If it's the first read of a sequence, and there are more reads to come,
            // the # of bytes it read will be max possible. Remember that.
            if (firstReadForBuf) {
                maxPacketBytes = nBytes;
                firstReadForBuf = false;
                //maxPacketsInBuf = bufLen / maxPacketBytes;
                if (debug) fprintf(stderr, "In first read, max bytes/packet = %d\n", maxPacketBytes);

                // Error check
                if (veryFirstRead && !packetFirst) {
                    if (debug) fprintf(stderr, "Expecting first bit to be set on very first read but wasn't\n");
                    freeMap(outOfOrderPackets);
                    return BAD_FIRST_LAST_BIT;
                }
            }
            else if (packetFirst) {
                if (debug) fprintf(stderr, "Expecting first bit NOT to be set on read but was\n");
                freeMap(outOfOrderPackets);
                return BAD_FIRST_LAST_BIT;
            }

            if (debug) fprintf(stderr, "remainingLen = %d, expected offset = %u, first = %s, last = %s\n",
                               remainingLen, expectedSequence, btoa(packetFirst), btoa(packetLast));

            // If no stored, out-of-order packets ...
            if (outOfOrderPackets.empty()) {
                // If very last packet, quit
                if (packetLast) {
                    break;
                }

                // Another mtu of data (as reckoned by source) will exceed buffer space, so quit
                if (remainingLen < maxPacketBytes) {
                    tooLittleRoom = true;
                    break;
                }
            }
            // If there were previous packets out-of-order, they may now be in order.
            // If so, write them into buffer.
            // Remember the map already sorts them into proper sequence.
            else {
                if (debug) fprintf(stderr, "We also have stored packets\n");
                // Go to first stored packet
                auto it = outOfOrderPackets.begin();

                // If it's truly the next packet ...
                if (it->first == expectedSequence) {
                    char *data  = std::get<0>(it->second);
                    nBytes      = std::get<1>(it->second);
                    packetLast  = std::get<2>(it->second);
                    packetFirst = std::get<3>(it->second);
                    sequence = expectedSequence;

                    memcpy(putDataAt, data, nBytes);
                    free(data);

                    // Remove packet from map
                    it = outOfOrderPackets.erase(it);
                    if (debug) fprintf(stderr, "Go and add stored packet %u, size of map = %lu, last = %s\n",
                                       expectedSequence, outOfOrderPackets.size(), btoa(packetLast));
                    continue;
                }
            }

            break;
        }

        if (packetLast || tooLittleRoom) {
            break;
        }
     }

    if (debug) fprintf(stderr, "getPacketizedBuffer: passing offset = %u\n\n", expectedSequence);

    *last = packetLast;
    *expSequence = expectedSequence;

    return totalBytesRead;
}




/**
 * Routine to process the data. In this case, write it to file pointer (file or stdout)
 *
 * @param dataBuf buffer filled with data.
 * @param nBytes  number of valid bytes.
 * @param fp      file pointer.
 * @return error code of 0 means OK. If there is an error, programs exits.
 */
int writeBuffer(const char* dataBuf, size_t nBytes, FILE* fp) {

    size_t n, totalWritten = 0;

    while (true) {
        n = fwrite(dataBuf, 1, nBytes, fp);

        // Error
        if (n != nBytes) {
            if (debug) fprintf(stderr, "\n ******* Last write had error, n = %lu, expected %ld\n\n", n, nBytes);
            if (debug) fprintf(stderr, "write error: %s\n", strerror(errno));
            exit(1);
        }

        totalWritten += n;
        if (totalWritten >= nBytes) {
            break;
        }
    }

    if (debug) fprintf(stderr, "writeBuffer: wrote %lu bytes\n", totalWritten);

    return 0;
}




#define INPUT_LENGTH_MAX 256



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



static void parseArgs(int argc, char **argv, int* bufSize, uint16_t* port,
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
    unsigned short port = 7777;

    char fileName[INPUT_LENGTH_MAX], listeningAddr[16];
    memset(fileName, 0, INPUT_LENGTH_MAX);
    memset(listeningAddr, 0, 16);

    parseArgs(argc, argv, &bufSize, &port, fileName, listeningAddr);

    // Create UDP socket
    udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
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

    // validate file open for reading
    if (!fp) {
        fprintf(stderr, "file open failed: %s\n", strerror(errno));
        return 1;
    }

    size_t totalRead = 0;
    bool last, firstRead = true;
    // Start with offset 0 in very first packet to be read
    uint32_t offset = 0;
    char dataBuf[bufSize];

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;


    while (true) {
        nBytes = getPacketizedBuffer(dataBuf, bufSize, udpSocket, firstRead, &last, &offset, outOfOrderPackets);
        if (nBytes < 0) {
            if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
            break;
        }
        totalRead += nBytes;
        firstRead = false;

        printBytes(dataBuf, nBytes, "buffer ---->");

        // Write out what was received
        writeBuffer(dataBuf, nBytes, fp);


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

