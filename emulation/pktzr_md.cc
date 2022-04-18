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

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i destination address (string)  \n\
        -p destination port (number)  \n\
        -t lb_tick  \n\
        -d re_data_id  \n\
        -v verbose mode (default is quiet)  \n\
        -s max packet size (default 9000)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i\n";
}

int main (int argc, char *argv[])
{
    const size_t max_pckt_sz = 9000-20-8;  // = MTU - IP header - UDP header
    const size_t lblen       = 16;
    const size_t relen       = 16;
    const size_t mdlen       = lblen + relen;

    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI=false, passedP=false, passed6=false, passedV=false;

    char     dst_ip[INET6_ADDRSTRLEN];  // target ip
    uint16_t dst_prt = 0x4c42;          // target port

    const uint8_t lb_vrsn    = 1;
    const uint8_t lb_prtcl   = 1;
    const uint16_t lb_rsrvd  = 0;
    uint64_t lb_tick         = 1;      // LB tick
    const uint8_t re_vrsn    = 1;
    const uint16_t re_rsrvd  = 0;
    uint8_t re_frst          = 1;
    uint8_t re_lst           = 0;
    uint16_t re_data_id      = 1;      // RE data_id
    uint32_t re_seq          = 0;

    size_t pckt_sz = max_pckt_sz;

    while ((optc = getopt(argc, argv, "i:p:t:d:n:6vs:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case '6':
            passed6 = true;
            fprintf(stdout, "-6 ");
            break;
        case 'i':
            strcpy(dst_ip, (const char *) optarg) ;
            passedI = true;
            fprintf(stdout, "-i ");
            break;
        case 'p':
            dst_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            fprintf(stdout, "-p ");
            break;
        case 't':
            lb_tick = (uint64_t) atoi((const char *) optarg) ;
            fprintf(stdout, "-t ");
            break;
        case 'd':
            re_data_id = (uint16_t) atoi((const char *) optarg) ;
            fprintf(stdout, "-d %d ", re_data_id);
            break;
        case 's':
            pckt_sz = (size_t) atoi((const char *) optarg) -20-8;  // = MTU - IP header - UDP header
            pckt_sz = min(pckt_sz,max_pckt_sz);
            fprintf(stdout, "-s %d ", pckt_sz);
            break;
        case 'v':
            passedV = true;
            fprintf(stdout, "-v ");
            break;
        case '?':
            cout <<"Unrecognised option: -"<<optopt<<'\n';
            Usage();
            exit(1);
        }
        fprintf(stdout, "%s ", optarg);
    }
    fprintf(stdout, "\n");
    if(!(passedI && passedP)) { Usage(); exit(1); }

    ifstream f1("/dev/stdin", std::ios::binary | std::ios::in);
    size_t num_to_read = pckt_sz-mdlen; // max bytes to read reserving space for metadata

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
    // set the don't fragment bit
    {
        int val = IP_PMTUDISC_DO;
        setsockopt(dst_sckt, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
    }
//=======================================================================
    inet_pton(AF_INET6, dst_ip, &dst_addr6.sin6_addr);  // LB address

    uint8_t buffer[pckt_sz];

    // meta-data in network order
	// LB metadata
    uint8_t*  pBufLb =  buffer;
    pBufLb[0] = 'L'; // 0x4c
    pBufLb[1] = 'B'; //0x42
    pBufLb[2] = lb_vrsn;   //version
    pBufLb[3] = lb_prtcl;  //protocol
    pBufLb[4] = 0;   //reserved
    pBufLb[5] = 0;   //reserved
    uint16_t* pEntrp  = (uint16_t*) &pBufLb[6];
    uint64_t* pTick   = (uint64_t*) &pBufLb[8];
    *pTick    = HTONLL(lb_tick);
	// RE metadata
    uint8_t*  pBufRe = &buffer[lblen];
    pBufRe[0] = 0x10; //(re_vrsn & 0xf) + (re_rsrvd & 0x3f0) >> 4;
    pBufRe[1] = 0x2; //(re_rsrvd  & 0x3f) << 2 + (re_frst << 1) + re_lst;
    uint16_t* pDid   = (uint16_t*) &pBufRe[2];
    uint32_t* pSeq   = (uint32_t*) &pBufRe[4];
    uint64_t* pReTick = (uint64_t*) &pBufLb[8];
    *pDid     = htons(re_data_id);
    *pSeq     = htonl(re_seq);
    *pReTick  = *pTick;

    do {
        f1.read((char*)&buffer[mdlen], num_to_read);
        streamsize nr = f1.gcount();
        if(passedV) cout  << "\nNum read from stdin: " << nr << endl;
        if(nr != num_to_read) {
            re_lst  = 1;
            pBufRe[1] = 0x1; //(re_rsrvd  & 0x3f) << 2 + (re_frst << 1) + re_lst;
        }

        // forward data to LB

        *pEntrp = 0; // until p4 entropy field field fix  // htons(ntohl(*pSeq)); // for now

        if(passedV) {
            fprintf ( stdout, "\nLB Meta-data:\n");
            for(uint8_t b = 0; b < lblen; b++) fprintf ( stdout, " [%d] = %x", b, pBufLb[b]);
            fprintf ( stdout, " entropy = %d", ntohs(*pEntrp));
            fprintf ( stdout, " lb_tick = %" PRIu64 "\n", lb_tick);

            fprintf ( stdout, "\nLB Meta-data on the wire:\n");
            for(uint8_t b = 0; b < lblen; b++) fprintf ( stdout, " [%d] = %x", b, pBufLb[b]);
            fprintf ( stdout, " entropy = %d", (*pEntrp));
            fprintf ( stdout, " for lb_tick = %" PRIu64 " ", *pTick);

            fprintf ( stdout, "\nRE Meta-data:\n");
            for(uint8_t b = 0; b < relen; b++) fprintf ( stdout, " [%d] = %x ", b, pBufRe[b]);
            fprintf ( stdout, " for re_frst = %d / re_lst = %d", re_frst, re_lst); 
            fprintf ( stdout, " / re_data_id = %d / re_seq = %d\n", re_data_id, re_seq);	
            fprintf ( stdout, " re_tick = %" PRIu64 "\n", lb_tick);

            fprintf ( stdout, "\nRE Meta-data on the wire:\n");
            for(uint8_t b = 0; b < relen; b++) fprintf ( stdout, " [%d] = %x ", b, pBufRe[b]);
            fprintf ( stdout, " for re_frst = %d / re_lst = %d", re_frst, re_lst); 
            fprintf ( stdout, " / re_data_id = %d / re_seq = %d\n", *pDid, *pSeq);	
            fprintf ( stdout, " for re_tick = %" PRIu64 " ", *pReTick);
        }

        ssize_t rtCd = 0;
        /* now send a datagram */
        size_t num_to_send = nr+mdlen; // max bytes to send including metadata
        if (passed6) {
            if ((rtCd = sendto(dst_sckt, buffer, num_to_send, 0, 
                    (struct sockaddr *)&dst_addr6, sizeof dst_addr6)) < 0) {
                perror("sendto failed");
                exit(4);
            }
        } else {
            if ((rtCd = sendto(dst_sckt, buffer, num_to_send, 0, 
                    (struct sockaddr *)&dst_addr, sizeof dst_addr)) < 0) {
                perror("sendto failed");
                exit(4);
            }
        }
        if(passedV) fprintf ( stdout, "\nSending %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt);
        re_frst = 0;
        pBufRe[1] = 0x0; //(re_rsrvd  & 0x3f) << 2 + (re_frst << 1) + re_lst;
        *pSeq = htonl(++re_seq);
    } while(!re_lst);
    return 0;
}
