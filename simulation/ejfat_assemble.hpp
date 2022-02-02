//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Contains routines to receive UDP packets that have been "packetized"
 * (broken up into smaller UDP packets by an EJFAT packetizer).
 * The receiving program handles sequentially numbered packets that may arrive out-of-order
 * coming from an FPGA-based between this and the sending program.
 */
#ifndef EJFAT_ASSEMBLE_H
#define EJFAT_ASSEMBLE_H


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


#define HEADER_BYTES 8
#define btoa(x) ((x)?"true":"false")


namespace ersap {
    namespace ejfat {

        static bool debug = true;

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


        enum errorCodes {
            RECV_MSG = -1,
            TRUNCATED_MSG = -2,
            BUF_TOO_SMALL = -3,
            OUT_OF_ORDER = -4,
            BAD_FIRST_LAST_BIT = -5,
            OUT_OF_MEM = -6,
            BAD_ARG = -7
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

            uint32_t i;
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

            uint32_t i;
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
        static int readPacket(char *dataBuf, size_t bufLen, int udpSocket,
                              uint32_t* sequence, int* dataId, int* version,
                              bool *first, bool *last) {

            // Storage for RE header
            union reHeader header{};

            // Prepare a msghdr structure to receive multiple buffers with one system call.
            // One buffer will contain the header.
            // Second buffer will contain the data being sent.
            // Doing things this way also eliminates having to copy the data.
            struct msghdr msg{};
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



        static void freeMap(std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {
            for (const auto& n : outOfOrderPackets) {
                // Free allocated buffer holding packet
                free(std::get<0>(n.second));
            }
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
         * @param bytesPerPacket  pointer to int which get filled with the very first packet's data byte length
         *                        (not including header). This gives us an indication of the MTU.
         * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
         *
         * @return total bytes read.
         *  If there's an error in recvmsg, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         */
        static ssize_t getPacketizedBuffer(char* dataBuf, size_t bufLen, int udpSocket,
                                           bool veryFirstRead, bool *last, uint32_t *expSequence, uint32_t *bytesPerPacket,
                                           std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {

            // TODO: build if sequence is file offset

            // uint32_t packetsInBuf = 0, maxPacketsInBuf = 0;
            uint32_t sequence, expectedSequence = *expSequence;

            bool packetFirst, packetLast, firstReadForBuf = false, tooLittleRoom = false;
            int  dataId, version, nBytes;
            size_t maxPacketBytes = 0;
            ssize_t totalBytesRead = 0;

            char *putDataAt = dataBuf;
            size_t remainingLen = bufLen;

            if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu\n", remainingLen);

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
                    *bytesPerPacket = nBytes - HEADER_BYTES;
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
                        if (debug) fprintf(stderr, "In first read, max bytes/packet = %lu\n", maxPacketBytes);

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

                    if (debug) fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s\n",
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


        /**
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets.
         *
         * @param userBuf       address of pointer to data buffer if noCopy is true.
         *                      Otherwise, this is used to return a locally allocated data buffer.
         * @param userBufLen    pointer to byte length of dataBuf if noCopy is true.
         *                      Otherwise it returns the size of the data buffer returned.
         * @param port          UDP port to read on.
         * @param listeningAddr if specified, this is the IP address to listen on (dot-decimal form).
         * @param noCopy        If true, write data directly into userBuf. If there's not enough room, an error is thrown.
         *                      If false, an internal buffer is allocated and returned in the userBuf arg.
         *
         * @return 0 if success.
         *         If there's an error in recvmsg, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         *         If userBuf is null or *userBuf is null when noCopy is true, it will return BAD_ARG.
         */
        static int getBuffer(char** userBuf, size_t *userBufLen, unsigned short port, const char *listeningAddr, bool noCopy) {


            if (userBuf == nullptr || userBufLen == nullptr) {
                return BAD_ARG;
            }

            port = port < 1024 ? 7777 : port;

            // Create UDP socket
            int udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

            // Configure settings in address struct
            struct sockaddr_in serverAddr{};
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(port);
            if (listeningAddr != nullptr && strlen(listeningAddr) > 0) {
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


            ssize_t nBytes, totalBytes = 0;
            bool last, firstRead = true;
            // Start with sequence 0 in very first packet to be read
            uint32_t sequence = 0;
            uint32_t bytesPerPacket = 0;

            // Map to hold out-of-order packets.
            // map key = sequence from incoming packet
            // map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
            // (is last packet), (is first packet).
            std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;


            if (noCopy) {
                // If it's no-copy, we give the reading routine the user's whole buffer ONCE and have it filled.
                // Write directly into user-specified buffer.
                // In this case, the user knows how much data is coming and provides
                // a buffer big enough to hold it all. If not, error.

                if (*userBuf == nullptr) {
                    return BAD_ARG;
                }

                nBytes = getPacketizedBuffer(*userBuf, *userBufLen, udpSocket,
                                             firstRead, &last, &sequence, &bytesPerPacket,
                                             outOfOrderPackets);
                if (totalBytes < 0) {
                    if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
                    // Return the error
                    freeMap(outOfOrderPackets);
                    return totalBytes;
                }

                if (!last) {
                    if (debug) fprintf(stderr, "Buffer full, but no last packet found, likely buffer is too small\n");
                    freeMap(outOfOrderPackets);
                    return BUF_TOO_SMALL;
                }

                totalBytes = nBytes;
            }
            else {
                // We're using our own internally allocated buffer.
                // We may have to loop through to read everything, since we don't know how much data is coming
                // Create an internal buffer that we'll fill and return.
                // If it's too small, it will be reallocated and data copied over.

                uint32_t dataBufLen = 100000;
                char *dataBuf = (char *) malloc(dataBufLen);
                if (dataBuf == nullptr) {
                    return OUT_OF_ORDER;
                }
                char *getDataFrom = dataBuf;
                uint32_t remaningBytes = dataBufLen;

                while (true) {
                    nBytes = getPacketizedBuffer(getDataFrom, remaningBytes, udpSocket,
                                                 firstRead, &last, &sequence, &bytesPerPacket,
                                                 outOfOrderPackets);
                    if (nBytes < 0) {
                        if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
                        // Return the error
                        freeMap(outOfOrderPackets);
                        return totalBytes;
                    }

                    totalBytes += nBytes;
                    getDataFrom += nBytes;
                    remaningBytes -= nBytes;
                    firstRead = false;

                    //printBytes(dataBuf, nBytes, "buffer ---->");

                    if (last) {
                        if (debug) fprintf(stderr, "Read last packet from incoming data, quit\n");
                        break;
                    }

                    // Do we have to allocate more memory? Double it if so.
                    if (remaningBytes < bytesPerPacket) {
                        dataBuf = (char *)realloc(dataBuf, 2*dataBufLen);
                        if (dataBuf == nullptr) {
                            freeMap(outOfOrderPackets);
                            return OUT_OF_MEM;
                        }
                        getDataFrom = dataBuf + totalBytes;
                        dataBufLen *= 2;
                        remaningBytes = dataBufLen - totalBytes;
                        if (debug) fprintf(stderr, "Reallocate buffer to %u bytes\n", dataBufLen);
                    }

                    if (debug) fprintf(stderr, "Read %ld bytes from incoming reassembled packet\n", nBytes);
                }

                *userBuf = dataBuf;
                *userBufLen = totalBytes;
            }

            freeMap(outOfOrderPackets);

            if (debug) fprintf(stderr, "Read %ld incoming data bytes\n", totalBytes);

            return 0;
        }

    }
}


#endif // EJFAT_ASSEMBLE_H
