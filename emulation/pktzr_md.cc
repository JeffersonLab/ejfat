/************* Loab Balancer Packetizer / Sender  *******************/
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
#include <inttypes.h>

using namespace std;

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

const size_t max_pckt_sz = 1024;
const size_t lblen       = 12;
const size_t relen       = 8+8;
const size_t mdlen       = lblen + relen;

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i destination address (string)  \n\
        -p destination port (number)  \n\
        -t tick  \n\
        -d data_id  \n\
        -n num_data_ids starting from initial  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i\n";
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI=false, passedP=false, passed6=false;

    char     dst_ip[INET6_ADDRSTRLEN];  // target ip
    uint16_t dst_prt = 0x4c42;          // target port

    uint64_t tick         = 1;      // LB tick
    uint16_t data_id      = 1;      // RE data_id
    uint16_t num_data_ids = 1;      // number of data_ids starting from initial
    const uint8_t vrsn    = 1;
    const uint16_t rsrvd  = 2; // = 2 just for testing
    uint8_t frst          = 1;
    uint8_t lst           = 0;
    uint32_t seq          = 0;

    while ((optc = getopt(argc, argv, "i:p:t:d:n:6")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case '6':
            passed6 = true;
            break;
        case 'i':
            strcpy(dst_ip, (const char *) optarg) ;
            passedI = true;
            break;
        case 'p':
            dst_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            break;
        case 't':
            tick = (uint64_t) atoi((const char *) optarg) ;
            break;
        case 'd':
            data_id = (uint16_t) atoi((const char *) optarg) ;
            break;
        case 'n':
            num_data_ids = (uint16_t) atoi((const char *) optarg) ;
            break;
        case '?':
            cerr<<"Unrecognised option: -"<<optopt<<'\n';
            Usage();
            exit(1);
        }
    }

    if(!(passedI && passedP)) { Usage(); exit(1); }

    ifstream f1("/dev/stdin", std::ios::binary | std::ios::in);

//===================== data destination setup ===================================
    int dst_sckt;
    struct sockaddr_in6 dst_addr6;
    struct sockaddr_in dst_addr;

    if (passed6) {

        /* create a DGRAM (UDP) socket in the INET/INET6 protocol */
        if ((dst_sckt = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
            perror("creating dst socket");
            exit(1);
        }

        // Configure settings in address struct
        /*Configure settings in address struct*/
        /* clear it out */
        memset(&dst_addr6, 0, sizeof(dst_addr6));
        /* it is an INET address */
        dst_addr6.sin6_family = AF_INET6; 
        /* the port we are going to send to, in network byte order */
        dst_addr6.sin6_port = htons(dst_prt);           // "LB" = 0x4c42 by spec (network order)
        /* the server IP address, in network byte order */
        inet_pton(AF_INET6, dst_ip, &dst_addr6.sin6_addr);  // LB address

    } else {

        // Create UDP socket for transmission to sender
        dst_sckt = socket(PF_INET, SOCK_DGRAM, 0);

        // Configure settings in address struct
        dst_addr.sin_family = AF_INET;
        dst_addr.sin_port = htons(dst_prt); // data consumer port to send to
        dst_addr.sin_addr.s_addr = inet_addr(dst_ip); // indra-s3 as data consumer
        memset(dst_addr.sin_zero, '\0', sizeof dst_addr.sin_zero);

        // Initialize size variable to be used later on
        socklen_t addr_size = sizeof dst_addr;
    }
//=======================================================================
    inet_pton(AF_INET6, dst_ip, &dst_addr6.sin6_addr);  // LB address

    uint8_t buffer[mdlen + max_pckt_sz];

    uint8_t*  pBufLb =  buffer;
    uint8_t*  pBufRe = &buffer[lblen];
    uint64_t* pTick   = (uint64_t*) &buffer[lblen-sizeof(uint64_t)];
    uint64_t* pReTick = (uint64_t*) &buffer[mdlen-sizeof(uint64_t)];
    uint32_t* pSeq    = (uint32_t*) &buffer[mdlen-sizeof(uint64_t)-sizeof(uint32_t)];
    uint16_t* pDid    = (uint16_t*) &buffer[mdlen-sizeof(uint64_t)-sizeof(uint32_t)-sizeof(uint16_t)];
    // meta-data in network order

    pBufLb[0] = 'L'; // 0x4c
    pBufLb[1] = 'B'; //0x42
    pBufLb[2] = 1;   //version
    pBufLb[3] = 1;   //protocol
    *pTick    = HTONLL(tick);
    *pReTick  = HTONLL(tick);

    pBufRe[1] = (rsrvd << 2) + (frst << 1) + lst;
    *pDid     = htons(data_id);
    *pSeq     = htonl(seq);

    do {
        f1.read((char*)&buffer[mdlen], max_pckt_sz);
        streamsize nr = f1.gcount();
        cerr << "Num read from stdin: " << nr << endl;
        if(nr != max_pckt_sz) {
            lst  = 1;
            pBufRe[1] = (rsrvd << 2) + (frst << 1) + lst;
        }

        // forward data to LB
        for(uint16_t didcnt = 0; didcnt < num_data_ids; didcnt++) {

            *pDid = htons(data_id + didcnt);

            fprintf( stderr, "LB Meta-data on the wire:");
            for(uint8_t b = 0; b < lblen; b++) fprintf( stderr, " [%d] = %x ", b, pBufLb[b]);
            fprintf( stderr, "\nfor tick = %" PRIu64 " ", *pTick);
            fprintf( stderr, "tick = %" PRIx64 " ", *pTick);
            fprintf( stderr, "for tick = %" PRIu64 "\n", tick);
            fprintf( stderr, "RE Meta-data on the wire:");
            for(uint8_t b = 0; b < relen; b++) fprintf( stderr, " [%d] = %x ", b, pBufRe[b]);
            fprintf( stderr, "\nfor frst = %d / lst = %d ", frst, lst); 
            fprintf( stderr, " / data_id = %d / seq = %d ", data_id + didcnt, seq);	
            fprintf( stderr, "tick = %" PRIu64 "\n", tick);

            ssize_t rtCd = 0;
            /* now send a datagram */
if (passed6) {
            if ((rtCd = sendto(dst_sckt, buffer, mdlen + nr, 0, 
                        (struct sockaddr *)&dst_addr6, sizeof dst_addr6)) < 0) {
                perror("sendto failed");
                exit(4);
            }
} else {
            if ((rtCd = sendto(dst_sckt, buffer, mdlen + nr, 0, 
                        (struct sockaddr *)&dst_addr, sizeof dst_addr)) < 0) {
                perror("sendto failed");
                exit(4);
            }
}
            fprintf( stderr, "Sending %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt);

        }
        frst = 0;
        pBufRe[1] = (rsrvd << 2) + (frst << 1) + lst;
        *pSeq = htonl(++seq);
    } while(!lst);
    return 0;
}
