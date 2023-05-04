/************* Load Balancer Packetizer / Sender  *******************/
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
#include <ctime>

using namespace std;

#ifdef __linux__
    #define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif

void   Usage()
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i destination address (string)  \n\
        -p destination port (number)  \n\
        -t lb_tick  \n\
        -d re_data_id  \n\
        -e re_entropy  \n\
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
	const size_t relen       = 20;     // 4 for ver & data_id, 4 for offset, 4 for len, 8 for tick (event_id)
    const size_t mdlen       = lblen + relen;

    int optc;

    bool passedI=false, passedP=false, passed6=false, passedV=false, passedE=false;

    char     dst_ip[INET6_ADDRSTRLEN];  // target ip
    uint16_t dst_prt = 0x4c42;          // target port

    const uint8_t lb_vrsn    = 2;
    const uint8_t lb_prtcl   = 1;
    const uint16_t lb_rsrvd  = 0;

    uint64_t lb_tick         = 1;      // LB tick
    uint64_t lb_tick_net;              // LB tick in network byte order

    const uint8_t re_vrsn    = 2;
    const uint16_t re_rsrvd  = 0;

    uint8_t re_frst          = 1;
    uint8_t re_lst           = 0;      // no longer in RE header, but useful in program

    uint16_t re_data_id      = 1;      // RE data_id
    uint16_t re_data_id_net;           // RE data_id in network byte order

    uint16_t re_entropy      = 0;      // RE entropy
    uint16_t re_entropy_net;           // RE entropy in network byte order

    uint32_t re_off          = 0;      // RE offset into reassembled buf
    uint32_t re_off_net;               // RE offset in network byte order

    uint32_t re_len          = 0;      // RE byte len of reassembled buf
    uint32_t re_len_net;               // RE byte len in network byte order

    size_t pckt_sz = max_pckt_sz;

    while ((optc = getopt(argc, argv, "i:p:t:d:n:e:6vs:")) != -1)
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
            fprintf(stdout, "-i %s ", dst_ip);
            break;
        case 'p':
            dst_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            fprintf(stdout, "-p %d", dst_prt);
            break;
        case 't':
            lb_tick = (uint64_t) atoi((const char *) optarg) ;
            fprintf(stdout, "-t %" PRIu64 "", lb_tick);
            break;
        case 'd':
            re_data_id = (uint16_t) atoi((const char *) optarg) ;
            fprintf(stdout, "-d %hu ", re_data_id);
            break;
        case 's':
            pckt_sz = (size_t) atoi((const char *) optarg) -20-8;  // = MTU - IP header - UDP header
            pckt_sz = min(pckt_sz,max_pckt_sz);
            fprintf(stdout, "-s %lu ", pckt_sz);
            break;
        case 'e':
            passedE = true;
            re_entropy = (uint16_t) atoi((const char *) optarg) ;
            fprintf(stdout, "-e %hu", re_entropy);
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

#ifdef __linux__
    // set the don't fragment bit
    {
        int val = IP_PMTUDISC_DO;
        setsockopt(dst_sckt, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
    }
#endif
//=======================================================================
    inet_pton(AF_INET6, dst_ip, &dst_addr6.sin6_addr);  // LB address

    uint8_t header_local[mdlen];  // LB and RE headers in local byte order
    uint8_t buffer_net[pckt_sz];  // network byte order data to send

    // meta-data in network order
	// LB metadata
    uint8_t*  pBufLb = buffer_net;
    pBufLb[0] = 'L'; // 0x4c
    pBufLb[1] = 'B'; //0x42
    pBufLb[2] = lb_vrsn;   //version
    pBufLb[3] = lb_prtcl;  //protocol
    pBufLb[4] = 0;   //reserved
    pBufLb[5] = 0;   //reserved
    auto* pEntrp  = (uint16_t*) &pBufLb[6];
    auto* pTick   = (uint64_t*) &pBufLb[8];

    // For verbose output, keep track of local byte order LB header
    if (passedV) {
        memcpy(header_local, pBufLb, lblen);
        auto* pEnt  = (uint16_t*) &header_local[6];
        auto* pTik  = (uint64_t*) &header_local[8];
        *pEnt       = re_entropy;
        *pTik       = lb_tick;
    }

    // put LB header in network byte order
    *pEntrp     = re_entropy_net = htons(re_entropy); // htons does NOT swap the arg iteself!
    lb_tick_net = lb_tick;
    *pTick      = HTONLL(lb_tick_net);                // HTONLL swaps the arg itself!
    

    // RE metadata
    uint8_t*  pBufRe = &buffer_net[lblen];
    pBufRe[0] = re_vrsn << 4;
    pBufRe[1] = 0;
    auto* pDid    = (uint16_t*) &pBufRe[2];
    auto* pOff    = (uint32_t*) &pBufRe[4];
    auto* pLen    = (uint32_t*) &pBufRe[8];
    auto* pReTick = (uint64_t*) &pBufRe[12];

    // For verbose output, keep track of local byte order RE header
    if (passedV) {
        memcpy(header_local+lblen, pBufRe, 2);
        auto* pD = (uint16_t*) &header_local[2 + lblen];
        auto* pO = (uint64_t*) &header_local[4 + lblen];
        auto* pL = (uint32_t*) &header_local[8 + lblen];
        auto* pR = (uint64_t*) &header_local[12 + lblen];

        *pD = re_data_id;
        *pO = re_off;
        *pL = re_len;
        *pR = lb_tick;
    }

    // put in network byte order
    *pDid          = re_data_id_net = htons(re_data_id); // does NOT swap arg itself
    *pOff          = re_off_net = htonl(re_off);
    *pReTick       = *pTick; // already swapped above

    // Find out how many bytes we'll be sending.
    // Go to end of input
    f1.seekg(0, ios_base::end);
    // Read how many preceding bytes in input
    re_len = (int) f1.tellg();
    *pLen = re_len_net = htonl(re_len);
    // Go back to the beginning for reading input
    f1.seekg(0, ios_base::beg);

    std::cout  << "\nLook like stdin is of len " << re_len << " bytes" << std::endl;

    do {
        f1.read((char*)&buffer_net[mdlen], num_to_read);
        streamsize nr = f1.gcount();
        if(passedV) cout  << "\nNum read from stdin: " << nr << endl;
        if(nr != num_to_read) {
            // last read from file piped in thru stdin
            re_lst = 1;
        }

        // forward data to LB

        if(passedV) {
            fprintf ( stdout, "\nLB Meta-data:\n");
            for(uint8_t b = 0; b < lblen; b++) fprintf ( stdout, " [%d] = %x", b, header_local[b]);
            fprintf ( stdout, " entropy = %hu", re_entropy);
            fprintf ( stdout, " lb_tick = %" PRIu64 "\n", lb_tick);

            fprintf ( stdout, "\nLB Meta-data on the wire:\n");
            for(uint8_t b = 0; b < lblen; b++) fprintf ( stdout, " [%d] = %x", b, pBufLb[b]);
            fprintf ( stdout, " entropy = %hu", re_entropy_net);
            fprintf ( stdout, " for lb_tick = %" PRIu64 "\n", lb_tick_net);

            fprintf ( stdout, "\nRE Meta-data:\n");
            for(uint8_t b = 0; b < relen; b++) fprintf ( stdout, " [%d] = %x ", b, header_local[b + lblen]);
            fprintf ( stdout, "\n for re_frst = %d / re_lst = %d", re_frst, re_lst);
            fprintf ( stdout, " / re_data_id %hu / re_off %u / re_len %u / re_tick %" PRIu64 "\n",
                      re_data_id, re_off, re_len, lb_tick);

            fprintf ( stdout, "\nRE Meta-data on the wire:\n");
            for(uint8_t b = 0; b < relen; b++) fprintf ( stdout, " [%d] = %x ", b, pBufRe[b]);
            fprintf ( stdout, "\n for re_frst = %d / re_lst = %d", re_frst, re_lst);
            fprintf ( stdout, " / re_data_id %hu / re_off %u / re_len %u / re_tick %" PRIu64 "\n",
                      re_data_id_net, re_off_net, re_len_net, lb_tick_net);
        }

        ssize_t rtCd = 0;
        /* now send a datagram */
        size_t num_to_send = nr+mdlen; // max bytes to send including metadata
        if (passed6) {
            if ((rtCd = sendto(dst_sckt, buffer_net, num_to_send, 0,
                    (struct sockaddr *)&dst_addr6, sizeof dst_addr6)) < 0) {
                perror("sendto failed");
                exit(4);
            }
        } else {
            if ((rtCd = sendto(dst_sckt, buffer_net, num_to_send, 0,
                    (struct sockaddr *)&dst_addr, sizeof dst_addr)) < 0) {
                perror("sendto failed");
                exit(4);
            }
        }
        if(passedV) fprintf ( stdout, "\nSending %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt);
        re_frst = 0;

        re_off += nr;
        *pOff = re_off_net = htonl(re_off);

    } while(!re_lst);


    // `time_t` is an arithmetic time type
    time_t now;

    // Obtain current time
    // `time()` returns the current time of the system as a `time_t` value
    time(&now);

    // Convert to local time format and print to stdout
    fprintf ( stdout, "Today is %s", ctime(&now));

    return 0;
}
