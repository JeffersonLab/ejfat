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
#include <ctime>
#include <cerrno>
#include <map>
#include <cmath>
#include <memory>
#include <getopt.h>
#include <climits>
#include <cinttypes>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

//#include "PacketsItem.h"

#ifdef __APPLE__
#include <cctype>
#endif

// Reassembly header size in bytes
#define HEADER_BYTES 20

#define btoa(x) ((x)?"true":"false")


#ifdef __linux__
    // for recvmmsg
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


#ifndef EJFAT_BYTESWAP_H
#define EJFAT_BYTESWAP_H

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
            volatile int64_t  endTime;          /**< Convenience variable to hold start time in microsec from clock_gettime. */
            volatile int64_t  startTime;        /**< Convenience variable to hold end time in microsec from clock_gettime. */
            volatile int64_t  readTime;         /**< Number of microsec taken to read (all packets forming) one complete buffer. */
            volatile uint64_t droppedPackets;   /**< Number of dropped packets. */
            volatile uint64_t acceptedPackets;  /**< Number of packets successfully read. */
            volatile uint64_t acceptedBytes;    /**< Number of bytes successfully read, NOT including RE header. */
            volatile uint64_t droppedBytes;     /**< Number of bytes dropped. */
            volatile uint32_t droppedTicks;     /**< Number of ticks dropped. */
            volatile uint32_t builtBuffers;     /**< Number of buffers fully built from packets. */
            volatile uint32_t combinedBuffers;  /**< Number of buffers fully built from packets. */

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
            stats->droppedBytes = 0;
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
            stats->droppedBytes = 0;
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
            fprintf(stderr,  "bytes = %" PRIu64 ", pkts = %" PRIu64 ", dropped bytes = %" PRIu64 ", dropped pkts = %" PRIu64 ", dropped ticks = %u\n",
                         stats->acceptedBytes, stats->acceptedPackets, stats->droppedBytes,
                         stats->droppedPackets, stats->droppedTicks);
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
         *  |              Rsvd             |            Entropy            | channel id
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
        static void parseLbHeader(const char* buffer, char* ll, char* bb,
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
         * The padding values are ignored and only there to circumvent
         * a bug in the ESNet Load Balancer which otherwise filters out
         * packets with data payload of 0 or 1 byte.
         *
         * <pre>
         *  protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32 Padding1:8, Padding2:8'
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
         *  *   Padding 1   |   Padding 2   |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
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
        static void parseReHeaderOld(char* buffer, int* version,
                                  bool *first, bool *last,
                                  uint16_t* dataId, uint32_t* sequence,
                                  uint64_t *tick)
        {
            // Now pull out the component values
            *version = (buffer[0] >> 4) & 0xf;
            *first   = (buffer[1] & 0x02) >> 1;
            *last    =  buffer[1] & 0x01;

            *dataId   = ntohs(*((uint16_t *) (buffer + 2)));
            *sequence = ntohl(*((uint32_t *) (buffer + 4)));
            *tick     = ntohll(*((uint64_t *) (buffer + 8)));
        }


        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in pointer args.
         *
         * <pre>
         *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
         *
         *  0                   1                   2                   3
         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |Version|        Rsvd           |            Data-ID            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                         Buffer Offset                         |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                         Buffer Length                         |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                                                               |
         *  +                             Tick                              +
         *  |                                                               |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         * </pre>
         *
         * @param buffer   buffer to parse.
         * @param version  returned version.
         * @param dataId   returned data source id.
         * @param offset   returned byte offset into buffer of this data payload.
         * @param length   returned total buffer length in bytes of which this packet is a port.
         * @param tick     returned tick value, also in LB meta data.
         */
        static void parseReHeader(const char* buffer, int* version, uint16_t* dataId,
                                  uint32_t* offset, uint32_t* length, uint64_t *tick)
        {
            // Now pull out the component values
            *version = (buffer[0] >> 4) & 0xf;
            *dataId  = ntohs(*((uint16_t *)  (buffer + 2)));
            *offset  = ntohl(*((uint32_t *)  (buffer + 4)));
            *length  = ntohl(*((uint32_t *)  (buffer + 8)));
            *tick    = ntohll(*((uint64_t *) (buffer + 12)));
        }


//        /**
//         * Parse the reassembly header at the start of the given buffer.
//         * Return parsed values in pointer arg.
//         *
//         * <pre>
//         *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
//         *
//         *  0                   1                   2                   3
//         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//         *  |Version|        Rsvd           |            Data-ID            |
//         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//         *  |                         Buffer Offset                         |
//         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//         *  |                         Buffer Length                         |
//         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//         *  |                                                               |
//         *  +                             Tick                              +
//         *  |                                                               |
//         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//         * </pre>
//         *
//         * @param buffer   buffer to parse.
//         * @param header   pointer to struct to be filled with RE header info.
//         */
//        static void parseReHeader(const char* buffer, ejfat::PacketsItem::reHeader* header)
//        {
//            // Now pull out the component values
//            if (header != nullptr) {
//                header->version =                       (buffer[0] >> 4) & 0xf;
//                header->dataId  = ntohs(*((uint16_t *)  (buffer + 2)));
//                header->offset  = ntohl(*((uint32_t *)  (buffer + 4)));
//                header->length  = ntohl(*((uint32_t *)  (buffer + 8)));
//                header->tick    = ntohll(*((uint64_t *) (buffer + 12)));
//            }
//        }


        /**
        * Parse the reassembly header at the start of the given buffer.
        * Return parsed values in pointer args.
        *
        * <pre>
        *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
        *
        *  0                   1                   2                   3
        *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        *  |Version|        Rsvd           |            Data-ID            |
        *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        *  |                         Buffer Offset                         |
        *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        *  |                         Buffer Length                         |
        *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        *  |                                                               |
        *  +                             Tick                              +
        *  |                                                               |
        *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        * </pre>
        *
        * @param buffer buffer to parse.
        * @param offset returned byte offset into buffer of this data payload.
        * @param length returned total buffer length in bytes of which this packet is a port.
        * @param tick   returned tick value, also in LB meta data.
        */
        static void parseReHeader(const char* buffer, uint32_t* offset, uint32_t *length, uint64_t *tick)
        {
            *offset = ntohl(*((uint32_t *)  (buffer + 4)));
            *length = ntohl(*((uint32_t *)  (buffer + 8)));
            *tick   = ntohll(*((uint64_t *) (buffer + 12)));
        }


        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in pointer arg and array.
         *
         * <pre>
         *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
         *
         *  0                   1                   2                   3
         *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |Version|        Rsvd           |            Data-ID            |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                         Buffer Offset                         |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                         Buffer Length                         |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         *  |                                                               |
         *  +                             Tick                              +
         *  |                                                               |
         *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
         * </pre>
         *
         * @param buffer    buffer to parse.
         * @param intArray  array of ints in which version, dataId, offset,
         *                  and length are returned, in that order.
         * @param arraySize number of elements in intArray
         * @param tick      returned tick value, also in LB meta data.
         */
        static void parseReHeader(const char* buffer, uint32_t* intArray, int arraySize, uint64_t *tick)
        {
            if (intArray != nullptr && arraySize > 4) {
                intArray[0] = (buffer[0] >> 4) & 0xf;  // version
                intArray[1] = ntohs(*((uint16_t *) (buffer + 2))); // data ID
                intArray[2] = ntohl(*((uint32_t *) (buffer + 4))); // offset
                intArray[3] = ntohl(*((uint32_t *) (buffer + 8))); // length
            }
            *tick = ntohll(*((uint64_t *) (buffer + 12)));
        }

        /**
         * Parse the reassembly header at the start of the given buffer.
         * Return parsed values in array. Used in packetBlasteeFast to
         * return only needed data.
         *
         * @param buffer    buffer to parse.
         * @param intArray  array of ints in which offset, length, and tick are returned.
         * @param index     where in intArray to start writing.
         * @param tick      returned tick value.
         */
        static void parseReHeaderFast(const char* buffer, uint32_t* intArray, int index, uint64_t *tick)
        {
            intArray[index]     = ntohl(*((uint32_t *) (buffer + 4))); // offset
            intArray[index + 1] = ntohl(*((uint32_t *) (buffer + 8))); // length

            *tick = ntohll(*((uint64_t *) (buffer + 12)));
            // store tick for later
            *((uint64_t *) (&(intArray[index + 2]))) = *tick;
        }


        //-----------------------------------------------------------------------
        // Be sure to print to stderr as programs may pipe data to stdout!!!
        //-----------------------------------------------------------------------


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
         * @param length    to be filled with buffer length from RE header.
         * @param offset    to be filled with buffer offset from RE header.
         * @param dataId    to be filled with data id read RE header.
         * @param version   to be filled with version read RE header.
         * @param last      to be filled with "last" bit id from RE header,
         *                  indicating the last packet in a series used to send data.
         * @param debug     turn debug printout on & off.
         *
         * @return number of data (not headers!) bytes read from packet.
         *         If there's an error in recvfrom, it will return RECV_MSG.
         *         If there is not enough data to contain a header, it will return INTERNAL_ERROR.
         *         If there is not enough room in dataBuf to hold incoming data, it will return BUF_TOO_SMALL.
         */
        static int readPacketRecvFrom(char *dataBuf, size_t bufLen, int udpSocket,
                                      uint64_t *tick, uint32_t *length, uint32_t* offset,
                                      uint16_t* dataId, int* version, bool debug) {

            // Storage for packet
            char pkt[9100];

            int bytesRead = recvfrom(udpSocket, pkt, 9100, 0, nullptr, nullptr);
            if (bytesRead < 0) {
                if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));
                return(RECV_MSG);
            }
            else if (bytesRead < HEADER_BYTES) {
                fprintf(stderr, "recvfrom(): not enough data to contain a header on read\n");
                return(INTERNAL_ERROR);
            }

            if (bufLen < bytesRead) {
                return(BUF_TOO_SMALL);
            }

            // Parse header
            parseReHeader(pkt, version, dataId, offset, length, tick);

            // Copy datq
            int dataBytes = bytesRead - HEADER_BYTES;
            memcpy(dataBuf, pkt + HEADER_BYTES, dataBytes);

            return dataBytes;
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
         *         If there is not enough data to contain a header, it will return INTERNAL_ERROR.
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
            else if (bytesRead < HEADER_BYTES) {
                fprintf(stderr, "recvfrom(): not enough data to contain a header on read\n");
                return(INTERNAL_ERROR);
            }

            if (bufLen < bytesRead) {
                return(BUF_TOO_SMALL);
            }

            // Parse header
            parseReHeaderOld(pkt, version, first, last, dataId, sequence, tick);

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
          * This routine does NOT allow for out-of-order packets.
          * </p>
          *
          * <p>
          * If the given tick value is <b>NOT</b> 0xffffffffffffffff, then it is the next expected tick.
          * And in this case, this method makes a number of assumptions:
          * <ul>
          * <li>Each successive tick differs by tickPrescale.</li>
          * <li>If the offset is &gt; the total received data, then a packet or packets have been dropped.
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
          * @param dataId            to be filled with data ID from RE header (can be nullptr).
          * @param stats             to be filled packet statistics.
          * @param tickPrescale      add to current tick to get next expected tick.
          * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
          *
          * @return total data bytes read (does not include RE header).
          *         If there's an error in recvfrom, it will return RECV_MSG.
          *         If the buffer is too small to receive a single tick's data, it will return BUF_TOO_SMALL.
          *         If a packet is out of order and no recovery is possible (e.g. duplicate offset),
          *              it will return OUT_OF_ORDER.
          *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
          *         If cannot allocate memory, it will return OUT_OF_MEM.
          *         If on a read no data is returned when buffer not filled, return INTERNAL_ERROR.
          *         If on a read &lt; HEADER_BYTES data returned, not enough data to contain header.
          *              Then some sort of internal error and will return INTERNAL_ERROR.
          */
        static ssize_t getCompletePacketizedBuffer(char* dataBuf, size_t bufLen, int udpSocket,
                                                   bool debug, uint64_t *tick, uint16_t *dataId,
                                                   std::shared_ptr<packetRecvStats> stats, uint32_t tickPrescale,
                                                   std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {

            uint64_t prevTick = UINT_MAX;
            uint64_t expectedTick = *tick;
            uint64_t packetTick;

            uint32_t offset, prevOffset = 0, expectedOffset = 0;
            int64_t offsetLong = 0, prevOffsetLong = 0, offsetDiff = 0;

            uint32_t length, prevLength = 0;
            int64_t lengthLong = 0, prevLengthLong = 0, lengthDiff = 0;

            bool packetFirst, packetLast, prevPacketLast = true;
            bool dumpTick = false;
            bool firstReadForBuf = false;
            bool takeStats = stats != nullptr;
            bool veryFirstRead = true;
            bool outOfSequence = false;

            bool knowExpectedTick = expectedTick != 0xffffffffffffffffL;

            int  version, nBytes;
     //       int  bytesRead;
            ssize_t bytesRead;
            uint16_t packetDataId;
            size_t  maxPacketBytes = 0;
            ssize_t totalBytesRead = 0;

            char headerStorage[HEADER_BYTES];

            char *writeHeaderAt, *putDataAt = dataBuf;
            size_t remainingLen = bufLen;
            struct timespec now {0,0};


            if (debug && takeStats) fprintf(stderr, "getCompletePacketizedBuffer: remainingLen = %lu, take stats = %d, %p\n",
                               remainingLen, takeStats, stats.get());

            while (true) {

                // Another packet of data will exceed buffer space, so quit
                if (remainingLen <= HEADER_BYTES) {
                    if (takeStats && stats->droppedPackets > 0) {
                        fprintf(stderr, "getCompletePacketizedBuffer: dropping packets?, remaining len <= header\n");
                    }
                    else {
                        fprintf(stderr, "getCompletePacketizedBuffer: buffer too small?, remaining len <= header\n");
                    }
                    return BUF_TOO_SMALL;
                }

                if (veryFirstRead) {

                    maxPacketBytes = 0;
                    totalBytesRead = 0;
                    expectedOffset = 0;
                    putDataAt      = dataBuf;
                    remainingLen   = bufLen;
                    outOfSequence  = false;

                    // Read in one packet, return value does NOT include RE header
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &length, &offset,
                                                &packetDataId, &version, debug);

                    // If error
                    if (nBytes < 0) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "getCompletePacketizedBuffer: on first read, buf too small? nBytes = %d, remainingLen = %zu\n", nBytes, remainingLen);
                        return nBytes;
                    }
                    else if (nBytes == 0) {
                        // Something clearly wrong. There should be SOME data returned.
                        fprintf(stderr, "getCompletePacketizedBuffer: on first read, buf too small? nBytes = 0, remainingLen = %zu\n", remainingLen);
                        clearMap(outOfOrderPackets);
                        return INTERNAL_ERROR;
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
                        fprintf(stderr, "getCompletePacketizedBuffer: recvfrom failed: %s\n", strerror(errno));
                        clearMap(outOfOrderPackets);
                        return(RECV_MSG);
                    }
                    else if (bytesRead < HEADER_BYTES) {
                        fprintf(stderr, "getCompletePacketizedBuffer: not enough data to contain a header on read\n");
                        clearMap(outOfOrderPackets);
                        return(INTERNAL_ERROR);
                    }

                    nBytes = bytesRead - HEADER_BYTES;

                    if (nBytes == 0) {
                        // Something clearly wrong. There should be SOME data besides header returned.
                        fprintf(stderr, "getCompletePacketizedBuffer: buf too small? nBytes = %d, remainingLen = %zu\n", nBytes, remainingLen);
                        clearMap(outOfOrderPackets);
                        return INTERNAL_ERROR;
                    }

                    // Parse header
                    parseReHeader(writeHeaderAt, &version, &packetDataId, &length, &offset, &packetTick);

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

                // What happens when you subtract 2 unsigned ints? (result modulo UINT_MAX + 1).
                // Avoid problems with unsigned int arithmetic and assign to int64_t.
                offsetLong = ((int64_t)offset & 0xffffffffL);
                lengthLong = ((int64_t)length & 0xffffffffL);
                offsetDiff = offsetLong - totalBytesRead;
                if (offsetDiff != 0) {
                    outOfSequence = true;
                }

                packetLast = false;
                if (bytesRead == length) {
                    packetLast = true;
                }

                packetFirst = false;
                if (offset == 0) {
                    packetFirst = true;
                }

                //                if (packetTick != expectedTick) {
                //                    printf("Packet != expected tick, got %" PRIu64 ", ex = %" PRIu64 ", prev = %" PRIu64 "\n",
                //                           packetTick, expectedTick, prevTick);
                //                }

                // This if-else statement is what enables the packet reading/parsing to keep
                // up an input rate that is too high (causing dropped packets) and still salvage
                // some of what is coming in.
                if (packetTick != prevTick) {
                    // If we're here, either we've just read the very first legitimate packet,
                    // or we've dropped some packets and advanced to another tick in the process.

                    expectedOffset = 0;

                    if (offset != 0) {
                        // Already have trouble, looks like we dropped the first packet of a tick,
                        // and possibly others after it.
                        // So go ahead and dump the rest of the tick in an effort to keep up.
                        if (debug) printf("Skip pkt from id %hu, %" PRIu64 " - %u, expected seq 0\n", packetDataId, packetTick, offset);
                        veryFirstRead = true;
                        dumpTick = true;
                        prevTick = packetTick;
                        prevOffset = offset;
                        prevOffsetLong = offsetLong;
                        prevPacketLast = packetLast;

                        continue;
                    }

                    if (putDataAt != dataBuf) {
                        // The last tick's buffer was not fully contructed
                        // before this new tick showed up!
                        if (debug) printf("Discard tick %" PRIu64 "\n", packetTick);

                        // We have a problem here, the first packet of this tick, unfortunately,
                        // is at the end of the buffer storing the previous tick. We must move it
                        // to the front of the buffer and overwrite the previous tick.
                        // This will happen if the end of the previous tick is completely dropped
                        // and the first packet of the new tick is read.
                        memcpy(dataBuf, putDataAt, nBytes);

                        maxPacketBytes   = 0;
                        totalBytesRead   = 0;
                        putDataAt        = dataBuf;
                        remainingLen     = bufLen;
                    }

                    // If here, new tick/buffer, offset = 0.
                    // There's a chance we can construct a full buffer.

                    // Dump everything we saved from previous tick.
                    // Delete all out-of-seq packets.
                    clearMap(outOfOrderPackets);
                    dumpTick = false;
                }
                // Same tick as last packet
                else {

                    if (offsetLong - prevOffsetLong <= 0) {
                        printf("GOT SAME or DECREASING Offset, %" PRId64 " (from %" PRId64 ")\n", offsetLong, prevOffsetLong);
                        continue;
                    }

                    if (dumpTick || (offsetDiff > 0)) {
                        // If here, the offset hopped by more than it should,
                        // probably dropped at least 1 packet,
                        // so drop rest of packets for record.
                        // This branch of the "if" will no longer
                        // be executed once the next record shows up.
                        veryFirstRead = true;
                        dumpTick = true;
                        prevOffset = offset;
                        prevOffsetLong = offsetLong;
                        prevPacketLast = packetLast;

                        if (debug) printf("Dump pkt from id %hu, %" PRIu64 " - %u, expected seq 0\n", packetDataId, packetTick, offset);
                        continue;
                    }
                }

                if (offset == 0) {
                    firstReadForBuf = true;
                    totalBytesRead = 0;
                    putDataAt = dataBuf;
                }

                prevTick   = packetTick;
                prevOffset = offset;
                prevOffsetLong = offsetLong;
                prevPacketLast = packetLast;

                if (debug) fprintf(stderr, "Received %d bytes, offset %u, last = %s, firstReadForBuf = %s\n",
                                   nBytes, offset, btoa(packetLast), btoa(firstReadForBuf));

                // Check to see if packet is out-of-sequence
                if ((offsetDiff > 0)) {
                    fprintf(stderr, "\n    Got seq %u, expecting %u\n", offset, expectedOffset);

                    // If we get one that we already received, ERROR!
                    if (offset < expectedOffset) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "getCompletePacketizedBuffer: already got seq %u, id %hu, t %" PRIu64 "\n", offset, packetDataId, packetTick);
                        return OUT_OF_ORDER;
                    }

                    // Set a limit on how much we're going to store (200 packets) while we wait
                    if (outOfOrderPackets.size() >= 200 || sizeof(outOfOrderPackets) >= outOfOrderPackets.max_size() ) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "getCompletePacketizedBuffer: reached size limit of stored packets!\n");
                        return OUT_OF_ORDER;
                    }

                    // Since it's out of order, what was written into dataBuf will need to be
                    // copied and stored. And that written data will eventually need to be
                    // overwritten with the correct packet data.
                    char *tempBuf = (char *) malloc(nBytes);
                    if (tempBuf == nullptr) {
                        clearMap(outOfOrderPackets);
                        fprintf(stderr, "getCompletePacketizedBuffer: ran out of memory storing packets!\n");
                        return OUT_OF_MEM;
                    }
                    memcpy(tempBuf, putDataAt, nBytes);

                    // Put it into map
                    if (debug) fprintf(stderr, "    Save and store packet %u, packetLast = %s\n", offset, btoa(packetLast));
                    outOfOrderPackets.emplace(offset, std::tuple<char *, uint32_t, bool, bool>{tempBuf, nBytes, packetLast, packetFirst});
                    // Read next packet
                    continue;
                }

                while (true) {
                    if (debug) fprintf(stderr, "Packet %u in proper order, last = %s\n", offset, btoa(packetLast));

                    // Packet was in proper order. Get ready to look for next in offset.
                    putDataAt += nBytes;
                    remainingLen -= nBytes;
                    totalBytesRead += nBytes;
                    expectedOffset++;

                    // If it's the first read of a offset, and there are more reads to come,
                    // the # of bytes it read will be max possible. Remember that.
                    if (firstReadForBuf) {
                        maxPacketBytes = nBytes;
                        firstReadForBuf = false;
                        //maxPacketsInBuf = bufLen / maxPacketBytes;
                        if (debug) fprintf(stderr, "In first read, max bytes/packet = %lu\n", maxPacketBytes);

                        // Error check
                        if (!packetFirst) {
                            fprintf(stderr, "getCompletePacketizedBuffer: expecting first bit to be set on very first read but wasn't\n");
                            clearMap(outOfOrderPackets);
                            return BAD_FIRST_LAST_BIT;
                        }
                    }
                    else if (packetFirst) {
                        fprintf(stderr, "getCompletePacketizedBuffer: expecting first bit NOT to be set on read but was\n");
                        clearMap(outOfOrderPackets);
                        return BAD_FIRST_LAST_BIT;
                    }

                    if (debug) fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s, OUTofOrder = %lu\n\n",
                                       remainingLen, expectedOffset, btoa(packetFirst), btoa(packetLast),
                                       outOfOrderPackets.size());

                    // If no stored, out-of-order packets ...
                    if (outOfOrderPackets.empty()) {
                        // If very last packet, quit
                        if (packetLast) {
                            // Finish up some stats
                            if (takeStats) {
                                int64_t diff = 0;
                                uint32_t droppedTicks = 0;
                                if (knowExpectedTick) {
                                    diff = packetTick - expectedTick;
                                    droppedTicks = diff / tickPrescale;
                                }

                                // Total microsec to read buffer
                                //                                stats->readTime += stats->endTime - stats->startTime;
                                stats->acceptedBytes += totalBytesRead;
                                stats->acceptedPackets += offset + 1;
                                //fprintf(stderr, "        accepted pkts = %llu, seq = %u\n", stats->acceptedPackets, offset);
                                stats->droppedTicks   += droppedTicks;
                                // This works if all the buffers coming in are exactly the same size.
                                // If they're not, then the offset (# of packets - 1) of this buffer
                                // is used to guess at how many packets were dropped for the dropped tick(s).
                                stats->droppedPackets += droppedTicks * (offset + 1);
                                //if (droppedTicks != 0) printf("Dropped %u ticks, tick diff %" PRId64 ", packets = %" PRIu64 ", seq#s = %u\n",
                                //                              droppedTicks, diff, stats->droppedPackets, (offset + 1));
                            }
                            break;
                        }
                        if (remainingLen < 1) fprintf(stderr, "        remaining len = %zu\n", remainingLen);
                    }
                        // If there were previous packets out-of-order, they may now be in order.
                        // If so, write them into buffer.
                        // Remember the map already sorts them into proper offset.
                    else {
                        if (debug) fprintf(stderr, "We also have stored packets\n");
                        // Go to first stored packet
                        auto it = outOfOrderPackets.begin();

                        // If it's truly the next packet ...
                        if (it->first == expectedOffset) {
                            char *data  = std::get<0>(it->second);
                            nBytes      = std::get<1>(it->second);
                            packetLast  = std::get<2>(it->second);
                            packetFirst = std::get<3>(it->second);
                            offset = expectedOffset;

                            // Another packet of data may exceed buffer space
                            if (remainingLen < nBytes) {
                                fprintf(stderr, "getCompletePacketizedBuffer: buffer too small at %zu bytes\n", bufLen);
                                clearMap(outOfOrderPackets);
                                return BUF_TOO_SMALL;
                            }

                            memcpy(putDataAt, data, nBytes);
                            free(data);

                            // Remove packet from map
                            it = outOfOrderPackets.erase(it);
                            if (debug) fprintf(stderr, "Go and add stored packet %u, size of map = %lu, last = %s\n",
                                               expectedOffset, outOfOrderPackets.size(), btoa(packetLast));
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
            if (dataId != nullptr) {*dataId = packetDataId;}
            clearMap(outOfOrderPackets);
            return totalBytesRead;
        }


#ifdef __linux__
//
///**
// * <p>
// * Assemble incoming packets into the given buffer.
// * It will read entire buffer or return an error.
// * This routine does NOT allow for out-of-order packets.
// * </p>
// *
// * <p>
// * If the given tick value is <b>NOT</b> 0xffffffffffffffff, then it is the next expected tick.
// * And in this case, this method makes a number of assumptions:
// * <ul>
// * <li>Each successive tick differs by tickPrescale.</li>
// * <li>If the offset is &gt; the total received data, then a packet or packets have been dropped.
// *     This results from the observation that for a simple network,
// *     there are never out-of-order packets.</li>
// * </ul>
// * </p>
// * <p>
// * This routine uses recvfrom to read in packets, but minimizes the copying of data
// * by copying as much data as possible, directly to dataBuf. This involves storing
// * what temporarily gets overwritten by a RE header and then restoring it once the
// * read of a packet is complete.
// *</p>
// *
// * <p>
// * A note on statistics. The raw counts are <b>ADDED</b> to what's already
// * in the stats structure. It's up to the user to clear stats before calling
// * this method if desired.
// * </p>
// *
// * @param dataBuf           place to store assembled packets.
// * @param bufLen            byte length of dataBuf.
// * @param udpSocket         UDP socket to read.
// * @param debug             turn debug printout on & off.
// * @param tick              value-result parameter which gives the next expected tick
// *                          and returns the tick that was built. If it's passed in as
// *                          0xffff ffff ffff ffff, then ticks are coming in no particular order.
// * @param dataId            to be filled with data ID from RE header (can be nullptr).
// * @param stats             to be filled packet statistics.
// * @param tickPrescale      add to current tick to get next expected tick.
// * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
// *
// * @return total data bytes read (does not include RE header).
// *         If there's an error in recvfrom, it will return RECV_MSG.
// *         If the buffer is too small to receive a single tick's data, it will return BUF_TOO_SMALL.
// *         If a packet is out of order and no recovery is possible (e.g. duplicate offset),
// *              it will return OUT_OF_ORDER.
// *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
// *         If cannot allocate memory, it will return OUT_OF_MEM.
// *         If on a read no data is returned when buffer not filled, return INTERNAL_ERROR.
// *         If on a read &lt; HEADER_BYTES data returned, not enough data to contain header.
// *              Then some sort of internal error and will return INTERNAL_ERROR.
// */
//        static ssize_t getCompleteBuffers(/* Queue on which to get and put buffers, */
//                                          int udpSocket, struct mmsghdr *msgvec, unsigned int vlen,
//                                          int flags, struct timespec *timeout, bool debug,
//                                          std::shared_ptr<packetRecvStats> stats, uint32_t tickPrescale,
//                                          std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {
//
//            uint64_t prevTick = UINT_MAX;
//            uint64_t expectedTick = *tick;
//            uint64_t packetTick;
//
//            uint32_t offset, prevOffset = 0, expectedOffset = 0;
//            int64_t offsetLong = 0, prevOffsetLong = 0, offsetDiff = 0;
//
//            uint32_t length, prevLength = 0;
//            int64_t lengthLong = 0, prevLengthLong = 0, lengthDiff = 0;
//
//            bool packetFirst, packetLast, prevPacketLast = true;
//            bool dumpTick = false;
//            bool firstReadForBuf = false;
//            bool takeStats = stats != nullptr;
//            bool veryFirstRead = true;
//            bool outOfSequence = false;
//
//            bool knowExpectedTick = expectedTick != 0xffffffffffffffffL;
//
//            int  version, nBytes;
//            //       int  bytesRead;
//            ssize_t bytesRead;
//            uint16_t packetDataId;
//            size_t  maxPacketBytes = 0;
//            ssize_t totalBytesRead = 0;
//
//            char headerStorage[HEADER_BYTES];
//
//            char *writeHeaderAt, *putDataAt = dataBuf;
//            size_t remainingLen = bufLen;
//            struct timespec now {0,0};
//
//
////            struct iovec {
////                ptr_t iov_base; /* Starting address */
////                size_t iov_len; /* Length in bytes */
////            }
//
//
//            //     struct msghdr {
//            //             void            *msg_name;      /* optional address */
//            //             socklen_t       msg_namelen;    /* size of address */
//            //             struct          iovec *msg_iov; /* scatter/gather array */
//            //             size_t          msg_iovlen;     /* # elements in msg_iov */
//            //             void            *msg_control;   /* ancillary data, see below */
//            //             size_t          msg_controllen; /* ancillary data buffer len */
//            //             int             msg_flags;      /* flags on received message */
//            //     };
//
//
//            //           struct mmsghdr {
//            //               struct msghdr msg_hdr;  /* Message header */
//            //               unsigned int  msg_len;  /* Number of received bytes for header */
//            //           };
//
//
//            //       #define VLEN 10
//            //       #define BUFSIZE 200
//            //       #define TIMEOUT 1
//            //           int sockfd, retval, i;
//            //           struct mmsghdr msgs[VLEN];
//            //           struct iovec iovecs[VLEN];
//            //           char bufs[VLEN][BUFSIZE+1];
//            //           struct timespec timeout;
//            //
//            //           memset(msgs, 0, sizeof(msgs));
//            //           for (i = 0; i < VLEN; i++) {
//            //               iovecs[i].iov_base         = bufs[i];
//            //               iovecs[i].iov_len          = BUFSIZE;
//            //               msgs[i].msg_hdr.msg_iov    = &iovecs[i];
//            //               msgs[i].msg_hdr.msg_iovlen = 1;
//            //           }
//            //
//            //           timeout.tv_sec = TIMEOUT;
//            //           timeout.tv_nsec = 0;
//            //
//            //           retval = recvmmsg(sockfd, msgs, VLEN, 0, &timeout);
//            //           if (retval == -1) {
//            //               perror("recvmmsg()");
//            //               exit(EXIT_FAILURE);
//            //           }
//            //
//            //           printf("%d messages received\n", retval);
//            //           for (i = 0; i < retval; i++) {
//            //               bufs[i][msgs[i].msg_len] = 0;
//            //               printf("%d %s", i+1, bufs[i]);
//            //           }
//            //           exit(EXIT_SUCCESS);
//
//// Put this in here so the included special queue in headers will compile
//moodycamel::BlockingConcurrentQueue<int> q;
//
//// TODO: HOW is a buffer passed back??????
//
//            if (debug && takeStats) fprintf(stderr, "getCompletePacketizedBuffer: remainingLen = %lu, take stats = %d, %p\n",
//                                            remainingLen, takeStats, stats.get());
//
//            again:
//            while (true) {
//
//                // Read up to vlen number of packets
//                int packetCount = recvmmsg(udpSocket, msgvec, vlen, MSG_WAITFORONE, timeout);
//                if (packetCount == -1) {
//                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
//                        // if timeout, go 'round again
//                        goto again;
//                    }
//                    return error;
//                }
//
//                for (int i = 0; i < packetCount; i++) {
//
//                    struct msghdr msgHdr = msgvec[i].msg_hdr;
//                    unsigned int msgLen  = msgvec[i].msg_len;
//
//                    // Actual data from packet is here
//                    char *dataBuf     = (char *) (msgHdr->msg_iov[1].iov_base);
//                    size_t dataBufLen = msgHdr->msg_iov[1].iov_len;
//
//                    // Reassembly header from packet is here
//                    char *reHdr     = (char *) (msgHdr->msg_iov[0].iov_base);
//                    size_t reHdrLen = msgHdr->msg_iov[0].iov_len; // This should always be 20 bytes
//
//                    if (reHdrLen != 20) {
//                        // Error in reading RE header
//                        return error;
//                    }
//
//                    // Parse header
//                    parseReHeader(reHdr, &version, &packetDataId, &length, &offset, &packetTick);
//
//
//
//                    // Another packet of data will exceed buffer space, so quit
//                    if (remainingLen <= dataBufLen) {
//                        if (takeStats && stats->droppedPackets > 0) {
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: dropping packets?, remaining len <= header\n");
//                        }
//                        else {
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: buffer too small?, remaining len <= header\n");
//                        }
//                        return BUF_TOO_SMALL;
//                    }
//
//                    if (veryFirstRead) {
//
//                        maxPacketBytes = 0;
//                        totalBytesRead = 0;
//                        expectedOffset = 0;
//                        putDataAt = dataBuf;
//                        remainingLen = bufLen;
//                        outOfSequence = false;
//
//
//                        // Read in one packet, return value does NOT include RE header
//                        nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
//                                                    &packetTick, &length, &offset,
//                                                    &packetDataId, &version, debug);
//
//                        // If error
//                        if (nBytes < 0) {
//                            clearMap(outOfOrderPackets);
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: on first read, buf too small? nBytes = %d, remainingLen = %zu\n",
//                                    nBytes, remainingLen);
//                            return nBytes;
//                        }
//                        else if (nBytes == 0) {
//                            // Something clearly wrong. There should be SOME data returned.
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: on first read, buf too small? nBytes = 0, remainingLen = %zu\n",
//                                    remainingLen);
//                            clearMap(outOfOrderPackets);
//                            return INTERNAL_ERROR;
//                        }
//
//                        //                    if (takeStats) {
//                        //                        clock_gettime(CLOCK_MONOTONIC, &now);
//                        //                        stats->startTime = 1000000L * now.tv_sec + now.tv_nsec/1000L; // microseconds
//                        //                    }
//
//                        veryFirstRead = false;
//                    }
//                    else {
//                        writeHeaderAt = putDataAt - HEADER_BYTES;
//                        // Copy part of buffer that we'll temporarily overwrite
//                        memcpy(headerStorage, writeHeaderAt, HEADER_BYTES);
//
//                        // Read data right into final buffer (including RE header)
//                        bytesRead = recvfrom(udpSocket, writeHeaderAt, remainingLen, 0, NULL, NULL);
//                        if (bytesRead < 0) {
//                            fprintf(stderr, "getCompletePacketizedBuffer: recvfrom failed: %s\n", strerror(errno));
//                            clearMap(outOfOrderPackets);
//                            return (RECV_MSG);
//                        }
//                        else if (bytesRead < HEADER_BYTES) {
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: not enough data to contain a header on read\n");
//                            clearMap(outOfOrderPackets);
//                            return (INTERNAL_ERROR);
//                        }
//
//                        nBytes = bytesRead - HEADER_BYTES;
//
//                        if (nBytes == 0) {
//                            // Something clearly wrong. There should be SOME data besides header returned.
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: buf too small? nBytes = %d, remainingLen = %zu\n",
//                                    nBytes, remainingLen);
//                            clearMap(outOfOrderPackets);
//                            return INTERNAL_ERROR;
//                        }
//
//                        // Parse header
//                        parseReHeader(writeHeaderAt, &version, &packetDataId, &length, &offset, &packetTick);
//
//                        //                    if (takeStats && packetLast) {
//                        //                        // This may or may not be the actual last packet.
//                        //                        // (A whole buffer may have been dropped after last received packet.)
//                        //                        // So, for now, just record time in interest of getting a good time value.
//                        //                        // This may be overwritten later if it turns out we had some dropped packets.
//                        //                        clock_gettime(CLOCK_MONOTONIC, &now);
//                        //                        stats->endTime = 1000000L * now.tv_sec + now.tv_nsec/1000L;
//                        //                    }
//
//                        // Replace what was written over
//                        memcpy(writeHeaderAt, headerStorage, HEADER_BYTES);
//                    }
//
//                    // What happens when you subtract 2 unsigned ints? (result modulo UINT_MAX + 1).
//                    // Avoid problems with unsigned int arithmetic and assign to int64_t.
//                    offsetLong = ((int64_t) offset & 0xffffffffL);
//                    lengthLong = ((int64_t) length & 0xffffffffL);
//                    offsetDiff = offsetLong - totalBytesRead;
//                    if (offsetDiff != 0) {
//                        outOfSequence = true;
//                    }
//
//                    packetLast = false;
//                    if (bytesRead == length) {
//                        packetLast = true;
//                    }
//
//                    packetFirst = false;
//                    if (offset == 0) {
//                        packetFirst = true;
//                    }
//
//                    //                if (packetTick != expectedTick) {
//                    //                    printf("Packet != expected tick, got %" PRIu64 ", ex = %" PRIu64 ", prev = %" PRIu64 "\n",
//                    //                           packetTick, expectedTick, prevTick);
//                    //                }
//
//                    // This if-else statement is what enables the packet reading/parsing to keep
//                    // up an input rate that is too high (causing dropped packets) and still salvage
//                    // some of what is coming in.
//                    if (packetTick != prevTick) {
//                        // If we're here, either we've just read the very first legitimate packet,
//                        // or we've dropped some packets and advanced to another tick in the process.
//
//                        expectedOffset = 0;
//
//                        if (offset != 0) {
//                            // Already have trouble, looks like we dropped the first packet of a tick,
//                            // and possibly others after it.
//                            // So go ahead and dump the rest of the tick in an effort to keep up.
//                            if (debug)
//                                printf("Skip pkt from id %hu, %" PRIu64 " - %u, expected seq 0\n", packetDataId,
//                                       packetTick, offset);
//                            veryFirstRead = true;
//                            dumpTick = true;
//                            prevTick = packetTick;
//                            prevOffset = offset;
//                            prevOffsetLong = offsetLong;
//                            prevPacketLast = packetLast;
//
//                            continue;
//                        }
//
//                        if (putDataAt != dataBuf) {
//                            // The last tick's buffer was not fully contructed
//                            // before this new tick showed up!
//                            if (debug) printf("Discard tick %" PRIu64 "\n", packetTick);
//
//                            // We have a problem here, the first packet of this tick, unfortunately,
//                            // is at the end of the buffer storing the previous tick. We must move it
//                            // to the front of the buffer and overwrite the previous tick.
//                            // This will happen if the end of the previous tick is completely dropped
//                            // and the first packet of the new tick is read.
//                            memcpy(dataBuf, putDataAt, nBytes);
//
//                            maxPacketBytes = 0;
//                            totalBytesRead = 0;
//                            putDataAt = dataBuf;
//                            remainingLen = bufLen;
//                        }
//
//                        // If here, new tick/buffer, offset = 0.
//                        // There's a chance we can construct a full buffer.
//
//                        // Dump everything we saved from previous tick.
//                        // Delete all out-of-seq packets.
//                        clearMap(outOfOrderPackets);
//                        dumpTick = false;
//                    }
//                        // Same tick as last packet
//                    else {
//
//                        if (offsetLong - prevOffsetLong <= 0) {
//                            printf("GOT SAME or DECREASING Offset, %" PRId64 " (from %" PRId64 ")\n", offsetLong,
//                                   prevOffsetLong);
//                            continue;
//                        }
//
//                        if (dumpTick || (offsetDiff > 0)) {
//                            // If here, the offset hopped by more than it should,
//                            // probably dropped at least 1 packet,
//                            // so drop rest of packets for record.
//                            // This branch of the "if" will no longer
//                            // be executed once the next record shows up.
//                            veryFirstRead = true;
//                            dumpTick = true;
//                            prevOffset = offset;
//                            prevOffsetLong = offsetLong;
//                            prevPacketLast = packetLast;
//
//                            if (debug)
//                                printf("Dump pkt from id %hu, %" PRIu64 " - %u, expected seq 0\n", packetDataId,
//                                       packetTick, offset);
//                            continue;
//                        }
//                    }
//
//                    if (offset == 0) {
//                        firstReadForBuf = true;
//                        totalBytesRead = 0;
//                        putDataAt = dataBuf;
//                    }
//
//                    prevTick = packetTick;
//                    prevOffset = offset;
//                    prevOffsetLong = offsetLong;
//                    prevPacketLast = packetLast;
//
//                    if (debug)
//                        fprintf(stderr, "Received %d bytes, offset %u, last = %s, firstReadForBuf = %s\n",
//                                nBytes, offset, btoa(packetLast), btoa(firstReadForBuf));
//
//                    // Check to see if packet is out-of-sequence
//                    if ((offsetDiff > 0)) {
//                        fprintf(stderr, "\n    Got seq %u, expecting %u\n", offset, expectedOffset);
//
//                        // If we get one that we already received, ERROR!
//                        if (offset < expectedOffset) {
//                            clearMap(outOfOrderPackets);
//                            fprintf(stderr, "getCompletePacketizedBuffer: already got seq %u, id %hu, t %" PRIu64 "\n",
//                                    offset, packetDataId, packetTick);
//                            return OUT_OF_ORDER;
//                        }
//
//                        // Set a limit on how much we're going to store (200 packets) while we wait
//                        if (outOfOrderPackets.size() >= 200 ||
//                            sizeof(outOfOrderPackets) >= outOfOrderPackets.max_size()) {
//                            clearMap(outOfOrderPackets);
//                            fprintf(stderr, "getCompletePacketizedBuffer: reached size limit of stored packets!\n");
//                            return OUT_OF_ORDER;
//                        }
//
//                        // Since it's out of order, what was written into dataBuf will need to be
//                        // copied and stored. And that written data will eventually need to be
//                        // overwritten with the correct packet data.
//                        char *tempBuf = (char *) malloc(nBytes);
//                        if (tempBuf == nullptr) {
//                            clearMap(outOfOrderPackets);
//                            fprintf(stderr, "getCompletePacketizedBuffer: ran out of memory storing packets!\n");
//                            return OUT_OF_MEM;
//                        }
//                        memcpy(tempBuf, putDataAt, nBytes);
//
//                        // Put it into map
//                        if (debug)
//                            fprintf(stderr, "    Save and store packet %u, packetLast = %s\n", offset,
//                                    btoa(packetLast));
//                        outOfOrderPackets.emplace(offset,
//                                                  std::tuple<char *, uint32_t, bool, bool>{tempBuf, nBytes, packetLast,
//                                                                                           packetFirst});
//                        // Read next packet
//                        continue;
//                    }
//
//                    while (true) {
//                        if (debug) fprintf(stderr, "Packet %u in proper order, last = %s\n", offset, btoa(packetLast));
//
//                        // Packet was in proper order. Get ready to look for next in offset.
//                        putDataAt += nBytes;
//                        remainingLen -= nBytes;
//                        totalBytesRead += nBytes;
//                        expectedOffset++;
//
//                        // If it's the first read of a offset, and there are more reads to come,
//                        // the # of bytes it read will be max possible. Remember that.
//                        if (firstReadForBuf) {
//                            maxPacketBytes = nBytes;
//                            firstReadForBuf = false;
//                            //maxPacketsInBuf = bufLen / maxPacketBytes;
//                            if (debug) fprintf(stderr, "In first read, max bytes/packet = %lu\n", maxPacketBytes);
//
//                            // Error check
//                            if (!packetFirst) {
//                                fprintf(stderr,
//                                        "getCompletePacketizedBuffer: expecting first bit to be set on very first read but wasn't\n");
//                                clearMap(outOfOrderPackets);
//                                return BAD_FIRST_LAST_BIT;
//                            }
//                        }
//                        else if (packetFirst) {
//                            fprintf(stderr,
//                                    "getCompletePacketizedBuffer: expecting first bit NOT to be set on read but was\n");
//                            clearMap(outOfOrderPackets);
//                            return BAD_FIRST_LAST_BIT;
//                        }
//
//                        if (debug)
//                            fprintf(stderr,
//                                    "remainingLen = %lu, expected offset = %u, first = %s, last = %s, OUTofOrder = %lu\n\n",
//                                    remainingLen, expectedOffset, btoa(packetFirst), btoa(packetLast),
//                                    outOfOrderPackets.size());
//
//                        // If no stored, out-of-order packets ...
//                        if (outOfOrderPackets.empty()) {
//                            // If very last packet, quit
//                            if (packetLast) {
//                                // Finish up some stats
//                                if (takeStats) {
//                                    int64_t diff = 0;
//                                    uint32_t droppedTicks = 0;
//                                    if (knowExpectedTick) {
//                                        diff = packetTick - expectedTick;
//                                        droppedTicks = diff / tickPrescale;
//                                    }
//
//                                    // Total microsec to read buffer
//                                    //                                stats->readTime += stats->endTime - stats->startTime;
//                                    stats->acceptedBytes += totalBytesRead;
//                                    stats->acceptedPackets += offset + 1;
//                                    //fprintf(stderr, "        accepted pkts = %llu, seq = %u\n", stats->acceptedPackets, offset);
//                                    stats->droppedTicks += droppedTicks;
//                                    // This works if all the buffers coming in are exactly the same size.
//                                    // If they're not, then the offset (# of packets - 1) of this buffer
//                                    // is used to guess at how many packets were dropped for the dropped tick(s).
//                                    stats->droppedPackets += droppedTicks * (offset + 1);
//                                    //if (droppedTicks != 0) printf("Dropped %u ticks, tick diff %" PRId64 ", packets = %" PRIu64 ", seq#s = %u\n",
//                                    //                              droppedTicks, diff, stats->droppedPackets, (offset + 1));
//                                }
//                                break;
//                            }
//                            if (remainingLen < 1) fprintf(stderr, "        remaining len = %zu\n", remainingLen);
//                        }
//                            // If there were previous packets out-of-order, they may now be in order.
//                            // If so, write them into buffer.
//                            // Remember the map already sorts them into proper offset.
//                        else {
//                            if (debug) fprintf(stderr, "We also have stored packets\n");
//                            // Go to first stored packet
//                            auto it = outOfOrderPackets.begin();
//
//                            // If it's truly the next packet ...
//                            if (it->first == expectedOffset) {
//                                char *data = std::get<0>(it->second);
//                                nBytes = std::get<1>(it->second);
//                                packetLast = std::get<2>(it->second);
//                                packetFirst = std::get<3>(it->second);
//                                offset = expectedOffset;
//
//                                // Another packet of data may exceed buffer space
//                                if (remainingLen < nBytes) {
//                                    fprintf(stderr, "getCompletePacketizedBuffer: buffer too small at %zu bytes\n",
//                                            bufLen);
//                                    clearMap(outOfOrderPackets);
//                                    return BUF_TOO_SMALL;
//                                }
//
//                                memcpy(putDataAt, data, nBytes);
//                                free(data);
//
//                                // Remove packet from map
//                                it = outOfOrderPackets.erase(it);
//                                if (debug)
//                                    fprintf(stderr, "Go and add stored packet %u, size of map = %lu, last = %s\n",
//                                            expectedOffset, outOfOrderPackets.size(), btoa(packetLast));
//                                continue;
//                            }
//                        }
//
//                        break;
//                    }
//
//                    veryFirstRead = false;
//
//                    if (packetLast) {
//                        break;
//                    }
//                }
//            }
//
//            *tick   = packetTick;
//            if (dataId != nullptr) {*dataId = packetDataId;}
//            clearMap(outOfOrderPackets);
//            return totalBytesRead;
//        }

#endif

        /**
         *
         * Remove all out of order recovery.
         *
         * <p>
         * Assemble incoming packets into the given buffer.
         * It will read entire buffer or return an error.
         * This routine does NOT allow for out-of-order packets.
         * </p>
         *
         * <p>
         * If the given tick value is <b>NOT</b> 0xffffffffffffffff, then it is the next expected tick.
         * And in this case, this method makes a number of assumptions:
         * <ul>
         * <li>Each successive tick differs by tickPrescale.</li>
         * <li>If the offset is &gt; the total received data, then a packet or packets have been dropped.
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
         * @param dataId            to be filled with data ID from RE header (can be nullptr).
         * @param tickPrescale      add to current tick to get next expected tick.
         * @param stats             to be filled packet statistics.
         *
         * @return total data bytes read (does not include RE header).
         *         If there's an error in recvfrom, it will return RECV_MSG.
         *         If the buffer is too small to receive a single tick's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate offset),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         *         If on a read no data is returned when buffer not filled, return INTERNAL_ERROR.
         *         If on a read &lt; HEADER_BYTES data returned, not enough data to contain header.
         *              Then some sort of internal error and will return INTERNAL_ERROR.
         */
        static ssize_t getCompletePacketizedBuffer_NotOutOfOrder(
                                    char* dataBuf, size_t bufLen, int udpSocket, bool debug,
                                    uint64_t *tick, uint16_t *dataId, uint32_t tickPrescale,
                                    const std::shared_ptr<packetRecvStats> & stats) {

            uint64_t prevTick = UINT_MAX;
            uint64_t expectedTick = *tick;
            uint64_t packetTick;
            uint64_t packetCount = 0;

            uint32_t offset, prevOffset = 0, expectedOffset = 0;
            int64_t offsetLong = 0, prevOffsetLong = 0, offsetDiff = 0;

            uint32_t length, prevLength = 0;
            int64_t lengthLong = 0, prevLengthLong = 0, lengthDiff = 0;

            bool packetFirst, packetLast, prevPacketLast = true;
            bool dumpTick = false;
            bool firstReadForBuf = false;
            bool takeStats = stats != nullptr;
            bool veryFirstRead = true;
            bool outOfSequence = false;

            // Do we know what's being sent? so we can attempt to figure out the dropped
            // number of bytes? The best we can do, if a whole tick is dropped, is to
            // assume it's size is the same as the last fully reassembled buffer.
            // Note that the tick sent to this receiver may not be sequential.
            // In fact, it most probably won't except while it's being tested.
            bool knowExpectedTick = expectedTick != 0xffffffffffffffffL;

            int  version, nBytes;
            ssize_t bytesRead;
            uint16_t packetDataId;
            ssize_t totalBytesRead = 0;
            uint64_t droppedBytes  = 0;

            char headerStorage[HEADER_BYTES];

            char *writeHeaderAt, *putDataAt = dataBuf;
            size_t remainingLen = bufLen;
            struct timespec now;

            if (debug && takeStats) fprintf(stderr, "getCompletePacketizedBuffer: remainingLen = %lu, take stats = %d, %p\n",
                                            remainingLen, takeStats, stats.get());

            while (true) {

                // Another packet of data will exceed buffer space, so quit
                if (remainingLen <= HEADER_BYTES) {
                    if (takeStats && stats->droppedPackets > 0) {
                        fprintf(stderr, "getCompletePacketizedBuffer: dropping packets?, remaining len <= header\n");
                    }
                    else {
                        fprintf(stderr, "getCompletePacketizedBuffer: buffer too small?, remaining len <= header\n");
                    }
                    return BUF_TOO_SMALL;
                }

                if (veryFirstRead) {

                    totalBytesRead = 0;
                    expectedOffset = 0;
                    putDataAt      = dataBuf;
                    remainingLen   = bufLen;
                    packetFirst    = false;
                    packetLast     = false;
                    outOfSequence  = false;
                    packetCount    = 0;

                    // Read in one packet, return value does NOT include RE header
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &length, &offset,
                                                &packetDataId, &version, debug);

                    // If error
                    if (nBytes < 0) {
                        fprintf(stderr, "getCompletePacketizedBuffer: on first read, buf too small? nBytes = %d, remainingLen = %zu\n", nBytes, remainingLen);
                        return nBytes;
                    }
                    else if (nBytes == 0) {
                        // Something clearly wrong. There should be SOME data returned.
                        fprintf(stderr, "getCompletePacketizedBuffer: on first read, buf too small? nBytes = 0, remainingLen = %zu\n", remainingLen);
                        return INTERNAL_ERROR;
                    }

                     if (takeStats) {
                         clock_gettime(CLOCK_MONOTONIC, &now);
                         stats->startTime = 1000000L * now.tv_sec + now.tv_nsec/1000L; // microseconds
                     }

                    veryFirstRead = false;
                }
                else {
                    writeHeaderAt = putDataAt - HEADER_BYTES;
                    // Copy part of buffer that we'll temporarily overwrite
                    memcpy(headerStorage, writeHeaderAt, HEADER_BYTES);

                    // Read data right into final buffer (including RE header)
                    bytesRead = recvfrom(udpSocket, writeHeaderAt, remainingLen, 0, nullptr, nullptr);
                    if (bytesRead < 0) {
                        fprintf(stderr, "getCompletePacketizedBuffer: recvfrom failed: %s\n", strerror(errno));
                        return(RECV_MSG);
                    }
                    else if (bytesRead < HEADER_BYTES) {
                        fprintf(stderr, "getCompletePacketizedBuffer: not enough data to contain a header on read\n");
                        return(INTERNAL_ERROR);
                    }

                    nBytes = (int)bytesRead - HEADER_BYTES;

                    if (nBytes == 0) {
                        // Something clearly wrong. There should be SOME data besides header returned.
                        fprintf(stderr, "getCompletePacketizedBuffer: buf too small? nBytes = %d, remainingLen = %zu\n", nBytes, remainingLen);
                        return INTERNAL_ERROR;
                    }

                    // Parse header
                    parseReHeader(writeHeaderAt, &version, &packetDataId, &offset, &length, &packetTick);

                    // Replace what was written over
                    memcpy(writeHeaderAt, headerStorage, HEADER_BYTES);
                }

                packetCount++;

                // What happens when you subtract 2 unsigned ints? (result modulo UINT_MAX + 1).
                // Avoid problems with unsigned int arithmetic and assign to int64_t.
                offsetLong = ((int64_t)offset & 0xffffffffL);
                lengthLong = ((int64_t)length & 0xffffffffL);
                offsetDiff = offsetLong - totalBytesRead;
                if (offsetDiff != 0) {
                    outOfSequence = true;
                }

                if (bytesRead == length) {
                    packetLast = true;
                }

                if (offset == 0) {
                    packetFirst = true;
                }

                if (takeStats && packetLast) {
                    // This may or may not be the actual last packet.
                    // (A whole buffer may have been dropped after last received packet.)
                    // So, for now, just record time in interest of getting a good time value.
                    // This may be overwritten later if it turns out we had some dropped packets.
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    stats->endTime = 1000000L * now.tv_sec + now.tv_nsec/1000L;
                }

                // Tick is NOT allowed to wrap

                // This if-else statement is what enables the packet reading/parsing to keep
                // up an input rate that is too high (causing dropped packets) and still salvage
                // some of what is coming in.
                if (packetTick != prevTick) {
                    // If we're here, either we've just read the very first legitimate packet,
                    // or we've dropped some packets and advanced to another tick in the process.

                    expectedOffset = 0;

                    if (offset != 0) {
                        // Already have trouble, looks like we dropped the first packet of a tick,
                        // and possibly others after it.
                        // So go ahead and dump the rest of the tick in an effort to keep up.
                        if (debug) printf("Skip pkt from id %hu, %" PRIu64 " - %u, expected seq 0\n", packetDataId, packetTick, offset);
                        veryFirstRead = true;
                        dumpTick = true;
                        prevTick = packetTick;
                        prevOffset = offset;
                        prevOffsetLong = offsetLong;
                        prevPacketLast = packetLast;

                        if (takeStats) {
                            // We're dumping this buf
                            droppedBytes += length;

                            // If the last buf was not fully constructed, record that too
                            if (putDataAt != dataBuf) {
                                droppedBytes += prevLength;
                            }
                        }

                        prevLength = length;

                        continue;
                    }

                    if (putDataAt != dataBuf) {
                        // The last tick's buffer was not fully contructed
                        // before this new tick showed up!
                        if (debug) printf("Discard tick %" PRIu64 "\n", packetTick);

                        // We have a problem here, the first packet of this tick, unfortunately,
                        // is at the end of the buffer storing the previous tick. We must move it
                        // to the front of the buffer and overwrite the previous tick.
                        // This will happen if the end of the previous tick is completely dropped
                        // and the first packet of the new tick is read.
                        memcpy(dataBuf, putDataAt, nBytes);

                        totalBytesRead = 0;
                        putDataAt      = dataBuf;
                        remainingLen   = bufLen;
                        droppedBytes  += prevLength;
                    }

                    // If here, new tick/buffer, offset = 0.
                    // There's a chance we can construct a full buffer.

                    // Dump everything we saved from previous tick.
                    dumpTick = false;
                }
                // Same tick as last packet
                else {

                    if (offsetLong - prevOffsetLong <= 0) {
                        printf("GOT SAME or DECREASING Offset, %" PRId64 " (from %" PRId64 ")\n", offsetLong, prevOffsetLong);
                        continue;
                    }

                    if (dumpTick || outOfSequence) {
                        // If here, the offset hopped by more than it should,
                        // probably dropped at least 1 packet,
                        // so drop rest of packets for buffer.
                        // This branch of the "if" will no longer
                        // be executed once the next record shows up.

                        // For first out-of-seq packet detected, record # bytes that will be dropped
                        if (takeStats && !dumpTick) {
                            droppedBytes += length;
                        }

                        veryFirstRead = true;
                        dumpTick = true;
                        prevOffset = offset;
                        prevOffsetLong = offsetLong;
                        prevPacketLast = packetLast;

                        if (debug) printf("Dump pkt from id %hu, %" PRIu64 " - %u, expected seq 0\n", packetDataId, packetTick, offset);
                        continue;
                    }
                }

                if (offset == 0) {
                    firstReadForBuf = true;
                    totalBytesRead = 0;
                    putDataAt = dataBuf;
                }

                prevTick   = packetTick;
                prevOffset = offset;
                prevOffsetLong = offsetLong;
                prevPacketLast = packetLast;

                if (debug) fprintf(stderr, "Received %d bytes, offset %u, first = %s, last = %s, firstReadForBuf = %s\n",
                                   nBytes, offset, btoa(packetFirst), btoa(packetLast), btoa(firstReadForBuf));

                // Only pocket in proper order get this far
                putDataAt += nBytes;
                remainingLen -= nBytes;
                totalBytesRead += nBytes;
                expectedOffset++;

                // If it's the first read of an offset, and there are more reads to come,
                // the # of bytes it read will be max possible. Remember that.
                if (firstReadForBuf) {
                    firstReadForBuf = false;

                    // Error check
                    if (!packetFirst) {
                        fprintf(stderr, "getCompletePacketizedBuffer: expecting first bit to be set on very first read but wasn't\n");
                        return BAD_FIRST_LAST_BIT;
                    }
                }
                else if (packetFirst) {
                    fprintf(stderr, "getCompletePacketizedBuffer: expecting first bit NOT to be set on read but was\n");
                    return BAD_FIRST_LAST_BIT;
                }

                // If very last packet, quit
                if (packetLast) {
                    // Finish up some stats
                    if (takeStats) {
                        uint64_t diff;
                        uint32_t droppedTicks = 0;
                        if (knowExpectedTick) {
                            diff = packetTick - expectedTick;
                            droppedTicks = diff / tickPrescale;
                        }

                        // Total microsec to read buffer. Using clock takes significant time,
                        // so only do this while debugging.
                        if (debug) stats->readTime += stats->endTime - stats->startTime;

                        stats->acceptedBytes   += totalBytesRead;
                        stats->acceptedPackets += packetCount;
                        stats->droppedTicks    += droppedTicks;
                        stats->droppedBytes    += droppedBytes;
                    }
                }
                else if (remainingLen < 1) {
                    fprintf(stderr, "        remaining len = %zu\n", remainingLen);
                }

                veryFirstRead = false;

                if (packetLast) {
                    break;
                }
            }

            *tick = packetTick;
            if (dataId != nullptr) {*dataId = packetDataId;}
            return totalBytesRead;
        }



//
//        /**
//         * <p>
//         * Assemble incoming packets into a buffer that may be provided by the caller.
//         * If it's null or if it ends up being too small,
//         * the buffer will be created / reallocated and returned by this routine.
//         * A internally allocated buffer is guaranteed to fit all reassembled data.
//         * It's the responsibility of the user to free any buffer that is internally allocated.
//         * If user gives nullptr for buffer and 0 for buffer length, buf defaults to internally
//         * allocated 100kB. If the user provides a buffer < 9000 bytes, a larger one will be allocated.
//         * This routine will read entire buffer or return an error
//         * and allows for out-of-order packets.
//         * </p>
//         *
//         * <p>
//         * How does the caller determine if a buffer was (re)allocated in this routine?
//         * If the returned buffer pointer is different than that supplied or if the supplied
//         * buffer length is smaller than that returned, then the buffer was allocated
//         * internally and must be freed by the caller.
//         * </p>
//         *
//         * <p>
//         * If the given tick value is <b>NOT</b> 0xffffffffffffffff, then it is the next expected tick.
//         * And in this case, this method makes a number of assumptions:
//         * <ul>
//         * <li>Each incoming buffer/tick is split up into the same # of packets.</li>
//         * <li>Each successive tick differs by tickPrescale.</li>
//         * <li>If the sequence changes by more than 1, then a dropped packet is assumed.
//         *     This results from the observation that for a simple network,
//         *     there are never out-of-order packets.</li>
//         * </ul>
//         * </p>
//         * <p>
//         * This routine uses recvfrom to read in packets, but minimizes the copying of data
//         * by copying as much data as possible, directly to dataBuf. This involves storing
//         * what temporarily gets overwritten by a RE header and then restoring it once the
//         * read of a packet is complete.
//         *</p>
//         *
//         * <p>
//         * A note on statistics. The raw counts are <b>ADDED</b> to what's already
//         * in the stats structure. It's up to the user to clear stats before calling
//         * this method if desired.
//         * </p>
//         *
//         * @param dataBufAlloc      value-result pointer to data buffer.
//         *                          User-given buffer to store assembled packets or buffer
//         *                          (re)allocated by this routine. If (re)allocated internally,
//         *                          CALLER MUST FREE THIS BUFFER!
//         * @param bufLenPtr         value-result pointer to byte length of dataBuf.
//         *                          Buffer length of supplied buffer. If no buffer supplied, or if buffer
//         *                          is (re)allocated, the length of the new buffer is passed back to caller.
//         *                          In all cases, the buffer length is returned.
//         * @param udpSocket         UDP socket to read.
//         * @param debug             turn debug printout on & off.
//         * @param tick              value-result parameter which gives the next expected tick
//         *                          and returns the tick that was built. If it's passed in as
//         *                          0xffff ffff ffff ffff, then ticks are coming in no particular order.
//         * @param dataId            to be filled with data ID from RE header (can be nullptr).
//         * @param stats             to be filled packet statistics.
//         * @param tickPrescale      add to current tick to get next expected tick.
//         * @param outOfOrderPackets map for holding out-of-order packets between calls to this function.
//         *
//         * @return total data bytes read (does not include RE header).
//         *         If either dataBufAlloc or bufLenPtr are null, it will return BAD_ARG.
//         *         If there's an error in recvfrom, it will return RECV_MSG.
//         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
//         *              it will return OUT_OF_ORDER.
//         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
//         *         If on a read no data is returned when buffer not filled, return INTERNAL_ERROR.
//         *         If on a read &lt; HEADER_BYTES data returned, not enough data to contain header.
//         *              Then some sort of internal error and will return INTERNAL_ERROR.
//         */
//        static ssize_t getCompleteAllocatedBuffer(char** dataBufAlloc, size_t *bufLenPtr, int udpSocket,
//                                                   bool debug, uint64_t *tick, uint16_t *dataId,
//                                                   std::shared_ptr<packetRecvStats> stats, uint32_t tickPrescale,
//                                                   std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> & outOfOrderPackets) {
//            int64_t  prevTick = -1;
//            uint64_t expectedTick = *tick;
//            uint64_t packetTick;
//            uint32_t sequence, prevSequence = 0, expectedSequence = 0;
//            uint32_t length, offset;
//
//            bool packetFirst, packetLast, prevPacketLast = true;
//            bool dumpTick = false;
//            bool firstReadForBuf = false;
//            bool takeStats = stats != nullptr;
//            bool veryFirstRead = true;
//
//            bool knowExpectedTick = expectedTick != 0xffffffffffffffffL;
//
//            int  version, nBytes, bytesRead;
//            uint16_t packetDataId;
//            size_t  maxPacketBytes = 0;
//            ssize_t totalBytesRead = 0;
//
//            char headerStorage[HEADER_BYTES];
//
//            if (bufLenPtr == nullptr || dataBufAlloc == nullptr) {
//                fprintf(stderr, "getCompletePacketizedBuffer: null arg(s)\n");
//                return BAD_ARG;
//            }
//
//            // Length of buf passed in, or suggested length for this routine to allocate
//            size_t bufLen = *bufLenPtr;
//            bool allocateBuf = false;
//
//            // If we need to allocate buffer
//            if (*dataBufAlloc == nullptr) {
//                if (bufLen == 0) {
//                    // Use default len of 100kB
//                    bufLen = 100000;
//                }
//                else if (bufLen < 9000) {
//                    // Make sure we can at least read one JUMBO packet
//                    bufLen = 9000;
//                }
//                allocateBuf = true;
//            }
//            else {
//                if (bufLen < 9000) {
//                    bufLen = 9000;
//                    allocateBuf = true;
//                }
//            }
//
//            char *dataBuf = *dataBufAlloc;
//            if (allocateBuf) {
//                dataBuf = (char *) malloc(bufLen);
//                if (dataBuf == nullptr) {
//                    return OUT_OF_MEM;
//                }
//            }
//
//            char *writeHeaderAt, *putDataAt;
//            size_t remainingLen = bufLen;
//            struct timespec now;
//
//            if (debug && takeStats) fprintf(stderr, "getCompleteAllocatedBuffer: remainingLen = %lu, take stats = %d, %p\n",
//                                            remainingLen, takeStats, stats.get());
//
//            while (true) {
//
//                // Another packet of data might exceed buffer space, so expand
//                if (remainingLen < 9000) {
//                    // double buffer size here
//                    bufLen *= 2;
//                    // realloc copies data over if necessary
//                    dataBuf = (char *)realloc(dataBuf, bufLen);
//                    if (dataBuf == nullptr) {
//                        clearMap(outOfOrderPackets);
//                        return OUT_OF_MEM;
//                    }
//                    putDataAt = dataBuf + totalBytesRead;
//                    remainingLen = bufLen - totalBytesRead;
//                    if (debug) fprintf(stderr, "getCompleteAllocatedBuffer: reallocated buffer to %zu bytes\n", bufLen);
//                }
//
//                if (veryFirstRead) {
//
//                    maxPacketBytes   = 0;
//                    totalBytesRead   = 0;
//                    expectedSequence = 0;
//                    putDataAt        = dataBuf;
//                    remainingLen     = bufLen;
//
////                    static int readPacketRecvFrom(char *dataBuf, size_t bufLen, int udpSocket,
////                                                  uint64_t *tick, uint32_t *length, uint32_t* offset,
////                                                  uint16_t* dataId, int* version, bool debug) {
//
//                    // Read in one packet, return value does NOT include RE header
//                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
//                                            //    &packetTick, &sequence,
//                                                &packetTick, &length, &offset,
//                                                &packetDataId, &version, debug);
//                    // If error
//                    if (nBytes < 0) {
//                        clearMap(outOfOrderPackets);
//                        fprintf(stderr, "getCompleteAllocatedBuffer: on first read, buf too small? nBytes = %d, remainingLen = %zu\n", nBytes, remainingLen);
//                        return nBytes;
//                    }
//                    else if (nBytes == 0) {
//                        // Something clearly wrong. There should be SOME data returned.
//                        fprintf(stderr, "getCompleteAllocatedBuffer: on first read, buf too small? nBytes = 0, remainingLen = %zu\n", remainingLen);
//                        clearMap(outOfOrderPackets);
//                        return INTERNAL_ERROR;
//                    }
//
//                    // if (takeStats) {
//                    //     clock_gettime(CLOCK_MONOTONIC, &now);
//                    //     stats->startTime = 1000000L * now.tv_sec + now.tv_nsec/1000L; // microseconds
//                    // }
//
//                    veryFirstRead = false;
//                }
//                else {
//                    writeHeaderAt = putDataAt - HEADER_BYTES;
//                    // Copy part of buffer that we'll temporarily overwrite
//                    memcpy(headerStorage, writeHeaderAt, HEADER_BYTES);
//
//                    // Read data right into final buffer (including RE header)
//                    bytesRead = recvfrom(udpSocket, writeHeaderAt, remainingLen, 0, NULL, NULL);
//                    if (bytesRead < 0) {
//                        fprintf(stderr, "getCompleteAllocatedBuffer: recvfrom failed: %s\n", strerror(errno));
//                        clearMap(outOfOrderPackets);
//                        return(RECV_MSG);
//                    }
//                    else if (bytesRead < HEADER_BYTES) {
//                        fprintf(stderr, "getCompleteAllocatedBuffer: not enough data to contain a header on read\n");
//                        clearMap(outOfOrderPackets);
//                        return(INTERNAL_ERROR);
//                    }
//
//                    nBytes = bytesRead - HEADER_BYTES;
//
//                    if (nBytes == 0) {
//                        // Something clearly wrong. There should be SOME data besides header returned.
//                        fprintf(stderr, "getCompleteAllocatedBuffer: buf too small? nBytes = %d, remainingLen = %zu\n", nBytes, remainingLen);
//                        clearMap(outOfOrderPackets);
//                        return INTERNAL_ERROR;
//                    }
//
//                    // Parse header
//                    parseReHeader(writeHeaderAt, &version, &packetFirst, &packetLast, &packetDataId, &sequence, &packetTick);
//
//                    //                    if (takeStats && packetLast) {
//                    //                        // This may or may not be the actual last packet.
//                    //                        // (A whole buffer may have been dropped after last received packet.)
//                    //                        // So, for now, just record time in interest of getting a good time value.
//                    //                        // This may be overwritten later if it turns out we had some dropped packets.
//                    //                        clock_gettime(CLOCK_MONOTONIC, &now);
//                    //                        stats->endTime = 1000000L * now.tv_sec + now.tv_nsec/1000L;
//                    //                    }
//
//                    // Replace what was written over
//                    memcpy(writeHeaderAt, headerStorage, HEADER_BYTES);
//                }
//
//                //                if (packetTick != expectedTick) {
//                //                    printf("Packet != expected tick, got %" PRIu64 ", ex = %" PRIu64 ", prev = %" PRIu64 "\n",
//                //                           packetTick, expectedTick, prevTick);
//                //                }
//
//                // This if-else statement is what enables the packet reading/parsing to keep
//                // up an input rate that is too high (causing dropped packets) and still salvage
//                // some of what is coming in.
//                if (packetTick != prevTick) {
//                    // If we're here, either we've just read the very first legitimate packet,
//                    // or we've dropped some packets and advanced to another tick in the process.
//
//                    expectedSequence = 0;
//
//                    if (sequence != 0) {
//                        // Already have trouble, looks like we dropped the first packet of a tick,
//                        // and possibly others after it.
//                        // So go ahead and dump the rest of the tick in an effort to keep up.
//                        if (debug) printf("Skip pkt from id %hu, %llu - %u, expected seq 0\n", packetDataId, packetTick, sequence);
//                        veryFirstRead = true;
//                        dumpTick = true;
//                        prevTick = packetTick;
//                        prevSequence = sequence;
//                        prevPacketLast = packetLast;
//
//                        continue;
//                    }
//
//                    if (putDataAt != dataBuf) {
//                        // The last tick's buffer was not fully contructed
//                        // before this new tick showed up!
//                        if (debug) printf("Discard tick %llu\n", packetTick);
//
//                        // We have a problem here, the first packet of this tick, unfortunately,
//                        // is at the end of the buffer storing the previous tick. We must move it
//                        // to the front of the buffer and overwrite the previous tick.
//                        // This will happen if the end of the previous tick is completely dropped
//                        // and the first packet of the new tick is read.
//                        memcpy(dataBuf, putDataAt, nBytes);
//
//                        maxPacketBytes   = 0;
//                        totalBytesRead   = 0;
//                        putDataAt        = dataBuf;
//                        remainingLen     = bufLen;
//                    }
//
//                    // If here, new tick/buffer, sequence = 0.
//                    // There's a chance we can construct a full buffer.
//
//                    // Dump everything we saved from previous tick.
//                    // Delete all out-of-seq packets.
//                    clearMap(outOfOrderPackets);
//                    dumpTick = false;
//                }
//                // Same tick as last packet
//                else {
//
//                    if (sequence - prevSequence <= 0) {
//                        printf("GOT SAME or DECREASING Sequence, %u (from %u)\n", sequence, prevSequence);
//                        continue;
//                    }
//
//                    if (dumpTick || (sequence - prevSequence > 1)) {
//                        // If here, the sequence hopped by at least 2,
//                        // probably dropped at least 1,
//                        // so drop rest of packets for record.
//                        // This branch of the "if" will no longer
//                        // be executed once the next record shows up.
//                        veryFirstRead = true;
//                        dumpTick = true;
//                        prevSequence = sequence;
//                        prevPacketLast = packetLast;
//
//                        if (debug) printf("Dump pkt from id %hu, %llu - %u\n", packetDataId, packetTick, sequence);
//                        continue;
//                    }
//                }
//
//                if (sequence == 0) {
//                    firstReadForBuf = true;
//                    totalBytesRead = 0;
//                    putDataAt = dataBuf;
//                }
//
//                prevTick = packetTick;
//                prevSequence = sequence;
//                prevPacketLast = packetLast;
//
//                if (debug) fprintf(stderr, "Received %d data bytes from sender in packet #%d, last = %s, firstReadForBuf = %s\n",
//                                   nBytes, sequence, btoa(packetLast), btoa(firstReadForBuf));
//
//                // Check to see if packet is out-of-sequence
//                if (sequence != expectedSequence) {
//                    fprintf(stderr, "\n    Got seq %u, expecting %u\n", sequence, expectedSequence);
//
//                    // If we get one that we already received, ERROR!
//                    if (sequence < expectedSequence) {
//                        clearMap(outOfOrderPackets);
//                        fprintf(stderr, "getCompleteAllocatedBuffer: already got seq %u, id %hu, t %" PRIu64 "\n", sequence, packetDataId, packetTick);
//                        return OUT_OF_ORDER;
//                    }
//
//                    // Set a limit on how much we're going to store (200 packets) while we wait
//                    if (outOfOrderPackets.size() >= 200 || sizeof(outOfOrderPackets) >= outOfOrderPackets.max_size() ) {
//                        clearMap(outOfOrderPackets);
//                        fprintf(stderr, "getCompleteAllocatedBuffer: reached size limit of stored packets!\n");
//                        return OUT_OF_ORDER;
//                    }
//
//                    // Since it's out of order, what was written into dataBuf will need to be
//                    // copied and stored. And that written data will eventually need to be
//                    // overwritten with the correct packet data.
//                    char *tempBuf = (char *) malloc(nBytes);
//                    if (tempBuf == nullptr) {
//                        clearMap(outOfOrderPackets);
//                        fprintf(stderr, "getCompleteAllocatedBuffer: ran out of memory storing packets!\n");
//                        return OUT_OF_MEM;
//                    }
//                    memcpy(tempBuf, putDataAt, nBytes);
//
//                    // Put it into map
//                    if (debug) fprintf(stderr, "    Save and store packet %u, packetLast = %s\n", sequence, btoa(packetLast));
//                    outOfOrderPackets.emplace(sequence, std::tuple<char *, uint32_t, bool, bool>{tempBuf, nBytes, packetLast, packetFirst});
//                    // Read next packet
//                    continue;
//                }
//
//                while (true) {
//                    if (debug) fprintf(stderr, "Packet %u in proper order, last = %s\n", sequence, btoa(packetLast));
//
//                    // Packet was in proper order. Get ready to look for next in sequence.
//                    putDataAt += nBytes;
//                    remainingLen -= nBytes;
//                    totalBytesRead += nBytes;
//                    expectedSequence++;
//
//                    // If it's the first read of a sequence, and there are more reads to come,
//                    // the # of bytes it read will be max possible. Remember that.
//                    if (firstReadForBuf) {
//                        maxPacketBytes = nBytes;
//                        firstReadForBuf = false;
//                        //maxPacketsInBuf = bufLen / maxPacketBytes;
//                        if (debug) fprintf(stderr, "In first read, max bytes/packet = %lu\n", maxPacketBytes);
//
//                        // Error check
//                        if (!packetFirst) {
//                            fprintf(stderr, "getCompleteAllocatedBuffer: expecting first bit to be set on very first read but wasn't\n");
//                            clearMap(outOfOrderPackets);
//                            return BAD_FIRST_LAST_BIT;
//                        }
//                    }
//                    else if (packetFirst) {
//                        fprintf(stderr, "getCompleteAllocatedBuffer: expecting first bit NOT to be set on read but was\n");
//                        clearMap(outOfOrderPackets);
//                        return BAD_FIRST_LAST_BIT;
//                    }
//
//                    if (debug) fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s, OUTofOrder = %lu\n\n",
//                                       remainingLen, expectedSequence, btoa(packetFirst), btoa(packetLast),
//                                       outOfOrderPackets.size());
//
//                    // If no stored, out-of-order packets ...
//                    if (outOfOrderPackets.empty()) {
//                        // If very last packet, quit
//                        if (packetLast) {
//                            // Finish up some stats
//                            if (takeStats) {
//                                int64_t diff = 0;
//                                uint32_t droppedTicks = 0;
//                                if (knowExpectedTick) {
//                                    diff = packetTick - expectedTick;
//                                    droppedTicks = diff / tickPrescale;
//                                }
//
//                                // Total microsec to read buffer
//                                //                                stats->readTime += stats->endTime - stats->startTime;
//                                stats->acceptedBytes += totalBytesRead;
//                                stats->acceptedPackets += sequence + 1;
//                                //fprintf(stderr, "        accepted pkts = %llu, seq = %u\n", stats->acceptedPackets, sequence);
//                                stats->droppedTicks   += droppedTicks;
//                                // This works if all the buffers coming in are exactly the same size.
//                                // If they're not, then the sequence (# of packets - 1) of this buffer
//                                // is used to guess at how many packets were dropped for the dropped tick(s).
//                                stats->droppedPackets += droppedTicks * (sequence + 1);
//                                //if (droppedTicks != 0) printf("Dropped %u ticks, tick diff %" PRId64 ", packets = %" PRIu64 ", seq#s = %u\n",
//                                //                              droppedTicks, diff, stats->droppedPackets, (sequence + 1));
//                            }
//                            break;
//                        }
//                        if (remainingLen < 1) fprintf(stderr, "        remaining len = %zu\n", remainingLen);
//                    }
//                    // If there were previous packets out-of-order, they may now be in order.
//                    // If so, write them into buffer.
//                    // Remember the map already sorts them into proper sequence.
//                    else {
//                        if (debug) fprintf(stderr, "We also have stored packets\n");
//                        // Go to first stored packet
//                        auto it = outOfOrderPackets.begin();
//
//                        // If it's truly the next packet ...
//                        if (it->first == expectedSequence) {
//                            char *data  = std::get<0>(it->second);
//                            nBytes      = std::get<1>(it->second);
//                            packetLast  = std::get<2>(it->second);
//                            packetFirst = std::get<3>(it->second);
//                            sequence = expectedSequence;
//
//                            // Not enough room for this packet
//                            if (remainingLen < nBytes) {
//                                // double buffer size here
//                                bufLen *= 2;
//                                // realloc copies data over if necessary
//                                dataBuf = (char *)realloc(dataBuf, bufLen);
//                                if (dataBuf == nullptr) {
//                                    clearMap(outOfOrderPackets);
//                                    return OUT_OF_MEM;
//                                }
//                                putDataAt = dataBuf + totalBytesRead;
//                                remainingLen = bufLen - totalBytesRead;
//                                if (debug) fprintf(stderr, "getCompleteAllocatedBuffer: reallocated buffer to %zu bytes\n", bufLen);
//                            }
//
//                            memcpy(putDataAt, data, nBytes);
//                            free(data);
//
//                            // Remove packet from map
//                            it = outOfOrderPackets.erase(it);
//                            if (debug) fprintf(stderr, "Go and add stored packet %u, size of map = %lu, last = %s\n",
//                                               expectedSequence, outOfOrderPackets.size(), btoa(packetLast));
//                            continue;
//                        }
//                    }
//
//                    break;
//                }
//
//                veryFirstRead = false;
//
//                if (packetLast) {
//                    break;
//                }
//            }
//
//            *tick = packetTick;
//            if (dataId != nullptr) {
//                *dataId = packetDataId;
//            }
//            *bufLenPtr = bufLen;
//            *dataBufAlloc = dataBuf;
//            clearMap(outOfOrderPackets);
//            return totalBytesRead;
//        }
//
//
        /**
         * <p>
         * Assemble incoming packets into the given buffer - not necessarily the entirety of the data.
         * This routine is best for reading a very large buffer, or a file, for the purpose
         * of writing it on the receiving end - something too big to hold in RAM.
         * Transfer this packet by packet.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets. It also allows for multiple calls
         * to read the buffer in stages.
         * </p>
         *
         * Note, this routine does a poor job of error handling!
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
                                           bool debug, bool veryFirstRead, bool *last, /*bool useRecvFrom,*/
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
                    nBytes = readPacketRecvFrom(putDataAt, remainingLen, udpSocket,
                                                &packetTick, &sequence, &dataId, &version,
                                                &packetFirst, &packetLast, debug);


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


//        /**
//         * Assemble incoming packets into the given buffer or into an internally allocated buffer.
//         * Any internally allocated buffer is guaranteed to be big enough to hold the entire
//         * incoming buffer.
//         * It will return when the buffer has less space left than it read from the first packet
//         * (for caller-given buffer) or when the "last" bit is set in a packet.
//         * This routine allows for out-of-order packets.
//         *
//         * @param userBuf       address of pointer to data buffer if noCopy is true.
//         *                      Otherwise, this must point to nullptr in order
//         *                      to return a locally allocated data buffer.
//         *                      Note that in the latter case, the returned buffer must be freed by caller!
//         * @param userBufLen    pointer to byte length of given dataBuf if noCopy is true.
//         *                      Otherwise it should pointer to a suggested buffer size (0 for default of 100kB)
//         *                      and returns the size of the data buffer internally allocated.
//         * @param port          UDP port to read on.
//         * @param listeningAddr if specified, this is the IP address to listen on (dot-decimal form).
//         * @param noCopy        If true, write data directly into userBuf. If there's not enough room, an error is thrown.
//         *                      If false, an internal buffer is allocated and returned in the userBuf arg.
//         * @param debug         turn debug printout on & off.
//         *
//4         * @return 0 if success.
//         *         If there's an error in recvmsg, it will return RECV_MSG.
//         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
//         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
//         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
//         *              it will return OUT_OF_ORDER.
//         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
//         *         If cannot allocate memory, it will return OUT_OF_MEM.
//         *         If userBuf is null or *userBuf is null when noCopy is true, it will return BAD_ARG.
//         *         If on a read no data is returned when buffer not filled, return INTERNAL_ERROR.
//         *         If on a read &lt; HEADER_BYTES data returned, not enough data to contain header.
//         *              Then some sort of internal error and will return INTERNAL_ERROR.
//         */
//        static int getBuffer(char** userBuf, size_t *userBufLen,
//                             uint16_t port, const char *listeningAddr,
//                             bool noCopy, bool debug, bool useIPv6) {
//
//
//            if (userBuf == nullptr || userBufLen == nullptr) {
//                return BAD_ARG;
//            }
//
//            port = port < 1024 ? 7777 : port;
//            int err, udpSocket;
//
//            if (useIPv6) {
//
//                struct sockaddr_in6 serverAddr6{};
//
//                // Create IPv6 UDP socket
//                if ((udpSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
//                    perror("creating IPv6 client socket");
//                    return -1;
//                }
//
//                // Try to increase recv buf size to 25 MB
//                socklen_t size = sizeof(int);
//                int recvBufBytes = 25000000;
//                setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
//                recvBufBytes = 0; // clear it
//                getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
//                if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);
//
//                // Configure settings in address struct
//                // Clear it out
//                memset(&serverAddr6, 0, sizeof(serverAddr6));
//                // it is an INET address
//                serverAddr6.sin6_family = AF_INET6;
//                // the port we are going to receiver from, in network byte order
//                serverAddr6.sin6_port = htons(port);
//                if (listeningAddr != nullptr && strlen(listeningAddr) > 0) {
//                    inet_pton(AF_INET6, listeningAddr, &serverAddr6.sin6_addr);
//                }
//                else {
//                    serverAddr6.sin6_addr = in6addr_any;
//                }
//
//                // Bind socket with address struct
//                err = bind(udpSocket, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
//                if (err != 0) {
//                    // TODO: handle error properly
//                    if (debug) fprintf(stderr, "bind socket error\n");
//                }
//
//            } else {
//
//                struct sockaddr_in serverAddr{};
//
//                // Create UDP socket
//                if ((udpSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
//                    perror("creating IPv4 client socket");
//                    return -1;
//                }
//
//                // Try to increase recv buf size to 25 MB
//                socklen_t size = sizeof(int);
//                int recvBufBytes = 25000000;
//                setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
//                recvBufBytes = 0; // clear it
//                getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
//                if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);
//
//                // Configure settings in address struct
//                memset(&serverAddr, 0, sizeof(serverAddr));
//                serverAddr.sin_family = AF_INET;
//                serverAddr.sin_port = htons(port);
//                if (listeningAddr != nullptr && strlen(listeningAddr) > 0) {
//                    serverAddr.sin_addr.s_addr = inet_addr(listeningAddr);
//                }
//                else {
//                    serverAddr.sin_addr.s_addr = INADDR_ANY;
//                }
//                memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);
//
//                // Bind socket with address struct
//                err = bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
//                if (err != 0) {
//                    // TODO: handle error properly
//                    if (debug) fprintf(stderr, "bind socket error\n");
//                }
//
//            }
//
//            ssize_t nBytes;
//            // Start with sequence 0 in very first packet to be read
//            uint64_t tick = 0;
//
//            // Map to hold out-of-order packets.
//            // map key = sequence from incoming packet
//            // map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
//            // (is last packet), (is first packet).
//            std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;
//
//
//            if (noCopy) {
//                // If it's no-copy, we give the reading routine the user's whole buffer ONCE and have it filled.
//                // Write directly into user-specified buffer.
//                // In this case, the user knows how much data is coming and provides
//                // a buffer big enough to hold it all. If not, error.
//                if (*userBuf == nullptr) {
//                    return BAD_ARG;
//                }
//
//                nBytes = getCompletePacketizedBuffer(*userBuf, *userBufLen, udpSocket,
//                                                     debug, &tick, nullptr,
//                                                     nullptr, 1, outOfOrderPackets);
//                if (nBytes < 0) {
//                    if (debug) fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
//                    // Return the error
//                    clearMap(outOfOrderPackets);
//                    return nBytes;
//                }
//            }
//            else {
//                nBytes = getCompleteAllocatedBuffer(userBuf, userBufLen, udpSocket,
//                                                    debug, &tick, nullptr,
//                                                    nullptr, 1, outOfOrderPackets);
//                if (nBytes < 0) {
//                    if (debug) fprintf(stderr, "Error in getCompleteAllocatedBuffer, %ld\n", nBytes);
//                    // Return the error
//                    clearMap(outOfOrderPackets);
//                    return nBytes;
//                }
//            }
//
//            if (debug) fprintf(stderr, "Read %ld bytes from incoming reassembled packet\n", nBytes);
//            clearMap(outOfOrderPackets);
//            return 0;
//        }
//

}


#endif // EJFAT_ASSEMBLE_ERSAP_H
