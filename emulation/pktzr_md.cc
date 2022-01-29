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

using namespace std;

const unsigned int max_pckt_sz = 1024;

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
    uint16_t tick         = 1;      // LB tick
    uint16_t data_id      = 1;      // RE data_id
    uint16_t num_data_ids = 1;      // number of data_ids starting from initial

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
            tick = (uint16_t) atoi((const char *) optarg) ;
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
    lbmd.lbmdbf = {'L','B',1,1,tick};
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
    remd.remdbf = {1,0,1,0,data_id,0};
    size_t relen =  sizeof(union re);
    size_t mdlen =  lblen + relen;
    char buffer[mdlen + max_pckt_sz];
    memmove(buffer, &lbmd, lblen);
    memmove(&buffer[lblen], &remd, relen);
    union lb* plbmd = (union lb*)buffer;
    union re* premd = (union re*)&buffer[lblen];
    premd->remdbf.frst = 1;
    premd->remdbf.lst  = 0;
    premd->remdbf.seq  = 0;
    do {
        premd->remdbf.seq++;
        f1.read((char*)&buffer[mdlen], max_pckt_sz);
        streamsize nr = f1.gcount();
        cerr << "Num read from stdin: " << nr << endl;
        if(nr != max_pckt_sz) premd->remdbf.lst  = 1;

        // forward data to LB
        for(unsigned int didcnt = 0; didcnt < num_data_ids; didcnt++) {
            premd->remdbf.data_id = data_id + didcnt;
            cerr << "Sending " << int(mdlen + nr) << " bytes to LB" << '\n';
            cerr << "l = " << char(plbmd->lbmdbf.l) << " / b = " << char(plbmd->lbmdbf.b) 
                 << " / tick = " << plbmd->lbmdbf.tick << '\n';	
            cerr << "frst = " << premd->remdbf.frst << " / lst = " << premd->remdbf.lst 
                 << " / data_id = " << premd->remdbf.data_id << " / seq = " << premd->remdbf.seq << '\n';	

            ssize_t rtCd = sendto(clientSocket, buffer, mdlen + nr, 0, (struct sockaddr *)&snkAddr, addr_size);
            cerr << "sendto return code = " << int(rtCd) << endl;
        }
        premd->remdbf.frst = 0;
    } while(premd->remdbf.lst == 0);

    return 0;
}
