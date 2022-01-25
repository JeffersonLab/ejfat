/************* UDP SERVER CODE *******************/

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <cassert>
#include <map>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

#ifdef __APPLE__
#include <cctype>
#endif


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

/**
 * map key = offset from incoming packet
 * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes), (is last packet)
 */
std::map<uint32_t, std::tuple<char *, uint32_t, bool>> outOfOrderPackets;

void freeMap() {
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
 * @param expOffset  value-result parameter which gives the next expected offset to be
 *                   read from RE header and returns its updated value
 *                   indicating its sequence in the flow of packets.
 *
 * @return If there's an error in recvmsg, it will return RECV_MSG.
 *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
 *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
 *         If a packet is out of order, it will return OUT_OF_ORDER.
 *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
 *         If cannot allocate memory, it will return OUT_OF_MEM.
 */
int getPacketizedBuffer(char* dataBuf, int bufLen, int udpSocket, int mtu,
                        bool veryFirstRead, bool *last, uint32_t *expOffset) {

    // TODO: build if sequence is file offset

    uint32_t packetsInBuf = 0, maxPacketsInBuf = 0;

    bool packetFirst, packetLast, firstReadForBuf = true;
    int  dataId, version, nBytes, maxPacketBytes = 0, totalBytesRead = 0;

    uint32_t offset, expectedOffset = *expOffset;

    char *putDataAt = dataBuf;
    int remainingLen = bufLen;

    if (bufLen < mtu) {
        // There may be trouble, increase the size of the receiving buffer
        return BUF_TOO_SMALL;
    }

    if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %d\n", remainingLen);

    while (true) {
        // Read in one packet
        nBytes = readPacket(putDataAt, remainingLen, udpSocket,
                            &offset, &dataId, &version,
                            &packetFirst, &packetLast);

        // If error
        if (nBytes < 0) {
            return nBytes;
        }

        if (debug) fprintf(stderr, "Received %d bytes from sender in packet #%d\n", nBytes, offset);

        // Check to see if packet is in sequence
        if (offset != expectedOffset) {
            // Since it's out of order, what was written into dataBuf will need to be
            // copied and stored. And that data will eventually need to be overwritten
            // with the correct packet data.
            char *tempBuf = (char *) malloc(nBytes);
            if (tempBuf == nullptr) {
                freeMap();
                return OUT_OF_MEM;
            }
            memcpy(tempBuf, putDataAt, nBytes);

            // What if it's the last Packet???

            // Put it into map
            outOfOrderPackets.emplace(offset, std::tuple<char *, uint32_t, bool>{tempBuf, nBytes, packetLast});
            // Read next packet
            continue;
        }

        while (true) {
            // Packet was in proper order. Get ready to look for next in sequence.
            putDataAt += nBytes;
            remainingLen -= nBytes;
            totalBytesRead += nBytes;
            packetsInBuf++;
            expectedOffset++;

            // If it's the first read of a sequence, and there are more reads to come,
            // the # of bytes it read will be max possible. Remember that.
            if (firstReadForBuf) {
                maxPacketBytes = nBytes;
                firstReadForBuf = false;
                maxPacketsInBuf = bufLen / maxPacketBytes;

                // Error check
                if (veryFirstRead && !packetFirst) {
                    if (debug) fprintf(stderr, "Expecting first bit to be set on very first read but wasn't\n");
                    return BAD_FIRST_LAST_BIT;
                }
            }
            else if (packetFirst) {
                if (debug) fprintf(stderr, "Expecting first bit NOT to be set on read but was\n");
                return BAD_FIRST_LAST_BIT;
            }

            if (debug) fprintf(stderr, "remainingLen = %d\n", remainingLen);
            if (debug) fprintf(stderr, "getPacketizedBuffer: set expected offset = %u\n", expectedOffset);

            // If very last packet, quit
            if (packetLast) {
                break;
            }

            // If there were previous packets out-of-order, they may now be in order.
            // If so, write them into buffer.
            if (!outOfOrderPackets.empty()) {
                // Go to first stored packet
                auto it = outOfOrderPackets.begin();

                // If it's truly the next packet ...
                if (it->first == expectedOffset) {
                    char *data = std::get<0>(it->second);
                    nBytes = std::get<1>(it->second);
                    packetLast = std::get<2>(it->second);
                    memcpy(putDataAt, data, nBytes);

                    // Remove packet from map
                    it = outOfOrderPackets.erase(it);
                    continue;
                }
            }
            break;
        }

        // Reading another mtu of data (as reckoned by source) will exceed buffer space, so quit
        if (remainingLen < maxPacketBytes) {
            break;
        }

    }

    if (debug) fprintf(stderr, "getPacketizedBuffer: passing offset = %u\n\n", expectedOffset);

    *last = packetLast;
    *expOffset = expectedOffset;

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
int writeBuffer(char* dataBuf, size_t nBytes, FILE* fp) {

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



int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    // Set this to max expected data size
    int BUFSIZE = 100000;
    char dataBuf[BUFSIZE];
    struct sockaddr_in serverAddr;

    // Create UDP socket
    udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(7891);
    //serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_addr.s_addr = INADDR_ANY;
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
    if (argc > 1) {
        fp = fopen (argv[1], "w");
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
    int mtu = 1400;
    // Start with offset 0 in very first packet to be read
    uint32_t offset = 0;


    while (true) {
        nBytes = getPacketizedBuffer(dataBuf, BUFSIZE, udpSocket, mtu, firstRead, &last, &offset);
        if (nBytes < 0) {
            if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
            break;
        }
        totalRead += nBytes;
        firstRead = false;

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

