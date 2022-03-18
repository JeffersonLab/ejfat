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
#ifndef EJFAT_ASSEMBLE_ERSAP_H
#define EJFAT_ASSEMBLE_ERSAP_H


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

#define HEADER_BYTES 16
#define btoa(x) ((x)?"true":"false")


#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


#ifndef _BYTESWAP_H
#define _BYTESWAP_H

static inline uint16_t bswap_16(uint16_t x) {
    return (x>>8) | (x<<8);
}

static inline uint32_t bswap_32(uint32_t x) {
    return (bswap_16(x&0xffff)<<16) | (bswap_16(x>>16));
}

static inline uint64_t bswap_64(uint64_t x) {
    return (((uint64_t)bswap_32(x&0xffffffffull))<<32) |
           (bswap_32(x>>32));
}
#endif


namespace ersap {
    namespace ejfat {

        enum errorCodes {
            RECV_MSG = -1,
            TRUNCATED_MSG = -2,
            BUF_TOO_SMALL = -3,
            OUT_OF_ORDER = -4,
            BAD_FIRST_LAST_BIT = -5,
            OUT_OF_MEM = -6,
            BAD_ARG = -7
        };


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
                if (i%8 == 0) {
                    fprintf(stderr, "\n  Buf(%3d - %3d) =  ", (i+1), (i + 8));
                }
                else if (i%4 == 0) {
                    fprintf(stderr, "  ");
                }

                // Accessing buf in this way does not change position or limit of buffer
                fprintf(stderr, "%02x ",( ((int)(*(data + i))) & 0xff)  );
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
         * Parse the load balance header at the start of the given buffer.
         *
         * @param buffer   buffer to parse.
         * @param ll       return 1st byte as char.
         * @param bb       return 2nd byte as char.
         * @param version  return 3rd byte as integer version.
         * @param protocol return 4th byte as integer protocol.
         * @param tick     return last 8 bytes as 64 bit integer tick.
         */
        static void parseLbHeader(char* buffer, char* ll, char* bb,
                                  uint32_t* version, uint32_t* protocol,
                                  uint64_t* tick)
        {
            // protocol 'L:8, B:8, Version:8, Protocol:8, Tick:64'
            // 0                   1                   2                   3
            // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |       L       |       B       |    Version    |    Protocol   |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |                                                               |
            // +                              Tick                             +
            // |                                                               |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            *ll = buffer[0];
            *bb = buffer[1];
            *version  = (uint32_t)buffer[2];
            *protocol = (uint32_t)buffer[3];
            *tick     = ntohll(*((uint64_t *)(&buffer[4])));
        }


        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in pointer args.
         *
         * @param buffer   buffer to parse.
         * @param version  returned version.
         * @param first    returned is-first-packet value.
         * @param last     returned is-last-packet value.
         * @param dataId   returned data source id.
         * @param sequence returned packet sequence number.
         * @param tick     returned tick value, also in LB meta data.
         */
        static void parseReHeader(char* buffer, int* version,
                                  bool *first, bool *last,
                                  uint16_t* dataId, uint32_t* sequence,
                                  uint64_t *tick)
        {
            // protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
            // 0                   1                   2                   3
            // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |Version|        Rsvd       |F|L|            Data-ID            |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |                  UDP Packet Offset                            |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |                                                               |
            // +                              Tick                             +
            // |                                                               |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

            // Now pull out the component values
            *version = (buffer[0] & 0xf0) >> 4;
            *first   = (buffer[1] & 0x02) >> 1;
            *last    =  buffer[1] & 0x01;

            *dataId   = ntohs(*((uint16_t *) (buffer + 2)));
            *sequence = ntohl(*((uint32_t *) (buffer + 4)));
            *tick     = ntohll(*((uint64_t *) (buffer + 8)));
        }


        //-----------------------------------------------------------------------
        // Be sure to print to stderr as programs may pipe data to stdout!!!
        //-----------------------------------------------------------------------


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
         * @param tick      to be filled with tick from RE header.
         * @param sequence  to be filled with packet sequence from RE header.
         * @param dataId    to be filled with data id read RE header.
         * @param version   to be filled with version read RE header.
         * @param first     to be filled with "first" bit from RE header,
         *                  indicating the first packet in a series used to send data.
         * @param last      to be filled with "last" bit id from RE header,
         *                  indicating the last packet in a series used to send data.
         * @param debug     turn debug printout on & off.
         *
         * @return number of data (not headers!) bytes read from packet.
         *         If there's an error in recvmsg, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         */
        static int readPacket(char *dataBuf, size_t bufLen, int udpSocket,
                              uint64_t *tick, uint32_t* sequence, uint16_t* dataId, int* version,
                              bool *first, bool *last, bool debug) {

            // Storage for RE header
            //union reHeader header{};
            char header[HEADER_BYTES];

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

            //iov[0].iov_base = (void *) header.reWords;
            iov[0].iov_base = (void *) header;
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

            // Parse header
            parseReHeader(header, version, first, last, dataId, sequence, tick);

            return bytesRead - HEADER_BYTES;
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
         * @param tick      to be filled with tick from RE header.
         * @param sequence  to be filled with packet sequence from RE header.
         * @param dataId    to be filled with data id read RE header.
         * @param version   to be filled with version read RE header.
         * @param first     to be filled with "first" bit from RE header,
         *                  indicating the first packet in a series used to send data.
         * @param last      to be filled with "last" bit id from RE header,
         *                  indicating the last packet in a series used to send data.
         * @param debug     turn debug printout on & off.
         *
         * @return number of data (not headers!) bytes read from packet.
         *         If there's an error in recvfrom, it will return RECV_MSG.
         *         If there is not enough room in dataBuf to hold incoming data, it will return BUF_TOO_SMALL.
         */
        static int readPacketRecvFrom(char *dataBuf, size_t bufLen, int udpSocket,
                              uint64_t *tick, uint32_t* sequence, uint16_t* dataId, int* version,
                              bool *first, bool *last, bool debug) {

            // Storage for packet
            char pkt[9100];

            int bytesRead = recvfrom(udpSocket, pkt, 9100, 0, NULL, NULL);
            if (bytesRead < 0) {
                if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));

                return(RECV_MSG);
            }

            if (bufLen < bytesRead) {
                return(BUF_TOO_SMALL);
            }

            // Parse header
            parseReHeader(pkt, version, first, last, dataId, sequence, tick);

            // Copy datq
            memcpy(dataBuf, pkt + HEADER_BYTES, bytesRead);

            return bytesRead - HEADER_BYTES;
        }


        static void freeMap(std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {
            for (const auto& n : outOfOrderPackets) {
                // Free allocated buffer holding packet
                free(std::get<0>(n.second));
            }
        }



        /** <p>
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets.</p>
         *
         * This routine uses recvfrom to read in packets, but minimizes the copying of data
         * by copying as much data as possible, directly to dataBuf. This involves storing
         * what temporarily gets overwritten by a RE header and then restoring it once the
         * read of a packet is complete.
         *
         * @param dataBuf           place to store assembled packets.
         * @param bufLen            byte length of dataBuf.
         * @param udpSocket         UDP socket to read.
         * @param debug             turn debug printout on & off.
         * @param veryFirstRead     this is the very first time data will be read for a sequence of same-tick packets.
         * @param last              to be filled with "last" bit id from RE header,
         *                          indicating the last packet in a series used to send data.
         * @param tick              to be filled with tick from RE header.
         * @param expSequence       value-result parameter which gives the next expected sequence to be
         *                          read from RE header and returns its updated value
         *                          indicating its sequence in the flow of packets.
         * @param bytesPerPacket    pointer to int which get filled with the very first packet's data byte length
         *                          (not including header). This gives us an indication of the MTU.
         * @param packetCount       pointer to int which get filled with the number of in-order packets read.
         * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
         *
         * @return total bytes read.
         *         If there's an error in recvfrom, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         */
        static ssize_t getPacketizedBufferFast(char* dataBuf, size_t bufLen, int udpSocket,
                                               bool debug, bool veryFirstRead, bool *last,
                                               uint64_t *tick, uint32_t *expSequence,
                                               uint32_t *bytesPerPacket, uint32_t *packetCount,
                                               std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {

            // TODO: build if sequence is file offset

            uint64_t packetTick;
            uint32_t sequence, expectedSequence = *expSequence;

            bool packetFirst, packetLast, firstReadForBuf = false, tooLittleRoom = false;
            int  version, nBytes;
            uint16_t dataId;
            uint32_t pktCount = 0;
            size_t  maxPacketBytes = 0;
            ssize_t totalBytesRead = 0;

            char headerStorage[HEADER_BYTES];

            char *writeHeaderAt, *putDataAt = dataBuf;
            size_t remainingLen = bufLen;

            fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu\n", remainingLen);

            while (true) {

                if (veryFirstRead) {
                    // Read in one packet
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &sequence, &dataId, &version,
                                                &packetFirst, &packetLast, debug);
                    // If error
                    if (nBytes < 0) {
                        return nBytes;
                    }
                }
                else {
                    writeHeaderAt = putDataAt - HEADER_BYTES;
                    // Copy part of buffer that we'll temporarily overwrite
                    memcpy(headerStorage, writeHeaderAt, HEADER_BYTES);

                    // Read data right into final buffer (including RE header)
                    nBytes = recvfrom(udpSocket, writeHeaderAt, remainingLen, 0, NULL, NULL);
                    if (nBytes < 0) {
                        if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));

                        return(RECV_MSG);
                    }

                    // Parse header
                    parseReHeader(writeHeaderAt, &version, &packetFirst, &packetLast, &dataId, &sequence, &packetTick);

                    // Replace what was written over
                    memcpy(writeHeaderAt, headerStorage, HEADER_BYTES);
                }

                // TODO: What if we get a zero-length packet???

                if (sequence == 0) {
                    firstReadForBuf = true;
                    *bytesPerPacket = nBytes - HEADER_BYTES;
                }

                if (debug) fprintf(stderr, "Received %d bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
                                   nBytes, sequence, btoa(packetLast), btoa(firstReadForBuf));

                // Check to see if packet is ou-of-sequence
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
                    pktCount++;
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

                veryFirstRead = false;

                if (packetLast || tooLittleRoom) {
                    break;
                }
            }

            if (debug) fprintf(stderr, "getPacketizedBuffer: passing offset = %u\n\n", expectedSequence);

            *last = packetLast;
            *tick = packetTick;
            *expSequence = expectedSequence;
            *packetCount = pktCount;

            return totalBytesRead;
        }


        /**
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets.
         *
         * @param dataBuf           place to store assembled packets.
         * @param bufLen            byte length of dataBuf.
         * @param udpSocket         UDP socket to read.
         * @param debug             turn debug printout on & off.
         * @param veryFirstRead     this is the very first time data will be read for a sequence of same-tick packets.
         * @param last              to be filled with "last" bit id from RE header,
         *                          indicating the last packet in a series used to send data.
         * @param useRecvFrom       if true, use recvfrom to read (and copy data to dataBuf), else
         *                          use recvmsg to read.
         * @param tick              to be filled with tick from RE header.
         * @param expSequence       value-result parameter which gives the next expected sequence to be
         *                          read from RE header and returns its updated value
         *                          indicating its sequence in the flow of packets.
         * @param bytesPerPacket    pointer to int which get filled with the very first packet's data byte length
         *                          (not including header). This gives us an indication of the MTU.
         * @param packetCount       pointer to int which get filled with the number of in-order packets read.
         * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
         *
         * @return total bytes read.
         *         If there's an error in recvmsg/recvfrom, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         */
        static ssize_t getPacketizedBuffer(char* dataBuf, size_t bufLen, int udpSocket,
                                           bool debug, bool veryFirstRead, bool *last, bool useRecvFrom,
                                           uint64_t *tick, uint32_t *expSequence,
                                           uint32_t *bytesPerPacket, uint32_t *packetCount,
                                           std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {

            // TODO: build if sequence is file offset

            uint64_t packetTick;
            uint32_t sequence, expectedSequence = *expSequence;

            bool packetFirst, packetLast, firstReadForBuf = false, tooLittleRoom = false;
            int  version, nBytes;
            uint16_t dataId;
            uint32_t pktCount = 0;
            size_t  maxPacketBytes = 0;
            ssize_t totalBytesRead = 0;

            char *putDataAt = dataBuf;
            size_t remainingLen = bufLen;

            if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu\n", remainingLen);

            while (true) {
                // Read in one packet
                if (useRecvFrom) {
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &sequence, &dataId, &version,
                                                &packetFirst, &packetLast, debug);
                }
                else {
                    nBytes = readPacket(putDataAt, remainingLen, udpSocket,
                                        &packetTick, &sequence, &dataId, &version,
                                        &packetFirst, &packetLast, debug);
                }

                // If error
                if (nBytes < 0) {
                    return nBytes;
                }

                // TODO: What if we get a zero-length packet???

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
                    pktCount++;
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
            *tick = packetTick;
            *expSequence = expectedSequence;
            *packetCount = pktCount;

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
        static int writeBuffer(const char* dataBuf, size_t nBytes, FILE* fp, bool debug) {

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
         * @param debug         turn debug printout on & off.
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
        static int getBuffer(char** userBuf, size_t *userBufLen,
                             uint16_t port, const char *listeningAddr,
                             bool noCopy, bool debug) {


            if (userBuf == nullptr || userBufLen == nullptr) {
                return BAD_ARG;
            }

            port = port < 1024 ? 7777 : port;

            // Create UDP socket
            int udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

            // Try to increase recv buf size to 25 MB
            socklen_t size = sizeof(int);
            int recvBufBytes = 25000000;
            setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0; // clear it
            getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

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
            bool last, firstRead = true, useRecvfrom = true;
            // Start with sequence 0 in very first packet to be read
            uint32_t sequence = 0;
            uint32_t bytesPerPacket = 0, packetCount = 0;
            uint64_t tick = 0;

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
                                             debug, firstRead, &last, useRecvfrom, &tick, &sequence,
                                             &bytesPerPacket, &packetCount, outOfOrderPackets);
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
                                                 debug, firstRead, &last, useRecvfrom, &tick, &sequence,
                                                 &bytesPerPacket, &packetCount, outOfOrderPackets);
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


#endif // EJFAT_ASSEMBLE_ERSAP_H
