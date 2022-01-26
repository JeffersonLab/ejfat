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

#include <iostream>

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const unsigned int max_pckt_sz = 1024;

int main(){
//===================== data source setup ===================================
    int udpSocket, nBytes;
    struct sockaddr_in srcAddr;
    struct sockaddr_storage srcRcvBuf;
    socklen_t addr_size;

    /*Create UDP socket for reception from sender */
    udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

    /*Configure settings in address struct*/
    srcAddr.sin_family = AF_INET;
    srcAddr.sin_port = htons(0x4c42); // "LB"
    srcAddr.sin_addr.s_addr = inet_addr("129.57.29.231"); //indra-s2
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
    snkAddr.sin_port = htons(7777); // data consumer port to send to
    snkAddr.sin_addr.s_addr = inet_addr("129.57.29.232"); // indra-s3 as data consumer
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
    union lb* plbmd = &lbmd;
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
    union re* premd = (union re*)&buffer[lblen];
    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        // locate ingress data after lb+re meta data regions
        nBytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&srcRcvBuf, &addr_size);

        //printf("Received %i bytes from source\n", nBytes);
        std::cerr << "Received "<< nBytes << " bytes from source for seq # " << premd->remdbf.seq << '\n';

        printf("Sending %i bytes to sink\n", int(nBytes-lblen));
        
        // forward data to sink skipping past lb meta data

        ssize_t rtCd = sendto(clientSocket, &buffer[lblen], nBytes-lblen, 0, (struct sockaddr *)&snkAddr, addr_size);
        printf("sendto return code = %d\n", int(rtCd));

    }

    return 0;
}

