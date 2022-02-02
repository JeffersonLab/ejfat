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

	// prepare LB meta-data
    // LB meta-data header on front of payload
    union lb {
        struct __attribute__((packed)) lb_hdr {
            unsigned int l    : 8;
            unsigned int b    : 8;
            unsigned int vrsn : 8;
            unsigned int ptcl : 8;
            unsigned long int tick : 64;
        } lbmdbf;
        unsigned int lbmduia [3];
    } lbmd;
    size_t lblen =  sizeof(union lb);
	// prepare RE meta-data
    // RE meta-data header on front of payload
    union re {
        struct __attribute__((packed))re_hdr {
            unsigned int vrsn    : 4;
            unsigned int rsrvd   : 10;
            unsigned int frst    : 1;
            unsigned int lst     : 1;
            unsigned int data_id : 16;
            unsigned int seq     : 32;
        } remdbf;
        unsigned int remduia[2];
    } remd;
    size_t relen =  sizeof(union re);
    size_t mdlen =  lblen + relen;
    char buffer[max_pckt_sz + mdlen];
    union lb* plbmd = (union lb*)&buffer[0];
    union re* premd = (union re*)&buffer[lblen];
    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        // locate ingress data after lb+re meta data regions
        nBytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&srcRcvBuf, &addr_size);
        // convert LB meta-data to host order
        plbmd->lbmduia[0] = ntohl(plbmd->lbmduia[0]);
        plbmd->lbmdbf.tick = NTOHLL(plbmd->lbmdbf.tick);
        // convert RE meta-data to host order
        premd->remduia[0] = ntohl(premd->remduia[0]);
        premd->remduia[1] = ntohl(premd->remduia[1]);

        //printf("Received %i bytes from source\n", nBytes);
        cerr << "Received "<< nBytes << " bytes from source for seq # " << premd->remdbf.seq << '\n';
        cerr << "l = " << char(plbmd->lbmdbf.l) << " / b = " << char(plbmd->lbmdbf.b) 
            << " / tick = " << plbmd->lbmdbf.tick << '\n';	
        cerr << "frst = " << premd->remdbf.frst << " / lst = " << premd->remdbf.lst 
            << " / data_id = " << premd->remdbf.data_id << " / seq = " << premd->remdbf.seq << '\n';	

        cerr << "Sending " << int(nBytes-lblen) << " bytes to sink" << '\n';
        
        // forward data to sink skipping past lb meta data

        ssize_t rtCd = sendto(clientSocket, &buffer[lblen], nBytes-lblen, 0, (struct sockaddr *)&snkAddr, addr_size);
        cerr << "sendto return code = " << int(rtCd) << '\n';

    }

    return 0;
}
