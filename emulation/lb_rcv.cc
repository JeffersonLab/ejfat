//       Reassembly Engine - Volkswagon  Quality
//
// reads binary from well known ip/port
// writes reassembled binary data to stdout

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
const size_t max_data_ids = 100;        // support up to 100 data_ids
const size_t max_ooo_pkts = 1000;        // support up to 1000 out of order packets
const size_t relen        = 20;         // 4 for ver & data_id, 4 for offset, 4 for len, 8 for tick (event_id)
const size_t mdlen        = relen;


uint8_t  in_buff[max_pckt_sz];
uint8_t  out_buff[max_ooo_pkts*max_pckt_sz]; // 8.96 MB max

void   Usage()
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (default INADDR_ANY)  \n\
        -p listen port (default 17750)  \n\
        -v verbose mode (default is quiet)  \n\
        -h help \n\n";
        cout<<usage_str;
}


int main (int argc, char *argv[])
{
    int optc;

    bool passedI=false, passedP=false, passed6=false,
	passedV=false;

    char     lstn_ip[INET6_ADDRSTRLEN]; // listening ip
    lstn_ip[0] = 0;
    uint16_t lstn_prt = 17750;          // listening port

    while ((optc = getopt(argc, argv, "i:p:6v")) != -1)
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
            fprintf(stdout, "-p %d", lstn_prt);
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

    // pre-open all data_id streams for tick
    ofstream rs[max_data_ids];
    for (uint16_t s = 0; s < max_data_ids; s++) {
        char x[64];
        sprintf(x,"/tmp/rs_%d_%d",lstn_prt,s);
//fprintf (stdout, "Open file %s\n", x);
        rs[s].open(x,std::ios::binary | std::ios::out);
    }

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

        /*Configure settings in address struct*/
        lstn_addr.sin_family = AF_INET;
        lstn_addr.sin_port = htons(lstn_prt); // "LB"
        if (strlen(lstn_ip) > 0) {
            fprintf (stdout, "Using addr = %s\n", lstn_ip);
            lstn_addr.sin_addr.s_addr = inet_addr(lstn_ip); //indra-s2
        }
        else {
            fprintf (stdout, "Using INADDR_ANY\n");
            lstn_addr.sin_addr.s_addr = INADDR_ANY;
        }

        memset(lstn_addr.sin_zero, '\0', sizeof lstn_addr.sin_zero);

        /*Bind socket with address struct*/
        bind(lstn_sckt, (struct sockaddr *) &lstn_addr, sizeof(lstn_addr));
    }

//=======================================================================

    // RE meta data is at front of in_buff
    uint8_t* pBufRe = in_buff;
    int outBufMaxLen = max_ooo_pkts*max_pckt_sz;

    auto* pDid    = (uint16_t*) &in_buff[mdlen-sizeof(uint64_t)-2*sizeof(uint32_t)-sizeof(uint16_t)];
    auto* pOff    = (uint32_t*) &in_buff[mdlen-sizeof(uint64_t)-2*sizeof(uint32_t)];
    auto* pLen    = (uint32_t*) &in_buff[mdlen-sizeof(uint64_t)-sizeof(uint32_t)];
    auto* pReTick = (uint64_t*) &in_buff[mdlen-sizeof(uint64_t)];

    uint32_t totalBytesRead = 0;
    bool veryFirstRead = true;
    uint16_t veryFirstId = UINT16_MAX;


    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        fprintf ( stdout, "Waiting to read packet\n");
        nBytes = (int)recvfrom(lstn_sckt, in_buff, sizeof(in_buff), 0, (struct sockaddr *)&src_addr, &addr_size);
        fprintf ( stdout, "Got packet\n");

        // decode to host encoding
        uint32_t off     = ntohl(*pOff);  // seq --> off
        uint32_t len     = ntohl(*pLen);
        uint16_t data_id = ntohs(*pDid);
        uint64_t re_tick_net = *pReTick;  // Beware on Mac, NTOHLL swaps actual arg!
        uint64_t re_tick = NTOHLL(re_tick_net);
        int test_pre_swap = 1, test_post_swap = 1;
        NTOHLL(test_post_swap);
        if (test_pre_swap != test_post_swap) {
            fprintf ( stdout, "NOTHLL(x) is a problem, pre swap i = 1, post swap i = %d\n", test_post_swap);
        }
        //uint8_t vrsn  = (pBufRe[0] >> 4) & 0xf;

        // At this point we dump packets from other sources
        if (veryFirstRead) {
            veryFirstId = data_id;
        }
        else if (data_id != veryFirstId) {
            // Dump packet
            if(passedV) fprintf (stdout, "Dump packet from src %hu\n", data_id);
            continue;
        }

        dataBytes = nBytes - relen;
        totalBytesRead += dataBytes;
        bool frst = (off == 0);
        bool lst  = (totalBytesRead >= len);


        if (passedV) {
            if (veryFirstRead) {
                char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
                if (getnameinfo((struct sockaddr *) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                                sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV)) {
                    perror("getnameinfo ");
                }
                fprintf ( stdout, "Receiving data from source %s / %s :\n", gtnm_ip, gtnm_srvc);
            }

            fprintf ( stdout, "Received %d bytes / ", nBytes);
            fprintf ( stdout, "frst = %s / lst = %s", btoa(frst), btoa(lst));
            fprintf ( stdout, " / data_id = %d / off = %u / len = %u / ", data_id, off, len);
            fprintf( stdout, "tick = %" PRIu64 "\n", re_tick);
        }

        // Check to see if there's room to write all data into buf
        if (len > outBufMaxLen) {
            fprintf(stdout, "Internal buffer too small to hold data\n");
            return (1);
        }

        // Copy data into buf at correct location (provided by RE header).
        // This automatically corrects for out of order packets as long as they don't
        // cross tick boundaries.
        memcpy(out_buff + off, in_buff + relen, dataBytes);
        veryFirstRead = false;

        // If we've written all data to this buf ...
        if (totalBytesRead >= len) {
            // Done w/ this buffer

            // forward data to sink skipping past lb meta data
            if(passedV) fprintf (stdout, "writing tick %" PRIu64 ", size = %u, from src %hu\n", re_tick, totalBytesRead, data_id);
            rs[data_id].write((char*)out_buff, totalBytesRead);
            rs[data_id].flush();

            // Start over
            totalBytesRead = 0;
            veryFirstRead = true;

            // `time_t` is an arithmetic time type
            time_t now;

            // Obtain current time
            // `time()` returns the current time of the system as a `time_t` value
            time(&now);

            // Convert to local time format and print to stdout
            fprintf ( stdout, "Today is %s", ctime(&now));
        }
        if(passedV) std::cout << std::endl;

    } while(true);

    return 0;
}
