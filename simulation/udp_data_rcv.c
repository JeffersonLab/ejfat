//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive locally generated data sent by udp_data_send.c program
 * This assumes no emulator between the 2 programs. That can be changed
 * by commenting out the "#define NO_FPGA" line.
 */


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>



// Add boolean support to C, as type and to print
#include <stdbool.h>
#define btoa(x) ((x)?"true":"false")

static bool debug = 1;


#define NO_FPGA 1

#ifdef NO_FPGA
    #define LB_HEADER_BYTES 12
#else
    #define LB_HEADER_BYTES 0
#endif

#define RE_HEADER_BYTES  8


/**
   * This method takes a pointer and prints out the desired number of bytes
   * from the given position, in hex.
   *
   * @param data      data to print out
   * @param bytes     number of bytes to print in hex
   * @param label     a label to print as header
   */
static void printBytes(const char *data, uint32_t bytes, const char *label) {

    if (label != NULL) printf("%s:\n", label);

    if (bytes < 1) {
        printf("<no bytes to print ...>\n");
        return;
    }

    int i;
    for (i=0; i < bytes; i++) {
        if (i%10 == 0) {
            printf("\n  Buf(%3d - %3d) =  ", (i+1), (i + 10));
        }
        else if (i%5 == 0) {
            printf("  ");
        }

        // Accessing buf in this way does not change position or limit of buffer
        printf("  0x%02x ", (int)(*((data + i))));
    }

    printf("\n\n");
}


/**
 * Routine to read a UDP packet into 2 buffers with one system call.
 * The first buffer filled will contain the reassemby header used in the EJFAT project.
 * The second buffer will contain all the rest of the data sent.
 *
 * @param dataBuf   buffer in which to store actual data (not RE header) read.
 * @param bufLen    available bytes in dataBuf in which to safely write.
 * @param udpSocket UDP socket to read.
 * @param offset    to be filled with packet offset read from RE header.
 * @param dataId    to be filled with data id read from RE header.
 * @param version   to be filled with version read from RE header.
 * @param first     to be filled with "first" bit read from RE header,
 *                  indicating the first packet in a series used to send data.
 * @param last      to be filled with "last" bit id read from RE header,
 *                  indicating the last packet in a series used to send data.
 * @return number of data (not header!) bytes read from packet.
 *         If there's an error in recvmsg, it will return -1.
 *         If the packet data is NOT completely read (truncated), it will return -2.
 */
int readPacket(char *dataBuf, int bufLen, int udpSocket,
                int* offset, int* dataId, int* version,
                bool *first, bool *last) {

    // Uncomment for reading source address
    //struct sockaddr_storage src_addr;

#ifdef NO_FPGA
    // Storage for LB header (temporary until Mike's simulator is available)
    char lbBuf[LB_HEADER_BYTES];
#endif

    // Storage for RE header
    char headerBuf[RE_HEADER_BYTES];

    // Prepare a msghdr structure to receive 2 buffers with one system call.
    // One buffer will contain the RE header and while the other will contain data being sent.
    // Doing things this way also eliminates having to copy all the data.
    struct msghdr msg;
    struct iovec iov[3];

    memset(&msg, 0, sizeof(msg));
    memset(iov, 0, sizeof(iov));

    // Uncomment for reading source address
    //msg.msg_name = &src_addr;
    //msg.msg_namelen = sizeof(src_addr);

    msg.msg_iov = iov;

#ifdef NO_FPGA

    msg.msg_iovlen = 3;

    iov[0].iov_base = (void *) lbBuf;
    iov[0].iov_len = LB_HEADER_BYTES;

    iov[1].iov_base = (void *) headerBuf;
    iov[1].iov_len = RE_HEADER_BYTES;

    iov[2].iov_base = (void *) dataBuf;
    iov[2].iov_len = bufLen;

#else

    msg.msg_iovlen = 2;

    iov[0].iov_base = (void *) headerBuf;
    iov[0].iov_len = RE_HEADER_BYTES;

    iov[1].iov_base = (void *) dataBuf;
    iov[1].iov_len = bufLen;

#endif

    printf("Waiting on recvmsg\n");
    int bytesRead = recvmsg(udpSocket, &msg, 0);
    if (bytesRead < 0) {
        perror("recvmsg() failed: ");
        return(-1);
    }
    else if (msg.msg_flags == MSG_TRUNC) {
        // end of datagram discarded as dataBuf not big enough
        printf("recvmsg() discarded valid data, receiving buffer too small, %d bytes\n", (int) bufLen);
        return(-2);
    }

    printf("Got msg!\n");

    // Parse header & return values
    uint32_t firstWord = ntohl(*((uint32_t *)headerBuf));
    printf("RE first word = 0x%x\n", firstWord);
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

    uint32_t packetOffset = ntohl(*((uint32_t *) (headerBuf + 4)));
    if (offset != NULL) {
        *offset = packetOffset;
    }

    return bytesRead - RE_HEADER_BYTES - LB_HEADER_BYTES;
}


int getPacketizedBuffer(char* dataBuf, int bufLen, int udpSocket) {

    // Build assuming, for the moment, sequential packet numbers starting with 0.
    // TODO: Later build if sequential buffer offsets on writing side
    // TODO: Still later allow for out-of-order packets

    bool first = true, last = false, shouldBeLastPkt = false;
    int offset, dataId, version, nBytes, packetCount = 0, maxPacketBytes = 0, totalBytesRead = 0;

    char *putDataAt = dataBuf, *biggerBuf = NULL;
    int remainingLen = bufLen;

    while(1) {

        // Read in one packet
        nBytes = readPacket(putDataAt, remainingLen, udpSocket,
                            &offset, &dataId, &version,
                            &first, &last);

        if (nBytes == -1) {
            // Error in recvmsg
            return -1;
        }
        else if (nBytes == -2) {
            // Data packet read was truncated (will never be > available memory)
            return -2;
        }

        printf("Received %d bytes from sender in packet #%d\n", nBytes, packetCount);

        remainingLen -= nBytes;
        totalBytesRead += nBytes;
        putDataAt += nBytes;

        // Check to see if packet # makes sense
        if (offset != packetCount) {
            printf("Expecting offset = %d, but got %d\n", packetCount, offset);
            break;
        }

        // If it's the first read of a sequence,
        // the # of bytes it read will be max possible. Store that
        if (first) {
           maxPacketBytes = nBytes;
        }

        // If last packet, quit
        if (last) {
            break;
        }
        else if (shouldBeLastPkt) {
            printf("This should be last packet, but isn't labeled as last\nremaining buffer room = %d, full packet contains %d bytes\n",
                   remainingLen, maxPacketBytes);
            break;
        }

        // Flag to warn if next packet read should be the last
        if (remainingLen < maxPacketBytes) {
            shouldBeLastPkt = true;
        }

        packetCount++;
    }

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
    bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));

    int counter = 0;

    while(true) {
        printf("Try calling getPacketizerBuffer");
        int nBytes = getPacketizedBuffer(dataBuf, 100000, udpSocket);
        if (nBytes < 0) {
            printf("Error in getPacketizerBuffer");
            break;
        }
        printf("Read %d bytes from incoming reassembled message", nBytes);
        printBytes(dataBuf, nBytes, "packet");
    }

    return 0;
}


int main(int argc, char **argv) {

    int udpSocket, nBytes;
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
    bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));

    int counter = 0;



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
        perror ("file open failed");
        return 1;
    }

    // Read from either file or stdin.
    // Reading from file systems, it's efficient to read in blocks of 16MB
    ssize_t n = 0, nTotal = 0;
    // change from file pointer to descriptor
    int fd = fileno(fp);

    while (true) {
        printf("Call getPacketizerBuffer");
        int nBytes = getPacketizedBuffer(dataBuf, BUFSIZE, udpSocket);
        if (nBytes < 0) {
            printf("Error in getPacketizerBuffer");
            break;
        }

        // Write out what was received
        while (true) {
            n = write(fd, dataBuf, nBytes);
            if (n == -1) {
                perror("write: ");
                exit(1);
            }
            fflush(fp);

            nTotal += n;
            if (nTotal >= nBytes) {
                break;
            }
        }

        // TODO: How do we know when the end comes ????


        printf("Read %d bytes from incoming reassembled packet", nBytes);
        //printBytes(dataBuf, nBytes, "packet");
    }


    if (writingToFile && (close(fd) == -1)) {
        perror("close");
        exit(1);
    }





    return 0;
}
