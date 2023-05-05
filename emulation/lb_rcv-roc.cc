//       Reassembly Engine - Volkswagon  Quality
//
// reads binary from well known ip/port
// writes reassembled binary data to disc

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <cinttypes>
#include <netdb.h>
#include <ctime>
#include <chrono>

using namespace std;

#ifdef __linux__
    #define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif

#ifdef __APPLE__
#include <cctype>
#endif

#define btoa(x) ((x)?"true":"false")


const size_t max_pckt_sz  = 9000-20-8;  // = MTU - IP header - UDP header
const size_t relen        = 20;         // 4 for ver & data_id, 4 for offset, 4 for len, 8 for tick (event_id)
const size_t mdlen        = relen;
const size_t max_pckts    = 100;        // support up to 100 packets

const int outBufMaxLen = max_pckts*max_pckt_sz; // 100*8972 = 897kB
uint8_t  out_buff[outBufMaxLen];
uint8_t  in_buff[max_pckt_sz];

void   Usage()
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (default INADDR_ANY)  \n\
        -p listen port (default 17750)  \n\
        -n num events  \n\
        -l simulated extra latency (loop count)  \n\
        -v verbose mode (default is quiet)  \n\
        -x omit event size prefix  \n\
        -h help \n\n";
        cerr<<usage_str;
}

static volatile int cpu=-1;

int main (int argc, char *argv[])
{
    int optc;

    bool passedI=false, passedP=false, passed6=false, passedN=false;
    bool passedV=false, passedX=false, passedL=false;

    char     lstn_ip[INET6_ADDRSTRLEN]; // listening ip
    lstn_ip[0] = 0;
    uint16_t lstn_prt = 17750;          // listening port
    uint32_t num_evnts = 1;             // number of events to recv
    uint64_t lat_lps = 0;               // loop count to simulate extra latency

    while ((optc = getopt(argc, argv, "hi:p:6n:vxl:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case '6':
            passed6 = true;
            fprintf(stderr, "-6 ");
            break;
        case 'i':
            strcpy(lstn_ip, (const char *) optarg) ;
            passedI = true;
            fprintf(stderr, "-i %s ", lstn_ip);
            break;
        case 'p':
            lstn_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            fprintf(stderr, "-p %d ", lstn_prt);
            break;
        case 'l':
            lat_lps = (uint64_t) atoll((const char *) optarg) ;
            passedL = true;
		    fprintf(stderr, "-l %" PRIu64 " ", lat_lps);
            break;
        case 'n':
            num_evnts = (uint32_t) atoi((const char *) optarg) ;
            passedN = true;
            fprintf(stderr, "-n %d ", num_evnts);
            break;
        case 'v':
            passedV = true;
            fprintf(stderr, "-v ");
            break;
        case 'x':
            passedX = true;
            fprintf(stderr, "-x ");
            break;
        case '?':
            fprintf (stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }
    fprintf(stderr, "\n");

    // pre-open stream for port
    ofstream rs;
    ofstream rslg;
    char x[64];
//  sprintf(x,"/tmp/rs_%d",lstn_prt);
    sprintf(x,"/dev/stdout"); //for ersap test //sprintf(x,"/nvme/goodrich/rs_%d",lstn_prt);   // on ejfat-fs
    rs.open(x,std::ios::binary | std::ios::out);
    char xlg[64];
    sprintf(xlg,"/tmp/rs_%d_log",lstn_prt);//for ersap test //sprintf(xlg,"/nvme/goodrich/rs_%d_log",lstn_prt);
    rslg.open(xlg,std::ios::out);

//===================== data reception setup ===================================
    int lstn_sckt, nBytes, dataBytes;
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
        if (strlen(lstn_ip) > 0) {
            inet_pton(AF_INET6, lstn_ip, &lstn_addr6.sin6_addr);  // LB address
        }
        else {
            lstn_addr6.sin6_addr = in6addr_any;
        }

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
            std::cerr << "Rcv buffer size " << recvBufSize1 << " optlen " << optlen << '\n';
        }

        /*Configure settings in address struct*/
        lstn_addr.sin_family = AF_INET;
        lstn_addr.sin_port = htons(lstn_prt); // "LB"
        if (strlen(lstn_ip) > 0) {
            fprintf (stderr, "Using addr = %s\n", lstn_ip);
            lstn_addr.sin_addr.s_addr = inet_addr(lstn_ip); //indra-s2
        }
        else {
            fprintf (stderr, "Using INADDR_ANY\n");
            lstn_addr.sin_addr.s_addr = INADDR_ANY;
        }

        memset(lstn_addr.sin_zero, '\0', sizeof lstn_addr.sin_zero);

        /*Bind socket with address struct*/
        bind(lstn_sckt, (struct sockaddr *) &lstn_addr, sizeof(lstn_addr));
    }

//=======================================================================


    auto t_start = std::chrono::steady_clock::now();
    auto t_start0 = t_start; //previous occurance of 'frst' flag
    auto t_end   = std::chrono::steady_clock::now();

    uint32_t evnt_num = 0;  // event number

    double ltncy_mn = 0, ltncy_sd = 0;

    uint32_t evnt_sz = 0;  // event size for ERSAP

    uint32_t totalBytesRead = 0;
    bool veryFirstRead = true;
    uint16_t veryFirstId = UINT16_MAX;


    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

#ifdef __linux__
//        cpu = sched_getcpu();
#endif

//        nBytes = recvfrom(lstn_sckt, (void*)in_buff, sizeof(in_buff), 0, (struct sockaddr *)&src_addr, &addr_size);
        nBytes = recv(lstn_sckt, (void*)in_buff, max_pckt_sz, 0);
        if(nBytes == -1)
        {
            perror("perror: recvfrom() == -1");
            std::cerr<<"perror: recvfrom() == -1: evnt_num = "<< evnt_num << '\n';
        }
//std::cerr<<"nBytes: "<<nBytes<<'\n';
        evnt_sz += std::max(0,nBytes-int(mdlen)); 

        // RE meta data is at front of in_buff
        auto* pReTick = (uint64_t*) &in_buff[mdlen-sizeof(uint64_t)];
        auto* pLen    = (uint32_t*) &in_buff[mdlen-sizeof(uint64_t)-sizeof(uint32_t)];
        auto* pOff    = (uint32_t*) &in_buff[mdlen-sizeof(uint64_t)-2*sizeof(uint32_t)];
        auto* pDid    = (uint16_t*) &in_buff[mdlen-sizeof(uint64_t)-2*sizeof(uint32_t)-sizeof(uint16_t)];

        // decode to host encoding
        uint32_t off     = ntohl(*pOff);
        uint32_t len     = ntohl(*pLen);
        uint16_t data_id = ntohs(*pDid);
        uint64_t tick_net = *pReTick;
#ifdef __APPLE__
        uint64_t tick = NTOHLL(tick_net); // Beware on Mac, NTOHLL swaps actual arg!
        NTOHLL(tick_net); // Swap it back
#else
        uint64_t tick = NTOHLL(tick_net);
#endif

        //uint8_t vrsn  = (in_buff[0] >> 4) & 0xf;

        // At this point we dump packets from other sources
        if (veryFirstRead) {
            veryFirstId = data_id;
        }
        else if (data_id != veryFirstId) {
            // Dump packet
            if(passedV) fprintf (stderr, "Dump packet from src %hu\n", data_id);
            continue;
        }

        dataBytes = nBytes - relen;
        totalBytesRead += dataBytes;
        bool frst = (off == 0);
        bool lst  = (totalBytesRead >= len);


        if(passedV && (evnt_num > 0) && frst) {
            t_start = std::chrono::steady_clock::now();
            std::cerr << "Interval: "
                      << std::chrono::duration<double, std::micro>(t_start-t_start0).count()
                      << " us" << std::endl;
            t_start0 = t_start;
        }

        if(passedV && evnt_num == 0 && frst) t_start0 = std::chrono::steady_clock::now();

        if(passedV) {
            char s[1024];
            sprintf ( s, "Received %d bytes: ", nBytes);
            sprintf ( s, "Capture %d bytes for offset %d: ", int(nBytes-mdlen), int(off));
            rslg.write((char*)s, strlen(s));
            sprintf ( s, "frst = %s / lst = %s ", btoa(frst), btoa(lst));
            rslg.write((char*)s, strlen(s));
            sprintf ( s, " / data_id = %hu / off = %u / len = %u / tick = %" PRIu64 "\n", data_id, off, len, tick);
            rslg.write((char*)s, strlen(s));
            sprintf( s, "cpu\t%d\n", cpu);
            rslg.write((char*)s, strlen(s));
        }

        // Check to see if there's room to write all data into buf
        if (len > outBufMaxLen) {
            fprintf(stderr, "Internal buffer too small to hold data\n");
            return (1);
        }

        // Copy data into buf at correct location (provided by RE header).
        // This automatically corrects for out of order packets as long as they don't
        // cross tick boundaries.
        memcpy(out_buff + off, in_buff + relen, dataBytes);
        veryFirstRead = false;

        if(lst) {
            if(passedV) std:cerr << "Event_size: " << totalBytesRead << " data_id: " << data_id << '\n';
            ++evnt_num;
            t_end = std::chrono::steady_clock::now();
//            ltncy_mn *= (evnt_num-1)/evnt_num; //incremental formula
//            ltncy_mn += std::chrono::duration<double, std::micro>(t_end-t_start).count()/evnt_num; //incremental formula

            // first four bytes must be event size for ERSAP
            evnt_sz = totalBytesRead;
            if(!passedX) rs.write((char*)&evnt_sz, sizeof(evnt_sz));

            // write out all data
            rs.write((char*) in_buff, totalBytesRead);
            if(passedV) {
                char s[1024];
                sprintf ( s, "Assembled %u bytes for tick %" PRIu64 " from id %hu\n ", totalBytesRead, tick, data_id);
                rslg.write((char*)s, strlen(s));
            }

            // Start over, assuming packet marked as last really is last
            totalBytesRead = 0;
            veryFirstRead = true;
            evnt_sz = 0;
            
            if(passedL) for(size_t i=0;i<lat_lps;i++) i++; //simulated extra latency
            
            if(passedV) {
                std::cerr << "Latency: "
                          << std::chrono::duration<double, std::micro>(t_end-t_start0).count()
                          << " us" << std::endl;
            }
        }


    } while(evnt_num < num_evnts);

//    std::cerr << "Mean Latency: " << ltncy_mn << '\n';

    rs.close();
    rslg.close();

    return 0;
}
