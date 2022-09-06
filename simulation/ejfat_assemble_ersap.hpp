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
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <time.h>
#include <cerrno>
#include <map>
#include <cmath>
#include <memory>
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


    namespace ejfat {

        enum errorCodes {
            RECV_MSG = -1,
            TRUNCATED_MSG = -2,
            BUF_TOO_SMALL = -3,
            OUT_OF_ORDER = -4,
            BAD_FIRST_LAST_BIT = -5,
            OUT_OF_MEM = -6,
            BAD_ARG = -7,
            INTERNAL_ERROR = -8
        };


        /**
         * Structure able to hold stats of packet-related quantities for receiving.
         * The contained info relates to the reading/reassembly of a complete buffer.
         */
        typedef struct packetRecvStats_t {
            volatile int64_t  endTime;         /**< Convenience variable to hold start time in microsec from clock_gettime. */
            volatile int64_t  startTime;       /**< Convenience variable to hold end time in microsec from clock_gettime. */
            volatile int64_t  readTime;        /**< Number of microsec taken to read (all packets forming) one complete buffer. */
            volatile uint64_t droppedPackets;  /**< Number of dropped packets. */
            volatile uint64_t acceptedPackets; /**< Number of packets successfully read. */
            volatile uint64_t acceptedBytes;   /**< Number of bytes successfully read, NOT including RE header. */
            volatile uint32_t droppedTicks;    /**< Number of ticks dropped. */
            volatile uint32_t builtBuffers;    /**< Number of buffers fully built from packets. */
            volatile uint32_t combinedBuffers;    /**< Number of buffers fully built from packets. */

            volatile int cpuPkt;               /**< CPU that thread to read pkts is running on. */
            volatile int cpuBuf;               /**< CPU that thread to read build buffers is running on. */
        } packetRecvStats;


        /**
         * Clear packetRecvStats structure.
         * @param stats pointer to structure to be cleared.
         */
        static void clearStats(packetRecvStats *stats) {
            stats->endTime = 0;
            stats->startTime = 0;
            stats->readTime = 0;
            stats->droppedPackets = 0;
            stats->acceptedPackets = 0;
            stats->acceptedBytes = 0;
            stats->droppedTicks = 0;
            stats->builtBuffers = 0;
            stats->combinedBuffers = 0;

            stats->cpuPkt = -1;
            stats->cpuBuf = -1;
        }

        /**
         * Clear packetRecvStats structure.
         * @param stats shared pointer to structure to be cleared.
         */
        static void clearStats(std::shared_ptr<packetRecvStats> stats) {
            stats->endTime = 0;
            stats->startTime = 0;
            stats->readTime = 0;
            stats->droppedPackets = 0;
            stats->acceptedPackets = 0;
            stats->acceptedBytes = 0;
            stats->droppedTicks = 0;
            stats->builtBuffers = 0;
            stats->combinedBuffers = 0;

            stats->cpuPkt = -1;
            stats->cpuBuf = -1;
        }


        /**
         * Print some of the given packetRecvStats structure.
         * @param stats shared pointer to structure to be printed.
         */
        static void printStats(std::shared_ptr<packetRecvStats> stats, std::string prefix) {
            if (!prefix.empty()) {
                fprintf(stderr, "%s: ", prefix.c_str());
            }
            fprintf(stderr,  "bytes = %llu, pkts = %llu, dropped pkts = %llu, dropped ticks = %u\n",
                         stats->acceptedBytes, stats->acceptedPackets, stats->droppedPackets, stats->droppedTicks);
         }


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
         * This routine will, most likely, never be used as this header is
         * stripped off and parsed in the load balancer and the user never
         * sees it.
         *
         * <pre>
         *  protocol 'L:8,B:8,Version:8,Protocol:8,Reserved:16,Entropy:16,Tick:64'
         *
         *  0                   1                   2                   3
         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |       L       |       B       |    Version    |    Protocol   |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  3               4                   5                   6
         *  2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |              Rsvd             |            Entropy            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  6                                               12
         *  4 5       ...           ...         ...         0 1 2 3 4 5 6 7
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                                                               |
         *  +                              Tick                             +
         *  |                                                               |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         * </pre>
         *
         * @param buffer   buffer to parse.
         * @param ll       return 1st byte as char.
         * @param bb       return 2nd byte as char.
         * @param version  return 3rd byte as integer version.
         * @param protocol return 4th byte as integer protocol.
         * @param entropy  return 2 bytes as 16 bit integer entropy.
         * @param tick     return last 8 bytes as 64 bit integer tick.
         */
        static void parseLbHeader(char* buffer, char* ll, char* bb,
                                  uint32_t* version, uint32_t* protocol,
                                  uint32_t* entropy, uint64_t* tick)
        {
            *ll = buffer[0];
            *bb = buffer[1];
            *version  = ((uint32_t)buffer[2] & 0xff);
            *protocol = ((uint32_t)buffer[3] & 0xff);
            *entropy  = ntohs(*((uint16_t *)(&buffer[6]))) & 0xffff;
            *tick     = ntohll(*((uint64_t *)(&buffer[8])));
        }


        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in pointer args.
         *
         * <pre>
         *  protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
         *
         *  0                   1                   2                   3
         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |Version|        Rsvd       |F|L|            Data-ID            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                  UDP Packet Offset                            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                                                               |
         *  +                              Tick                             +
         *  |                                                               |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         * </pre>
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
            // Now pull out the component values
            *version = (buffer[0] & 0xf0) >> 4;
            *first   = (buffer[1] & 0x02) >> 1;
            *last    =  buffer[1] & 0x01;

            *dataId   = ntohs(*((uint16_t *) (buffer + 2)));
            *sequence = ntohl(*((uint32_t *) (buffer + 4)));
            *tick     = ntohll(*((uint64_t *) (buffer + 8)));
        }


        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in pointer arg and array.
         *
         * <pre>
         *  protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
         *
         *  0                   1                   2                   3
         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |Version|        Rsvd       |F|L|            Data-ID            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                  UDP Packet Offset                            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                                                               |
         *  +                              Tick                             +
         *  |                                                               |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         * </pre>
         *
         * @param buffer    buffer to parse.
         * @param intArray  array of ints in which version, first, last, dataId
         *                  and sequence are returned, in that order.
         * @param arraySize number of elements in intArray
         * @param tick      returned tick value, also in LB meta data.
         */
        static void parseReHeader(char* buffer, uint32_t* intArray, int arraySize, uint64_t *tick)
        {
            if (intArray != nullptr && arraySize > 4) {
                intArray[0] = (buffer[0] & 0xf0) >> 4; // version
                intArray[1] = (buffer[1] & 0x02) >> 1; // first
                intArray[2] =  buffer[1] & 0x01;       // last

                intArray[3] = ntohs(*((uint16_t *) (buffer + 2))); // data ID
                intArray[4] = ntohl(*((uint32_t *) (buffer + 4))); // sequence
            }
            *tick = ntohll(*((uint64_t *) (buffer + 8)));
        }

        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in array. Used in packetBlasteeFast to
         * return only needed data.
         *
         * @param buffer    buffer to parse.
         * @param intArray  array of ints in which version, first, last,
         *                  sequence, and tick are returned.
         * @param index     where in intArray to start writing.
         * @param tick      returned tick value.
         */
        static void parseReHeaderFast(char* buffer, uint32_t* intArray, int index, uint64_t *tick)
        {
            intArray[index]     = (buffer[1] & 0x02) >> 1; // first
            intArray[index + 1] =  buffer[1] & 0x01;       // last
            intArray[index + 2] = ntohl(*((uint32_t *) (buffer + 4))); // sequence

            *tick = ntohll(*((uint64_t *) (buffer + 8)));

            // store tick for later use in big endian form
            *((uint64_t *) (&(intArray[index + 3]))) = *tick;
        }


        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in pointer args.
         *
         * <pre>
         *  protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
         *
         *  0                   1                   2                   3
         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |Version|        Rsvd       |F|L|            Data-ID            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                  UDP Packet Offset                            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                                                               |
         *  +                              Tick                             +
         *  |                                                               |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         * </pre>
         *
         * @param buffer   buffer to parse.
         * @param sequence returned packet sequence number.
         * @param tick     returned tick value, also in LB meta data.
         */
        static void parseReHeader(char* buffer, uint32_t* sequence, uint64_t *tick)
        {
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
         * The reassembly header will be parsed and its data retrieved.
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
         * Routine to read a single UDP packet into a single buffer.
         * The reassembly header will be parsed and its data retrieved.
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
            int dataBytes = bytesRead - HEADER_BYTES;
            memcpy(dataBuf, pkt + HEADER_BYTES, dataBytes);

            return dataBytes;
        }


        static void clearMap(std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {
            if (outOfOrderPackets.empty()) return;

            for (const auto& n : outOfOrderPackets) {
                // Free allocated buffer holding packet
                free(std::get<0>(n.second));
            }
            outOfOrderPackets.clear();
        }



        /**
         * <p>
         * Assemble incoming packets into the given buffer.
         * It will read entire buffer or return an error.
         * This routine allows for out-of-order packets.
         * </p>
         *
         * <p>
         * If the given tick value is <b>NOT</b> 0xffffffffffffffff, then it is the next expected tick.
         * And in this case, this method makes a number of assumptions:
         * <ul>
         * <li>Each incoming buffer/tick is split up into the same # of packets.</li>
         * <li>Each successive tick differs by tickPrescale.</li>
         * <li>If the sequence changes by 2, then a dropped packet is assumed.
         *     This results from the observation that for a simple network,
         *     there are never out-of-order packets.</li>
         * </ul>
         * </p>
         * <p>
         * This routine uses recvfrom to read in packets, but minimizes the copying of data
         * by copying as much data as possible, directly to dataBuf. This involves storing
         * what temporarily gets overwritten by a RE header and then restoring it once the
         * read of a packet is complete.
         *</p>
         *
         * <p>
         * A note on statistics. The raw counts are <b>ADDED</b> to what's already
         * in the stats structure. It's up to the user to clear stats before calling
         * this method if desired.
         * </p>
         *
         * @param dataBuf           place to store assembled packets.
         * @param bufLen            byte length of dataBuf.
         * @param udpSocket         UDP socket to read.
         * @param debug             turn debug printout on & off.
         * @param tick              value-result parameter which gives the next expected tick
         *                          and returns the tick that was built. If it's passed in as
         *                          0xffff ffff ffff ffff, then ticks are coming in no particular order.
         * @param dataId            to be filled with data ID from RE header.
         * @param stats             to be filled packet statistics.
         * @param tickPrescale      add to current tick to get next expected tick.
         * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
         *
         * @return total bytes read.
         *         If there's an error in recvfrom, it will return RECV_MSG.
         *         If the buffer is too small to receive a single tick's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         */
        static ssize_t getCompletePacketizedBuffer(char* dataBuf, size_t bufLen, int udpSocket,
                                                   bool debug, uint64_t *tick, uint16_t *dataId,
                                                   std::shared_ptr<packetRecvStats> stats, uint32_t tickPrescale,
                                                   std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {

            int64_t  prevTick = -1, firstTick = -1;
            uint64_t expectedTick = *tick;
            uint64_t packetTick;
            uint32_t sequence, prevSequence = 0, expectedSequence = 0;

            bool packetFirst, packetLast;
            bool dumpTick = false;
            bool firstReadForBuf = false;
            bool takeStats = stats != nullptr;
            bool veryFirstRead = true;

            bool knowExpectedTick = expectedTick != 0xffffffffffffffffL;

            int  version, nBytes, bytesRead;
            uint16_t packetDataId;
            size_t  maxPacketBytes = 0;
            ssize_t totalBytesRead = 0;

            char headerStorage[HEADER_BYTES];

            char *writeHeaderAt, *putDataAt = dataBuf;
            size_t remainingLen = bufLen;
            struct timespec now;


            if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu, take stats = %d, %p\n",
                               remainingLen, takeStats, stats.get());

            while (true) {

                // Another packet of data will exceed buffer space, so quit
                if (remainingLen < 1) {
                    return BUF_TOO_SMALL;
                }

                if (veryFirstRead) {
                    // Read in one packet, return value does NOT include RE header
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &sequence, &packetDataId, &version,
                                                &packetFirst, &packetLast, debug);
                    // If error
                    if (nBytes < 0) {
                        clearMap(outOfOrderPackets);
                        return nBytes;
                    }

//                    if (takeStats) {
//                        clock_gettime(CLOCK_MONOTONIC, &now);
//                        stats->startTime = 1000000L * now.tv_sec + now.tv_nsec/1000L; // microseconds
//                    }

                    veryFirstRead = false;
                }
                else {
                    writeHeaderAt = putDataAt - HEADER_BYTES;
                    // Copy part of buffer that we'll temporarily overwrite
                    memcpy(headerStorage, writeHeaderAt, HEADER_BYTES);

                    // Read data right into final buffer (including RE header)
                    bytesRead = recvfrom(udpSocket, writeHeaderAt, remainingLen, 0, NULL, NULL);
                    if (bytesRead < 0) {
                        fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));
                        clearMap(outOfOrderPackets);
                        return(RECV_MSG);
                    }
                    nBytes = bytesRead - HEADER_BYTES;

                    // Parse header
                    parseReHeader(writeHeaderAt, &version, &packetFirst, &packetLast, &packetDataId, &sequence, &packetTick);

//                    if (takeStats && packetLast) {
//                        // This may or may not be the actual last packet.
//                        // (A whole buffer may have been dropped after last received packet.)
//                        // So, for now, just record time in interest of getting a good time value.
//                        // This may be overwritten later if it turns out we had some dropped packets.
//                        clock_gettime(CLOCK_MONOTONIC, &now);
//                        stats->endTime = 1000000L * now.tv_sec + now.tv_nsec/1000L;
//                    }

                    // Replace what was written over
                    memcpy(writeHeaderAt, headerStorage, HEADER_BYTES);
                }

                // This if-else statement is what enables the packet reading/parsing to keep
                // up an input rate that is too high (causing dropped packets) and still salvage
                // some of what is coming in.
                if (packetTick != prevTick) {
                    // If we're here, either we've just read the very first legitimate packet,
                    // or we've dropped some packets and advanced to another tick in the process.

                    expectedSequence = 0;

                    if (sequence != 0) {
                        // Already have trouble, looks like we dropped the first packet of a tick,
                        // and possibly others after it.
                        // So go ahead and dump the rest of the tick in an effort to keep up.
                        //printf("Skip %hu, %llu - %u\n", packetDataId, packetTick, sequence);
                        //printf("S %llu - %u\n", packetTick, sequence);
                        putDataAt = dataBuf;
                        remainingLen = bufLen;
                        veryFirstRead = true;
                        continue;
                    }

                    // If here, new tick/buffer, sequence = 0.
                    // There's a chance we can construct a full buffer.

                    // Dump everything we saved from previous tick.
                    // Delete all out-of-seq packets.
                    clearMap(outOfOrderPackets);
                    dumpTick = false;
                }
                // Same tick as last packet
                else {
                    if (dumpTick || (std::abs((int)(sequence - prevSequence)) > 1)) {
                        // If here, the sequence hopped by at least 2,
                        // probably dropped at least 1,
                        // so drop rest of packets for record.
                        // This branch of the "if" will no longer
                        // be executed once the next record shows up.
                        putDataAt = dataBuf;
                        remainingLen = bufLen;
                        veryFirstRead = true;
                        expectedSequence = 0;
                        dumpTick = true;
                        prevSequence = sequence;
                        //printf("Dump %hu, %llu - %u\n", packetDataId, packetTick, sequence);
                        //printf("D %llu - %u\n", packetTick, sequence);
                        continue;
                    }
                }

                // TODO: What if we get a zero-length packet???

                if (sequence == 0) {
                    firstReadForBuf = true;
                }

                prevTick = packetTick;
                prevSequence = sequence;

if (debug) fprintf(stderr, "Received %d data bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
                                   nBytes, sequence, btoa(packetLast), btoa(firstReadForBuf));

                // Check to see if packet is out-of-sequence
                if (sequence != expectedSequence) {
                    if (debug) fprintf(stderr, "\n    Got seq %u, expecting %u\n", sequence, expectedSequence);

                    // If we get one that we already received, ERROR!
                    if (sequence < expectedSequence) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Already got seq %u, id %hu, t %llu\n", sequence, packetDataId, packetTick);
                        return OUT_OF_ORDER;
                    }

                    // Set a limit on how much we're going to store (200 packets) while we wait
                    if (outOfOrderPackets.size() >= 200 || sizeof(outOfOrderPackets) >= outOfOrderPackets.max_size() ) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Reached size limit of stored packets!\n");
                        return OUT_OF_ORDER;
                    }

                    // Since it's out of order, what was written into dataBuf will need to be
                    // copied and stored. And that written data will eventually need to be
                    // overwritten with the correct packet data.
                    char *tempBuf = (char *) malloc(nBytes);
                    if (tempBuf == nullptr) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Ran out of memory storing packets!\n");
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

                    // If it's the first read of a sequence, and there are more reads to come,
                    // the # of bytes it read will be max possible. Remember that.
                    if (firstReadForBuf) {
                        maxPacketBytes = nBytes;
                        firstReadForBuf = false;
                        //maxPacketsInBuf = bufLen / maxPacketBytes;
                        if (debug) fprintf(stderr, "In first read, max bytes/packet = %lu\n", maxPacketBytes);

                        // Error check
                        if (!packetFirst) {
                            fprintf(stderr, "Expecting first bit to be set on very first read but wasn't\n");
                            clearMap(outOfOrderPackets);
                            return BAD_FIRST_LAST_BIT;
                        }
                    }
                    else if (packetFirst) {
                        fprintf(stderr, "Expecting first bit NOT to be set on read but was\n");
                        clearMap(outOfOrderPackets);
                        return BAD_FIRST_LAST_BIT;
                    }

                    if (debug) fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s, OUTofOrder = %lu\n",
                                       remainingLen, expectedSequence, btoa(packetFirst), btoa(packetLast),
                                       outOfOrderPackets.size());

                    // If no stored, out-of-order packets ...
                    if (outOfOrderPackets.empty()) {
                        // If very last packet, quit
                        if (packetLast) {
                            // Finish up some stats
                            if (takeStats) {
                                uint32_t droppedTicks = 0;
                                if (knowExpectedTick) {
                                    int64_t diff = packetTick - expectedTick;
                                    if (diff % tickPrescale != 0) {
                                        // Error in the way we set things up
                                        // This should always be 0.
                                        clearMap(outOfOrderPackets);
                                        fprintf(stderr, "    Using wrong value for tick prescale, %u\n", tickPrescale);
                                        return INTERNAL_ERROR;
                                    }
                                    else {
                                        droppedTicks = diff / tickPrescale;
                                        //printf("Dropped %u, dif %lld, t %llu x %llu \n", stats->droppedTicks, diff, packetTick, expectedTick);
                                    }
                                }

                                // Total microsec to read buffer
//                                stats->readTime += stats->endTime - stats->startTime;
                                stats->acceptedBytes += totalBytesRead;
                                stats->acceptedPackets += sequence + 1;
//fprintf(stderr, "        accepted pkts = %llu, seq = %u\n", stats->acceptedPackets, sequence);
                                stats->droppedTicks   += droppedTicks;
                                stats->droppedPackets += droppedTicks * (sequence + 1);
                            }
                            break;
                        }
if (remainingLen < 1) fprintf(stderr, "        remaining len = %llu\n", remainingLen);
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

                if (packetLast) {
                    break;
                }
            }

            *tick   = packetTick;
            *dataId = packetDataId;
            clearMap(outOfOrderPackets);
            return totalBytesRead;
        }



        /**
         * <p>
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets. It also allows for multiple calls
         * to read the buffer in stages.</p>
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

            int64_t prevTick = -1;
            uint64_t packetTick;
            uint32_t sequence, prevSequence = 0, expectedSequence = *expSequence;

            bool dumpTick;
            bool packetFirst, packetLast, firstReadForBuf = false, tooLittleRoom = false;
     //       bool takeStats = stats != nullptr;
            int  version, nBytes, bytesRead;
            uint16_t dataId;
            uint32_t pktCount = 0;
            size_t  maxPacketBytes = 0;
            ssize_t totalBytesRead = 0;

            char headerStorage[HEADER_BYTES];

            char *writeHeaderAt, *putDataAt = dataBuf;
            size_t remainingLen = bufLen;

            // Track stats
            int64_t totalT = 0, time, time1, time2;
            struct timespec t1, t2;


            if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu\n", remainingLen);

            while (true) {

                dumpTick = false;

                if (veryFirstRead) {
                    // Read in one packet, return value does NOT include RE header
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &sequence, &dataId, &version,
                                                &packetFirst, &packetLast, debug);

//                    if (takeStats) {
//                        clock_gettime(CLOCK_MONOTONIC, &t1);
//                        stats->startTime = 1000000000L*t1.tv_sec + t1.tv_nsec; // nanoseconds
//                    }

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
                    bytesRead = recvfrom(udpSocket, writeHeaderAt, remainingLen, 0, NULL, NULL);
                    if (bytesRead < 0) {
                        fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));
                        return(RECV_MSG);
                    }
                    nBytes = bytesRead - HEADER_BYTES;

                    // Parse header
                    parseReHeader(writeHeaderAt, &version, &packetFirst, &packetLast, &dataId, &sequence, &packetTick);

                    // Replace what was written over
                    memcpy(writeHeaderAt, headerStorage, HEADER_BYTES);
                }

                // This if-else statement is what enables the packet reading/parsing to keep
                // up an input rate that is too high (causing dropped packets) and still salvage
                // much of what is coming in.
                if (packetTick != prevTick) {
                    expectedSequence = 0;
                    veryFirstRead = true;

                    if (sequence != 0) {
                        // Already have trouble, looks like we dropped the first packet of a tick.
                        // So go ahead and dump the rest of the tick in an effort to keep up.
                        //printf("Skip id %hu, t %llu, s %u\n", dataId, packetTick, sequence);
                        continue;
                    }

                    // If here, new record, seq = 0

                    // Dump everything we saved from previous tick.
                    // Delete all out-of-seq packets.
                    outOfOrderPackets.clear();
                    dumpTick = false;
                }
                    // Same tick as last packet
                else {
                    if (dumpTick || (std::abs((int)(sequence - prevSequence)) > 1)) {
                        // If here, the sequence hopped by at least 2,
                        // probably dropped at least 1,
                        // so drop rest of packets for record.
                        // This branch of the "if" will no longer
                        // be executed once the next record shows up.
                        veryFirstRead = true;
                        expectedSequence = 0;
                        dumpTick = true;
                        prevSequence = sequence;
                        //printf("Dump id %hu, t %llu, s %u\n", dataId, packetTick, sequence);
                        continue;
                    }
                }

                // TODO: What if we get a zero-length packet???

                if (sequence == 0) {
                    firstReadForBuf = true;
                    *bytesPerPacket = nBytes;
                }

                prevTick = packetTick;
                prevSequence = sequence;


                if (debug) fprintf(stderr, "Received %d data bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
                                   nBytes, sequence, btoa(packetLast), btoa(firstReadForBuf));

                // Check to see if packet is out-of-sequence
                if (sequence != expectedSequence) {
                    if (debug) fprintf(stderr, "\n    Got seq %u, expecting %u\n", sequence, expectedSequence);

                    // If we get one that we already received, ERROR!
                    if (sequence < expectedSequence) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Already got seq %u, id %hu, t %llu\n", sequence, dataId, packetTick);
                        return OUT_OF_ORDER;
                    }

                    // Set a limit on how much we're going to store (1000 packets) while we wait
                    if (outOfOrderPackets.size() >= 1000 || sizeof(outOfOrderPackets) >= outOfOrderPackets.max_size() ) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Reached size limit of stored packets!\n");
                        return OUT_OF_ORDER;
                    }

                    // Since it's out of order, what was written into dataBuf will need to be
                    // copied and stored. And that written data will eventually need to be
                    // overwritten with the correct packet data.
                    char *tempBuf = (char *) malloc(nBytes);
                    if (tempBuf == nullptr) {
                        clearMap(outOfOrderPackets);
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
                            fprintf(stderr, "Expecting first bit to be set on very first read but wasn't\n");
                            clearMap(outOfOrderPackets);
                            return BAD_FIRST_LAST_BIT;
                        }
                    }
                    else if (packetFirst) {
                        fprintf(stderr, "Expecting first bit NOT to be set on read but was\n");
                        clearMap(outOfOrderPackets);
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
         * This routine allows for out-of-order packets. It also allows for multiple calls
         * to read the buffer in stages.
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
                    *bytesPerPacket = nBytes;
                }

                if (debug) fprintf(stderr, "Received %d bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
                                   nBytes, sequence, btoa(packetLast), btoa(firstReadForBuf));

                // Check to see if packet is in sequence
                if (sequence != expectedSequence) {
                    if (debug) fprintf(stderr, "\n    Got seq %u, expecting %u\n", sequence, expectedSequence);

                    // If we get one that we already received, ERROR!
                    if (sequence < expectedSequence) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Already got seq %u once before!\n", sequence);
                        return OUT_OF_ORDER;
                    }

                    // Set a limit on how much we're going to store (1000 packets) while we wait
                    if (outOfOrderPackets.size() >= 1000 || sizeof(outOfOrderPackets) >= outOfOrderPackets.max_size() ) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "    Reached size limit of stored packets!\n");
                        return OUT_OF_ORDER;
                    }

                    // Since it's out of order, what was written into dataBuf will need to be
                    // copied and stored. And that written data will eventually need to be
                    // overwritten with the correct packet data.
                    char *tempBuf = (char *) malloc(nBytes);
                    if (tempBuf == nullptr) {
                        clearMap(outOfOrderPackets);
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
                            fprintf(stderr, "Expecting first bit to be set on very first read but wasn't\n");
                            clearMap(outOfOrderPackets);
                            return BAD_FIRST_LAST_BIT;
                        }
                    }
                    else if (packetFirst) {
                        fprintf(stderr, "Expecting first bit NOT to be set on read but was\n");
                        clearMap(outOfOrderPackets);
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
                             bool noCopy, bool debug, bool useIPv6) {


            if (userBuf == nullptr || userBufLen == nullptr) {
                return BAD_ARG;
            }

            port = port < 1024 ? 7777 : port;
            int err, udpSocket;

            if (useIPv6) {

                struct sockaddr_in6 serverAddr6{};

                // Create IPv6 UDP socket
                if ((udpSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                    perror("creating IPv6 client socket");
                    return -1;
                }

                // Try to increase recv buf size to 25 MB
                socklen_t size = sizeof(int);
                int recvBufBytes = 25000000;
                setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
                recvBufBytes = 0; // clear it
                getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
                if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

                // Configure settings in address struct
                // Clear it out
                memset(&serverAddr6, 0, sizeof(serverAddr6));
                // it is an INET address
                serverAddr6.sin6_family = AF_INET6;
                // the port we are going to receiver from, in network byte order
                serverAddr6.sin6_port = htons(port);
                if (listeningAddr != nullptr && strlen(listeningAddr) > 0) {
                    inet_pton(AF_INET6, listeningAddr, &serverAddr6.sin6_addr);
                }
                else {
                    serverAddr6.sin6_addr = in6addr_any;
                }

                // Bind socket with address struct
                err = bind(udpSocket, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
                if (err != 0) {
                    // TODO: handle error properly
                    if (debug) fprintf(stderr, "bind socket error\n");
                }

            } else {

                struct sockaddr_in serverAddr{};

                // Create UDP socket
                if ((udpSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                    perror("creating IPv4 client socket");
                    return -1;
                }

                // Try to increase recv buf size to 25 MB
                socklen_t size = sizeof(int);
                int recvBufBytes = 25000000;
                setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
                recvBufBytes = 0; // clear it
                getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
                if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

                // Configure settings in address struct
                memset(&serverAddr, 0, sizeof(serverAddr));
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
                err = bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
                if (err != 0) {
                    // TODO: handle error properly
                    if (debug) fprintf(stderr, "bind socket error\n");
                }

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
                    clearMap(outOfOrderPackets);
                    return totalBytes;
                }

                if (!last) {
                    if (debug) fprintf(stderr, "Buffer full, but no last packet found, likely buffer is too small\n");
                    clearMap(outOfOrderPackets);
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
                        clearMap(outOfOrderPackets);
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
                            clearMap(outOfOrderPackets);
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

            clearMap(outOfOrderPackets);

            if (debug) fprintf(stderr, "Read %ld incoming data bytes\n", totalBytes);

            return 0;
        }

    }


#endif // EJFAT_ASSEMBLE_ERSAP_H
