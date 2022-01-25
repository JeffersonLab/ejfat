/************* UDP SERVER CODE *******************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>

#ifdef __APPLE__
#include <ctype.h>
#endif


// Add boolean support to C, as type and to print
#include <stdbool.h>
#define btoa(x) ((x)?"true":"false")

static bool debug = 1;

// Is this going to an FPGA or FPGA simulator?
// i.e. will the LB header be stripped off at receiving end?
//#define TO_FPGA 1

#ifdef TO_FPGA
    #define LB_HEADER_BYTES 0
    #define HEADER_BYTES    8
#else
    #define LB_HEADER_BYTES 12
    #define HEADER_BYTES    20
#endif


enum errorCodes {
    RECV_MSG = -1,
    TRUNCATED_MSG = -2,
    BUF_TOO_SMALL = -3,
    OUT_OF_ORDER = -4,
    BAD_FIRST_LAST_BIT = -5
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

    if (label != NULL) fprintf(stderr, "%s:\n", label);

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
 * The first buffer filled will contain the load balancing header (if existing)
 * followed by the reassemby header used in the EJFAT project.
 * The second buffer will contain all the rest of the data sent.
 * </p>
 *
 * It's the responsibility of the caller to have at least enough space in the
 * buffer for 1 MTU of data. Otherwise, the caller risks truncating the data
 * of a packet and having error code of -2 returned.
 *
 *
 * @param dataBuf   buffer in which to store actual data read (not any headers).
 * @param bufLen    available bytes in dataBuf in which to safely write.
 * @param udpSocket UDP socket to read.
 * @param offset    to be filled with packet offset read from RE header.
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
                uint32_t* offset, int* dataId, int* version,
                bool *first, bool *last) {

    // Uncomment for reading source address
    //struct sockaddr_storage src_addr;

    // Storage for all headers
    char headerBuf[HEADER_BYTES];

    // Prepare a msghdr structure to receive multiple buffers with one system call.
    // One buffer will contain the all headers .
    // The second buffer will contain the data being sent.
    // Doing things this way also eliminates having to copy all the data.
    struct msghdr msg;
    struct iovec iov[2];

    memset(&msg, 0, sizeof(msg));
    memset(iov, 0, sizeof(iov));

    // Uncomment for reading source address
    //msg.msg_name = &src_addr;
    //msg.msg_namelen = sizeof(src_addr);

    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    iov[0].iov_base = (void *) headerBuf;
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

    // Parse header & return values
    uint32_t firstWord = ntohl(*((uint32_t *)(headerBuf + LB_HEADER_BYTES)));
    if (debug) fprintf(stderr, "\nRE first word = 0x%x\n", firstWord);
    if (dataId != NULL) {
        *dataId = (firstWord >> 16) & 0xffff;
    }

    if (version != NULL) {
        *version = firstWord & 0xf;
    }

    if (first != NULL) {
        *first = (firstWord >> 14) & 0x1;
    }

    if (last != NULL) {
        *last = (firstWord >> 15) & 0x1;
    }

    uint32_t packetOffset = ntohl(*((uint32_t *) (headerBuf + LB_HEADER_BYTES + 4)));
    if (debug) fprintf(stderr, "readPacket: packetOffset read as %u\n", packetOffset);
    if (offset != NULL) {
        *offset = packetOffset;
    }

    return bytesRead - HEADER_BYTES;
}


/**
 * Assemble incoming packets into the given buffer.
 * It will return when the buffer has less space left than it read in the first packet
 * or when the "last" bit is set in a packet.
 *
 * @param dataBuf   place to store assembled packets.
 * @param bufLen    byte length of dataBuf.
 * @param udpSocket UDP socket to read.
 * @param veryFirstRead this is the very first time data will be read for a sequence of same-tick packets.
 * @param last      to be filled with "last" bit id read from RE header,
 *                  indicating the last packet in a series used to send data.
 * @param exOffset  value-result parameter which gives the next expected offset to be
 *                  read from RE header and returns its updated value
 *                  indicating its sequence in the flow of packets.
 *
 * @return If there's an error in recvmsg, it will return RECV_MSG.
 *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
 *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
 *         If a packet is out of order, it will return OUT_OF_ORDER.
 *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
 */
int getPacketizedBuffer(char* dataBuf, int bufLen, int udpSocket, int mtu,
                        bool veryFirstRead, bool *last, uint32_t *exOffset) {

    // Build assuming, for the moment, sequential packet numbers starting with 0.
    // TODO: Later build if sequential buffer offsets on writing side
    // TODO: Still later allow for out-of-order packets

    assert(last);
    assert(dataBuf);
    assert(exOffset);

    bool packetFirst, packetlast, firstReadForBuf = true;
    int  dataId, version, nBytes, maxPacketBytes = 0, totalBytesRead = 0;

    uint32_t offset, expectedOffset = *exOffset;
    char *putDataAt = dataBuf;
    int remainingLen = bufLen;

    if (bufLen < mtu) {
        // There may be trouble, increase the size of the receiving buffer
        return BUF_TOO_SMALL;
    }

    if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %d\n", remainingLen);


    while (1) {

        // Read in one packet
        nBytes = readPacket(putDataAt, remainingLen, udpSocket,
                            &offset, &dataId, &version,
                            &packetFirst, &packetlast);

        // If error
        if (nBytes < 0) {
            return nBytes;
        }

        if (debug) fprintf(stderr, "Received %d bytes from sender in packet #%d\n", nBytes, offset);

        putDataAt += nBytes;
        remainingLen -= nBytes;
        totalBytesRead += nBytes;
        if (debug) fprintf(stderr, "remainingLen = %d\n", remainingLen);

        // Check to see if packet # is in sequence
        if (offset != expectedOffset) {
            if (debug) fprintf(stderr, "Expecting offset = %u, but got %u\n", expectedOffset, offset);
            return OUT_OF_ORDER;
        }

        // Look for next in sequence (at least for now)
        expectedOffset++;
        if (debug) fprintf(stderr, "getPacketizedBuffer: set expected offset = %u\n", expectedOffset);

        // If it's the first read of a sequence, and there are more reads to come,
        // the # of bytes it read will be max possible. Remember that.
        if (firstReadForBuf) {
            maxPacketBytes = nBytes;
            firstReadForBuf = false;

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

        // If very last packet, quit
        if (packetlast) {
            break;
        }

        // Reading another mtu of data (as reckoned by source) will exceed buffer space, so quit
        if (remainingLen < maxPacketBytes) {
            break;
        }
    }

    if (debug) fprintf(stderr, "getPacketizedBuffer: passing offset = %u\n\n", expectedOffset);
    *last = packetlast;
    *exOffset = expectedOffset;

    return totalBytesRead;
}



int main1(int argc, char **argv) {

    int udpSocket, nBytes;
    // Set this to max expected data size
    char dataBuf[100000];
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
        if (debug) perror ("socket bind failed: ");
    }

    bool firstBuffer = true, last;
    int mtu = 1400;
    // Start with offset 0 in very first packet to be read
    uint32_t offset = 0;

    while(true) {
        if (debug) printf("Try calling getPacketizerBuffer");
        nBytes = getPacketizedBuffer(dataBuf, 100000, udpSocket, mtu, firstBuffer, &last, &offset);
        if (nBytes < 0) {
            if (debug) printf("Error in getPacketizerBuffer");
            break;
        }
        if (debug) printf("Read %d bytes from incoming reassembled message", nBytes);
        printBytes(dataBuf, nBytes, "packet");
    }

    return 0;
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

    FILE *fp = NULL;
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
