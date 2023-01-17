/************* Load Balancer Emulation  *******************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <arpa/inet.h>
#include <string.h>

#include <iostream>

using std::cerr;
using std::cout;

#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


#ifdef __APPLE__
#include <ctype.h>
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


const unsigned int max_pckt_sz = 1024;


#define LB_HEADER_BYTES 12
#define    HEADER_BYTES 20



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



static void parseLbHeader(char* buffer, char* ll, char* bb,
                          uint32_t* version, uint32_t* protocol,
                          uint64_t* tick)
{
    *ll = buffer[0];
    *bb = buffer[1];
    *version  = (uint32_t)(buffer[2]) & 0xff;
    *protocol = (uint32_t)(buffer[3]) & 0xff;
    *tick     = ntohll(*((uint64_t *)(&buffer[4])));
}


static void parseReHeader(char* buffer, uint32_t* version,
                          bool *first, bool *last,
                          uint16_t* dataId, uint32_t* sequence)
{
    // version (LSB) is first in buffer, last bit is last
    uint16_t s = *((uint16_t *) buffer);

    // If this is a little endian machine, we're good.
    // If this is a big endian machine, we need to swap.
    // If we call ntohs on a big endian machine it does nothing,
    // then calling bswap_16 will make it little endian.
    // If we call ntohs on a little endian machine, it swaps to make things big endian,
    // then calling bswap_16 makes it little again.
    s = bswap_16(ntohs(s));

    // Now pull out the component values
    *version = s & 0x1f;
    *first   = (s >> 14) & 1;
    *last    = (s >> 15) & 1;

    *dataId   = ntohs(*((uint16_t *) (buffer + 2)));
    *sequence = ntohl(*((uint32_t *) (buffer + 4)));
}


void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -i listening ipv4 address (string)  \n\
        -p listening ipv4 port (number)  \n\
        -t destination ipv4 address (string)  \n\
        -r destination ipv4 port (number)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -s\n";
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI, passedP, passedT, passedR  = false;

    char in_ip[64], out_ip[64]; // listening, target ip
    uint16_t in_prt, out_prt;   // listening, target ports

    while ((optc = getopt(argc, argv, "i:p:t:r:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'i':
            strcpy(in_ip, (const char *) optarg) ;
            passedI = true;
            break;
        case 'p':
            in_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            break;
        case 't':
            strcpy(out_ip, (const char *) optarg) ;
            passedT = true;
            break;
        case 'r':
            out_prt = (uint16_t) atoi((const char *) optarg) ;
            passedR = true;
            break;
        case '?':
            cerr<<"Unrecognised option: -"<<optopt<<'\n';
            Usage();
            exit(1);
        }
    }

    if(!(passedI && passedP && passedT && passedR)) { Usage(); exit(1); }

//===================== data source setup ===================================
    int udpSocket, nBytes;
    struct sockaddr_in srcAddr;
    struct sockaddr_storage srcRcvBuf;
    socklen_t addr_size;

    /*Create UDP socket for reception from sender */
    udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

    /*Configure settings in address struct*/
    srcAddr.sin_family = AF_INET;
    srcAddr.sin_port = htons(in_prt);           // "LB" = 0x4c42 by spec
    srcAddr.sin_addr.s_addr = inet_addr(in_ip); // LB address
    //srcAddr.sin_addr.s_addr = INADDR_ANY;
    memset(srcAddr.sin_zero, '\0', sizeof srcAddr.sin_zero);

    /*Bind socket with address struct*/
    int err = bind(udpSocket, (struct sockaddr *) &srcAddr, sizeof(srcAddr));
    if (err < 0) {
        perror("ERROR in bind:");
        return -1;
    }

    /*Initialize size variable to be used later on*/
    addr_size = sizeof srcRcvBuf;

//===================== data sink setup ===================================

    int clientSocket;
    uint32_t counter = 0;
    uint16_t port = 0x4c42;  // LB recv port
    struct sockaddr_in snkAddr;

    // Create UDP socket for transmission to sender
    clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    snkAddr.sin_family = AF_INET;
    snkAddr.sin_port = htons(out_prt);           // Data Sink
    snkAddr.sin_addr.s_addr = inet_addr(out_ip); // Data Sink
    memset(snkAddr.sin_zero, '\0', sizeof snkAddr.sin_zero);

    // Initialize size variable to be used later on
    addr_size = sizeof snkAddr;

    char buffer[max_pckt_sz + HEADER_BYTES];

    bool first, last;
    uint16_t dataId;
    uint32_t seq;

    char ll, bb;
    uint32_t version, protocol;
    uint64_t tick;

    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        // locate ingress data after lb+re meta data regions
        nBytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&srcRcvBuf, &addr_size);

        //printBytes(buffer, HEADER_BYTES, "Header bytes:");

        parseLbHeader(buffer, &ll, &bb, &version, &protocol, &tick);
        parseReHeader(buffer + LB_HEADER_BYTES, &version, &first, &last, &dataId, &seq);

        //printf("Received %i bytes from source\n", nBytes);
        cerr << "Received "<< nBytes << " bytes for seq # " << seq << '\n';
        cerr << "l = " << char(ll) << " / b = " << char(bb) << " / ver = " << version << " / pro = " << protocol
            << " / tick = " << tick << '\n';
        cerr << "frst = " << first << " / lst = " << last
            << " / data_id = " << dataId << " / seq = " << seq << '\n';

        cerr << "Sending " << int(nBytes-LB_HEADER_BYTES) << " bytes to sink" << '\n';
        
        // forward data to sink skipping past lb meta data

        ssize_t rtCd = sendto(clientSocket, &buffer[LB_HEADER_BYTES],
                              nBytes-LB_HEADER_BYTES, 0, (struct sockaddr *)&snkAddr, addr_size);
        cerr << "sendto return code = " << int(rtCd) << '\n';

    }

    return 0;
}
