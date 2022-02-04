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





#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <getopt.h>
#include <cinttypes>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifdef __APPLE__
#include <cctype>
#endif

#define INPUT_LENGTH_MAX 256


#define LB_HEADER_BYTES 12
#define HEADER_BYTES    20
#define RE_HEADER_BYTES  8

#define btoa(x) ((x)?"true":"false")


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

    uint32_t reWords[2];
};


/** Union to facilitate unpacking of RE UDP header. */
union lbHeader {
    struct __attribute__((packed))lb_hdr {
        uint32_t l       : 8;
        uint32_t b       : 8;
        uint32_t version : 8;
        uint32_t data_id : 8;
        uint64_t tick    : 64;
    } lbFields;

    uint32_t lbWords[3];
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


static void setLbMetadata(char* buffer, uint64_t tick, int version, int protocol) {
    // Put 1 32-bit word, followed by 1 64-bit word in network byte order, followed by 32 bits of data

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

    //if (debug) printf("LB first word = 0x%x\n", firstWord);
    //if (debug) printf("LB tick = 0x%llx (%llu)\n\n", tick, tick);

    // Put the data in network byte order (big endian)
    *buffer     = 'L';
    *(buffer+1) = 'B';
    *(buffer+2) = version;
    *(buffer+3) = protocol;
    *((uint64_t *)(buffer + 4)) = htonll(tick);
}



/**
 * <p>Set the Reassembly Header data.
 * The first 16 bits go as ordered. The dataId is put in network byte order.
 * The offset is also put into network byte order.</p>
 * Implemented using C++ bit fields.
 *
 * @param buffer  buffer in which to write the header.
 * @param first   is this the first packet?
 * @param last    is this the last packet?
 * @param offset  the packet sequence number.
 * @param version the version of this software.
 * @param dataId  the data source id number.
 */
static void setReMetadataBitField(char* buffer, bool first, bool last,
                                  uint32_t offset, int version, uint16_t dataId) {

    // protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |Version|        Rsvd       |F|L|            Data-ID            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                  UDP Packet Offset                            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    union reHeader* header = (union reHeader*)buffer;

    header->reFields.version = version;
    header->reFields.first = first;
    header->reFields.last = last;
    header->reFields.data_id = htons(dataId);
    header->reFields.sequence = htonl(offset);
}


/**
 * <p>Set the Reassembly Header data.
 * The first 16 bits go as ordered. The dataId is put in network byte order.
 * The offset is also put into network byte order.</p>
 * Implemented <b>without</b> using C++ bit fields.
 *
 * @param buffer  buffer in which to write the header.
 * @param first   is this the first packet?
 * @param last    is this the last packet?
 * @param offset  the packet sequence number.
 * @param version the version of this software.
 * @param dataId  the data source id number.
 */
static void setReMetadata(char* buffer, bool first, bool last,
                          uint32_t offset, int version, uint16_t dataId) {

    // protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |Version|        Rsvd       |F|L|            Data-ID            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                  UDP Packet Offset                            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    uint16_t firstShort = (version & 0x1f) | first << 14 | last << 15;
    // When sending, we need to ensure that the LSB (version) goes first.
    // Transform to big endian, then swap.
    firstShort = bswap_16(htons(firstShort));
    dataId = htons(dataId);
    offset = htonl(offset);

    *((uint16_t *)buffer) = firstShort;
    *((uint16_t *)(buffer + 2)) = dataId;
    *((uint32_t *)(buffer + 4)) = offset;
}


static void parseReHeader(char* buffer, int* version,
                          bool *first, bool *last,
                          uint16_t* dataId, uint32_t* sequence)
{
    // version (LSB) is first in buffer, last bit is last
    uint16_t s = *((uint16_t *) buffer);

    // If this is a little endian machine, we're good.
    // If this is a big endian machine, we need to swap.
    // If we call htons on a big endian machine it does nothing,
    // then calling bswap_16 will make it little endian.
    // If we call htons on a little endian machine, it makes things big endian,
    // then calling bswap_16 makes it little again.
    s = bswap_16(htons(s));

    // Now pull out the component values
    *version = s & 0x1f;
    *first   = (s >> 14) & 1;
    *last    = (s >> 15) & 1;

    *dataId   = ntohs(*((uint16_t *) (buffer + 2)));
    *sequence = ntohl(*((uint32_t *) (buffer + 4)));
}

static void parseReHeaderBitField(char* buffer, int* version,
                                  bool *first, bool *last,
                                  uint16_t* dataId, uint32_t* offset)
{
    union reHeader* header = (union reHeader*)buffer;

    // Make sure first short is read as little endian
    uint16_t *s = (uint16_t *) buffer;
    *s = bswap_16(htons(*s));

    // Parse
    *version = header->reFields.version;
    *first   = header->reFields.first;
    *last    = header->reFields.last;
    *dataId  = ntohs(header->reFields.data_id);
    *offset  = ntohl(header->reFields.sequence);
}



int main(int argc, char **argv) {

    uint32_t offset = 4;
    uint64_t tick = 0xc0da;
    int version = 2;
    uint16_t dataId = 3;
    bool first = true;
    bool last  = true;

    char buffer[RE_HEADER_BYTES];

    // Write data into RE header in network byte order
//    setReMetadataBitField(buffer, first, last, offset, version, dataId);
    setReMetadata(buffer, first, last, offset, version, dataId);

    printBytes(buffer, RE_HEADER_BYTES, "Written (network order) bytes");

//    parseReHeaderBitField(buffer, &version, &first, &last, &dataId, &offset);
//    printf("From parseReHeaderBitField:  dataId = %hu, version = %d, first = %s, last = %s, seq = %u\n",
//           dataId, version, btoa(first), btoa(last), offset);

    parseReHeader(buffer, &version, &first, &last, &dataId, &offset);
    printf("From parseReHeader:  dataId = %hu, version = %d, first = %s, last = %s, seq = %u\n",
           dataId, version, btoa(first), btoa(last), offset);


    return 0;
}
