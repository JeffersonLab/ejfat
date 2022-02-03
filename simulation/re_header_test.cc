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



static void setReMetadata(char* buffer, bool first, bool last, bool debug,
                          uint32_t offsetVal, int version, int dataId) {
    // Put 2 32-bit words in network byte order

    // protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |Version|        Rsvd       |F|L|            Data-ID            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                  UDP Packet Offset                            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    int firstWord = (version & 0x1F) | (first << 14) | (last << 15) | (dataId << 16);

    if (debug) printf("RE first word = 0x%x\n", firstWord);
    if (debug) printf("RE offset = %u\n", offsetVal);

    // Put the data in network byte order (big endian)
    *((uint32_t *)buffer) = htonl(firstWord);
    *((uint32_t *)(buffer + 4)) = htonl(offsetVal);
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



int main(int argc, char **argv) {

    uint32_t offset = 4;
    uint64_t tick = 0xc0da;
    int mtu, version = 2, dataId = 3;
    bool first = true;
    bool last  = true;
    bool debug = true;

    char buffer[RE_HEADER_BYTES];

    // Write data into RE header in network byte order
    setReMetadata(buffer, first, last, debug, offset, version, dataId);

    printBytes(buffer, RE_HEADER_BYTES, "Written (network order) bytes");

    union reHeader* header = (union reHeader*)buffer;

    // Swap to local byte order
    header->reWords[0] = ntohl(header->reWords[0]);
    header->reWords[1] = ntohl(header->reWords[1]);

    printBytes(buffer, RE_HEADER_BYTES, "Written (local order) bytes");

    // Parse
        dataId  = header->reFields.data_id;
        version = header->reFields.version;
        first   = header->reFields.first;
        last    = header->reFields.last;
        offset  = header->reFields.sequence;

    printf("dataId = %u, version = %d, first = %s, last = %s, seq = %u\n",
           dataId, version, btoa(first), btoa(last), offset);


    return 0;
}
