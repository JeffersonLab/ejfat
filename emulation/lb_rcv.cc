//       Reassembly Engine - Volkswagon  Quality
//
// reads binary from well known ip/port
// writes reassembled binary data to disc

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
#include <string.h>
#include <fstream>
#include <iostream>
#include <inttypes.h>
#include <netdb.h>
#include <time.h>
#include <chrono>
#include <ctime>

using namespace std;

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const size_t max_pckt_sz  = 9000-20-8;  // = MTU - IP header - UDP header
const size_t enet_pad     = 2+8;
const size_t relen        = 8+enet_pad;     // 8 for flags, data_id, (2+8) bytes of pad to avoid ethernet packets < 64B
const size_t mdlen        = relen;
const size_t max_data_ids = 100;          // support up to 10 data_ids

uint8_t  in_buff[max_pckt_sz];

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address  \n\
        -p listen port  \n\
        -n num events  \n\
        -v verbose mode (default is quiet)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i -p\n";
}

static volatile int cpu=-1;

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI=false, passedP=false, passed6=false, passedN=false;
    bool passedV=false;

    char     lstn_ip[INET6_ADDRSTRLEN]; // listening ip
    uint16_t lstn_prt;                  // listening port
    uint32_t num_evnts;                 // number of events to recv

    while ((optc = getopt(argc, argv, "hi:p:6n:v")) != -1)
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
            strcpy(lstn_ip, (const char *) optarg) ;
            passedI = true;
            fprintf(stdout, "-i %s ", lstn_ip);
            break;
        case 'p':
            lstn_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            fprintf(stdout, "-p %d ", lstn_prt);
            break;
        case 'n':
            num_evnts = (uint32_t) atoi((const char *) optarg) ;
            passedN = true;
            fprintf(stdout, "-n %d ", num_evnts);
            break;
        case 'v':
            passedV = true;
            fprintf(stdout, "-v ");
            break;
        case '?':
            fprintf (stdout, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }
    fprintf(stdout, "\n");
    if(!(passedI && passedP && passedN)) { Usage(); exit(1); }

    // pre-open stream for port
    ofstream rs;
    ofstream rslg;
    char x[64];
//  sprintf(x,"/tmp/rs_%d",lstn_prt);
    sprintf(x,"/tmp/rs_%d",lstn_prt);   // on ejfat-fs
    rs.open(x,std::ios::binary | std::ios::out);
    char xlg[64];
//  sprintf(xlg,"/tmp/rs_%d_log",lstn_prt);
    sprintf(xlg,"/nvme/goodrich/rs_%d_log",lstn_prt);
    rslg.open(xlg,std::ios::out);

//===================== data reception setup ===================================
    int lstn_sckt, nBytes;
    socklen_t addr_size;
    struct sockaddr_storage src_addr;

    if (passed6) {
        struct sockaddr_in6 lstn_addr6;

        /*Create UDP socket for reception from sender */
        if ((lstn_sckt = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
            perror("creating src socket");
            exit(1);
        }

        /* increase UDP receive buffer size */
        int recvBufSize = 0;
    #ifdef __APPLE__
        // By default set recv buf size to 7.4 MB which is the highest
        // it wants to go before before reverting back to 787kB.
        recvBufSize = 7400000;
    #else
        // By default set recv buf size to 100 MB
        recvBufSize = 100000000;
    #endif
        setsockopt(lstn_sckt, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));

        /*Configure settings in address struct*/
        /* clear it out */
        memset(&lstn_addr6, 0, sizeof(lstn_addr6));
        /* it is an INET address */
        lstn_addr6.sin6_family = AF_INET6; 
        /* the port we are going to send to, in network byte order */
        lstn_addr6.sin6_port = htons(lstn_prt);           // "LB" = 0x4c42 by spec (network order)
        /* the server IP address, in network byte order */
        inet_pton(AF_INET6, lstn_ip, &lstn_addr6.sin6_addr);  // LB address
        ///bind(lstn_sckt, (struct sockaddr *) &lstn_addr6, sizeof(lstn_addr6));
        bind(lstn_sckt, (struct sockaddr *) &lstn_addr6, sizeof(lstn_addr6));
    } else {
        struct sockaddr_in lstn_addr;
        socklen_t addr_size;

        /*Create UDP socket for reception from sender */
        lstn_sckt = socket(PF_INET, SOCK_DGRAM, 0);

        /* increase UDP receive buffer size */
        int recvBufSize = 25000000;
        setsockopt(lstn_sckt, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        {
            int recvBufSize1 = 0;
            socklen_t optlen = sizeof(recvBufSize1);
            getsockopt(lstn_sckt, SOL_SOCKET, SO_RCVBUF, &recvBufSize1, &optlen);
            std::cout << "Rcv buffer size " << recvBufSize1 << " optlen " << optlen << '\n';
        }

        /*Configure settings in address struct*/
        lstn_addr.sin_family = AF_INET;
        lstn_addr.sin_port = htons(lstn_prt); // "LB"
        lstn_addr.sin_addr.s_addr = inet_addr(lstn_ip); //indra-s2
        memset(lstn_addr.sin_zero, '\0', sizeof lstn_addr.sin_zero);

        /*Bind socket with address struct*/
        bind(lstn_sckt, (struct sockaddr *) &lstn_addr, sizeof(lstn_addr));
    }

//=======================================================================

    // RE meta data is at front of in_buff
    uint8_t* pBufRe = in_buff;

    uint32_t* pSeq    = (uint32_t*) &in_buff[mdlen-sizeof(uint32_t)-enet_pad];
    uint16_t* pDid    = (uint16_t*) &in_buff[mdlen-sizeof(uint32_t)-sizeof(uint16_t)-enet_pad];

    auto t_start = std::chrono::steady_clock::now();
    auto t_start0 = t_start; //previous occurance of 'frst' flag
    auto t_end   = std::chrono::steady_clock::now();

    uint32_t evnt_num = 0;  // event number

    double ltncy_mn = 0, ltncy_sd = 0;

    uint64_t evnt_sz = 0;  // event size

    uint32_t xseq   = 0; //expected seq #

    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

#ifdef __linux__
//        cpu = sched_getcpu();
#endif

//        nBytes = recvfrom(lstn_sckt, (void*)in_buff, sizeof(in_buff), 0, (struct sockaddr *)&src_addr, &addr_size);
        nBytes = recv(lstn_sckt, (void*)in_buff, sizeof(in_buff), 0);
        if(nBytes == -1)
        {
            perror("perror: recvfrom() == -1");
            std::cerr<<"perror: recvfrom() == -1: evnt_num = "<<evnt_num<<" seq = "<<xseq<<'\n';
        }
//std::cout<<"nBytes: "<<nBytes<<'\n';
        evnt_sz += std::max(0,nBytes-int(mdlen)); //////////////  why is this necessary ?

        // decode to host encoding
        uint32_t seq     = ntohl(*pSeq);
        uint16_t data_id = ntohs(*pDid);

        uint8_t vrsn = pBufRe[0] & 0xf;
        uint8_t frst = (pBufRe[1] & 0x02) >> 1;
        uint8_t lst  =  pBufRe[1] & 0x01;

        if(passedV && (evnt_num > 0) && frst) 
        {
            if(passedV)
            {
                t_start = std::chrono::steady_clock::now();
                std::cout << "Interval: "
                          << std::chrono::duration<double, std::micro>(t_start-t_start0).count()
                          << " us" << std::endl;
                t_start0 = t_start;
            }
        }
        
        if(passedV && evnt_num == 0 && frst) t_start0 = std::chrono::steady_clock::now();

        rs.write((char*)&in_buff[mdlen], nBytes-mdlen);
        if(passedV)
        {
            char s[1024];
            sprintf ( s, "Received %d bytes: ", nBytes);
            sprintf ( s, "Writing %d bytes: ", int(nBytes-mdlen));
            rslg.write((char*)s, strlen(s));
            sprintf ( s, "frst = %d / lst = %d ", frst, lst);
            rslg.write((char*)s, strlen(s));
            sprintf ( s, " / data_id = %d / seq = %d \n", data_id, seq);
            rslg.write((char*)s, strlen(s));
            sprintf( s, "cpu\t%d\n", cpu);
            rslg.write((char*)s, strlen(s));
        }
        xseq++;
        if(lst) 
        {
            std:cout << "Event_size: "<<evnt_sz<<" data_id: "<<lstn_prt<<'\n';
            xseq = 0;
            evnt_sz = 0;
            ++evnt_num;
            t_end = std::chrono::steady_clock::now();
//            ltncy_mn *= (evnt_num-1)/evnt_num; //incremental formula
//            ltncy_mn += std::chrono::duration<double, std::micro>(t_end-t_start).count()/evnt_num; //incremental formula

            if(passedV)
            {
                std::cout << "Latency: "
                          << std::chrono::duration<double, std::micro>(t_end-t_start0).count()
                          << " us" << std::endl;
            }
        }


    } while(evnt_num < num_evnts);

//    std::cout << "Mean Latency: " << ltncy_mn << '\n';

    rs.close();
    rslg.close();

    return 0;
}
