/************* Loab Balancer Packet Flood over repeats, ticks, data_ids  *******************/
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
#include <time.h>
#include <chrono>
#include <ctime>
#include <unistd.h>

using namespace std;

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i destination address  \n\
        -p destination port  \n\
        -t num ticks  \n\
        -d data_id  \n\
        -l event delay in usec  \n\
        -m mtu size (default 9000)  \n\
        -n num repeats  \n\
        -v verbose mode (default is quiet)  \n\
        -s event + remd size (B)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i\n";
}

const int64_t max_pckt_sz = 9000-20-8;  // = max MTU - IP header - UDP header
const int64_t lblen       = 16;
const size_t  enet_pad    = 2+8;
const size_t  relen       = 8+enet_pad;     // 8 for flags, data_id, (2+8) bytes of pad to avoid ethernet packets < 64B
const int64_t mdlen       = lblen + relen;

int main (int argc, char *argv[])
{

    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI=false, passedP=false, passed6=false;
    bool passedV=false, passedN=false;
    bool passedL=false, passedT=false, passedD=false;
    bool passedM=false, passedS=false;

    char     dst_ip[INET6_ADDRSTRLEN];  // target ip
    uint16_t dst_prt = 0x4c42;          // target port
    uint32_t num_rpts = 1;              // number of repeat sends
    uint64_t num_tcks = 1;              // number of sequential ticks

    const uint8_t  lb_vrsn      = 2;
    const uint8_t  lb_prtcl     = 1;
    const uint16_t lb_rsrvd     = 0;
    const uint8_t  re_vrsn      = 1;
    const uint16_t re_rsrvd     = 0;
    uint16_t       data_id      = 0;      // RE data_id
    useconds_t     lb_dly       = 0;
    int64_t        mtu_pyld_sz  = max_pckt_sz;
    int64_t        data_pyld_sz = mtu_pyld_sz;
    size_t         evnt_sz      = 0;


    while ((optc = getopt(argc, argv, "i:p:t:d:m:n:6vs:l:")) != -1)
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
            fprintf(stdout, "-p %d ", dst_prt);
            break;
        case 't':
            num_tcks = (uint64_t) atoi((const char *) optarg) ;
            passedT = true;
            fprintf(stdout, "-t %lu ", num_tcks);
            break;
        case 'd':
            data_id = (uint16_t) atoi((const char *) optarg) ;
            passedD = true;
            fprintf(stdout, "-d %d ", data_id);
            break;
        case 'm':
            mtu_pyld_sz = (size_t) atoi((const char *) optarg);
            mtu_pyld_sz = std::min(mtu_pyld_sz,max_pckt_sz);
            data_pyld_sz = mtu_pyld_sz;
            passedM = true;
            fprintf(stdout, "-m %lu ", mtu_pyld_sz);
            break;
        case 'l':
            lb_dly = (uint32_t) atoi((const char *) optarg) ;
            passedL = true;
            fprintf(stdout, "-l %d ", lb_dly);
            break;
        case 's':
            evnt_sz = (size_t) atoi((const char *) optarg);
            passedS = true;
            fprintf(stdout, "-s %lu ", evnt_sz);
            break;
        case 'n':
            num_rpts = (uint32_t) atoi((const char *) optarg) ;
            passedN = true;
            fprintf(stdout, "-n %d ", num_rpts);
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

    uint8_t buffer[mtu_pyld_sz];
    uint16_t pckt_sz;

    auto t_start = std::chrono::steady_clock::now();
    auto t_end   = std::chrono::steady_clock::now();

    uint8_t*  pBufLb = buffer;
    uint8_t*  pBufRe = &buffer[lblen];
    uint16_t* pEntrp = (uint16_t*) &pBufLb[6];
    uint64_t* pTick  = (uint64_t*) &pBufLb[8];
    uint16_t* pDid   = (uint16_t*) &pBufRe[2];
    uint32_t* pSeq   = (uint32_t*) &pBufRe[4];

    // meta-data in network order
    // LB metadata
    pBufLb[0] = 'L'; // 0x4c
    pBufLb[1] = 'B'; //0x42
    pBufLb[2] = lb_vrsn;   //version
    pBufLb[3] = lb_prtcl;  //protocol
    pBufLb[4] = 0;   //reserved
    pBufLb[5] = 0;   //reserved
    *pEntrp = htons(data_id);
    // RE metadata
    pBufRe[0] = 0x10; //(re_vrsn & 0xf) + (re_rsrvd & 0x3f0) >> 4;
    *pDid     = htons(data_id);

    for(uint64_t i = 0; i < num_rpts; i++) {
        for(uint64_t tick = 0; tick < num_tcks; tick++) {
            *pTick = HTONLL(tick);
            int64_t bytes_to_send = evnt_sz;
            uint32_t seq_num = 0;
            while(bytes_to_send > 0) {
                *pSeq = htonl(seq_num);
                //cout<<"bytes_to_send: "<<bytes_to_send<<" evnt_sz: "<<evnt_sz<<" data_pyld_sz: "<<data_pyld_sz<<endl;
                if(evnt_sz <= data_pyld_sz)   //first and last
                {
                    pBufRe[1] = 0x3; //first and last
                } else if(bytes_to_send == evnt_sz)   //first
                {
                    t_start = std::chrono::steady_clock::now();
                    if(passedV) std::cout << "Interval: "
                              << std::chrono::duration<double, std::micro>(t_start-t_end).count()
                              << " us" << std::endl;
                    pBufRe[1] = 0x2;  // neither first nor last
                } else if(bytes_to_send <= data_pyld_sz)  //last
                {
                    pBufRe[1] = 0x1; //last
                } else {
                    pBufRe[1] = 0x0;  // neither first nor last
                }

                // forward data to LB

                if(passedV) {
                    fprintf ( stdout, "\nLB Meta-data: ");
                    for(uint8_t b = 0; b < lblen; b++) fprintf ( stdout, " [%d] = %x", b, pBufLb[b]);
                    fprintf ( stdout, " entropy = %d", ntohs(*pEntrp));
                    fprintf ( stdout, " tick = %" PRIu64 " ", tick);

/***
                    fprintf ( stdout, "\nLB Meta-data on the wire:\n");
                    for(uint8_t b = 0; b < lblen; b++) fprintf ( stdout, " [%d] = %x", b, pBufLb[b]);
                    fprintf ( stdout, " entropy = %d", (*pEntrp));
                    fprintf ( stdout, " for tick = %" PRIu64 " ", *pTick);
***/

                    fprintf ( stdout, "\nRE Meta-data: ");
                    for(uint8_t b = 0; b < relen; b++) fprintf ( stdout, " [%d] = %x ", b, pBufRe[b]);
                    fprintf ( stdout, " for re_frst = %d / re_lst = %d",  (pBufRe[1] & 0x2)>>1, pBufRe[1] & 0x1); 
                    fprintf ( stdout, " / data_id = %d / re_seq = %d\n", data_id, seq_num);	

/***
                    fprintf ( stdout, "\nRE Meta-data on the wire:\n");
                    for(uint8_t b = 0; b < relen; b++) fprintf ( stdout, " [%d] = %x ", b, pBufRe[b]);
                    fprintf ( stdout, " for re_frst = %d / re_lst = %d", pBufRe[1] == 0x2, pBufRe[1] == 0x1); 
                    fprintf ( stdout, " / data_id = %d / re_seq = %d\n", *pDid, *pSeq);	
***/
                }

                ssize_t rtCd = 0;
                /* now send a datagram */
                if (passed6) {
                    if ((rtCd = sendto(dst_sckt, buffer, std::min(bytes_to_send+mdlen,mtu_pyld_sz), 0, 
                            (struct sockaddr *)&dst_addr6, sizeof dst_addr6)) < 0) {
                        perror("sendto failed");
                        exit(4);
                    }
                } else {
                    if ((rtCd = sendto(dst_sckt, buffer, std::min(bytes_to_send+mdlen,mtu_pyld_sz), 0, 
                            (struct sockaddr *)&dst_addr, sizeof dst_addr)) < 0) {
                        perror("sendto failed");
                        exit(4);
                    }
                }
                if(passedV) fprintf ( stdout, "\nSending %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt);
                if(bytes_to_send <= data_pyld_sz) //last
                {
                    t_end = std::chrono::steady_clock::now();
                    usleep(lb_dly);
                }
                bytes_to_send -= uint16_t(rtCd-mdlen);
                seq_num++;
            }
        }
    }
    // `time_t` is an arithmetic time type
    time_t now;
         
    // Obtain current time
    // `time()` returns the current time of the system as a `time_t` value
    time(&now);
         
    // Convert to local time format and print to stdout
    if(passedV) fprintf ( stdout, "Today is %s", ctime(&now));

    //dump buffer for inspection
    ofstream rs;
    char x[64];
    sprintf(x,"/tmp/lbpf_%d",data_id);
    rs.open(x,std::ios::binary | std::ios::out);
    for(uint64_t i = 0; i < num_rpts; i++) {
	int64_t bytes_to_send = evnt_sz;
	while(bytes_to_send > 0) {
            uint16_t nb = std::min(bytes_to_send,mtu_pyld_sz-mdlen);
            rs.write((char*)&buffer[mdlen], nb);
            bytes_to_send -= nb;
        }
    }
    rs.close();

    return 0;
}