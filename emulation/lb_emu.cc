/************* Load Balancer Emulation  *******************/

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <string.h>

#include <fstream>
#include <iostream>

using namespace std;

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const unsigned int max_pckt_sz = 1024;
const size_t lblen = 12;
const size_t relen = 8;
const size_t mdlen =  lblen + relen;


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
    bind(udpSocket, (struct sockaddr *) &srcAddr, sizeof(srcAddr));

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

//=======================================================================

    uint8_t buffer[mdlen + max_pckt_sz];

    uint8_t* pBuf   = buffer;
    uint8_t* pBufLb = buffer;
    uint8_t* pBufRe = &buffer[lblen];

    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        // locate ingress data after lb+re meta data regions
        nBytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&srcRcvBuf, &addr_size);

        // decode to little endian
        uint32_t seq     = pBufRe[4]*0x1000000 + pBufRe[5]*0x10000 + pBufRe[6]*0x100 + pBufRe[7];
        uint16_t data_id = pBufRe[2]*0x100 + pBufRe[3];
        uint8_t vrsn     = (pBufRe[0] & 0xf0) >> 4;
        uint8_t frst     = (pBufRe[1] & 0x02) >> 1;
        uint8_t lst      = pBufRe[1] & 0x01;

        //printf("Received %i bytes from source\n", nBytes);
        fprintf( stderr, "\n data_id = %d ", data_id);
        fprintf( stderr, "Received %d bytes from source for seq # %d\n", nBytes, seq );
        fprintf( stderr, "l = %c  / b = %c ", pBufLb[0], pBufLb[1]); 
            /* << " / tick = " << NTOHLL(plbmd->lbmdbf.tick)  << '\n';	*/
        fprintf( stderr, "frst = %d / lst = %d ", frst, lst); 
        fprintf( stderr, " / data_id = %d / seq = %d\n", data_id, seq);	

        
        // forward data to sink skipping past lb meta data
/**
        unsigned int tick = NTOHLL(plbmd->lbmdbf.tick);
        if(tick != 0) continue;
**/
        cerr << "Sending " << int(nBytes-lblen) << " bytes to sink" << '\n';
        ssize_t rtCd = sendto(clientSocket, &buffer[lblen], nBytes-lblen, 0, (struct sockaddr *)&snkAddr, addr_size);
        cerr << "sendto return code = " << int(rtCd) << '\n';

    }

    return 0;
}
