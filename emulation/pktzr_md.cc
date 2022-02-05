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
const size_t relen       = 8;
const size_t mdlen       = lblen + relen;

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -i destination ipv4 address (string)  \n\
        -p destination ipv4 port (number)  \n\
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

    bool passedI = false;

    char     lb_ip[64];             // LB target ip
    uint16_t lb_prt       = 0x4c42; // target LB port
    uint64_t tick         = 1;      // LB tick
    uint16_t data_id      = 1;      // RE data_id
    uint16_t num_data_ids = 1;      // number of data_ids starting from initial
    const uint8_t vrsn    = 1;
    const uint16_t rsrvd  = 2; // = 2 just for testing
    uint8_t frst          = 1;
    uint8_t lst           = 0;
    uint32_t seq          = 0;

    while ((optc = getopt(argc, argv, "i:p:t:d:n:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'i':
            strcpy(lb_ip, (const char *) optarg) ;
            passedI = true;
            break;
        case 'p':
            lb_prt = (uint16_t) atoi((const char *) optarg) ;
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

    if(!passedI) { Usage(); exit(1); }

    ifstream f1("/dev/stdin", std::ios::binary | std::ios::in);

//===================== data sink setup ===================================

    int clientSocket;
    uint32_t counter = 0;
    uint16_t port = 0x4c42;  // LB recv port
    struct sockaddr_in snkAddr;

    // Create UDP socket for transmission to sender
    clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    snkAddr.sin_family = AF_INET;
    snkAddr.sin_port = htons(lb_prt); // data consumer port to send to
    snkAddr.sin_addr.s_addr = inet_addr(lb_ip); // indra-s3 as data consumer
    memset(snkAddr.sin_zero, '\0', sizeof snkAddr.sin_zero);

    // Initialize size variable to be used later on
    socklen_t addr_size = sizeof snkAddr;

    uint8_t buffer[mdlen + max_pckt_sz];

    uint8_t* pBuf   = buffer;
    uint8_t* pBufLb = buffer;
    uint8_t* pBufRe = &buffer[lblen];
    uint64_t* pTick = (uint64_t*) &buffer[lblen-sizeof(uint64_t)];
    uint32_t* pSeq  = (uint32_t*) &buffer[mdlen-sizeof(uint32_t)];
    uint16_t* pDid  = (uint16_t*) &buffer[mdlen-sizeof(uint32_t)-sizeof(uint16_t)];

    // meta-data in network order

    pBufLb[0] = 'L';
    pBufLb[1] = 'B';
    pBufLb[2] = 1;
    pBufLb[3] = 1;
    *pTick    = HTONLL(tick);

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
            fprintf( stderr, " / data_id = %d / seq = %d\n", data_id + didcnt, seq);	

            ssize_t rtCd = sendto(clientSocket, buffer, mdlen + nr, 0, (struct sockaddr *)&snkAddr, addr_size);
            cerr << "sendto return code = " << int(rtCd) << endl;
        }
        frst = 0;
        pBufRe[1] = (rsrvd << 2) + (frst << 1) + lst;
        *pSeq = htonl(++seq);
    } while(!lst);
    return 0;
}
