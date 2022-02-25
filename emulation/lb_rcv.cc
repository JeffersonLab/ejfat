//       Reassembly Engine - Volkswagon  Quality
//
// reads binary from well known ip/port
// writes reassembled binary data to TCP port

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

using namespace std;

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const size_t max_pckt_sz  = 1024;
const size_t max_data_ids = 10;   // support up to 10 data_ids
const size_t max_ooo_pkts = 100;  // support up to 100 out of order packets
const size_t relen        = 8+8;  // 8 for flags, data_id, 8 for tick
const size_t mdlen        = relen;

// set up some cachd buffers for out-of-sequence work
char     pckt_cache[      max_data_ids][max_ooo_pkts][max_pckt_sz];
bool     pckt_cache_inuse[max_data_ids][max_ooo_pkts];
uint16_t pckt_sz[         max_data_ids][max_ooo_pkts];
bool     data_ids_inuse[  max_data_ids];
bool     lst_pkt_rcd[     max_data_ids];
int8_t   max_seq[         max_data_ids];

uint8_t  in_buff[relen + max_pckt_sz];
uint8_t out_buff[max_ooo_pkts*max_pckt_sz];

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (string)  \n\
        -p listen port (number)  \n\
        -t send address (string)  \n\
        -r send port (number)  \n\
        -u send UDP (default TCP)  \n\
        -v verbose mode (default is quiet)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i -p -t -r\n";
}

uint16_t cnt_trues(bool b[], uint16_t n) // returns count of true values in array
{
    uint16_t cnt = 0;
    for(uint16_t k = 0; k<n; k++) if(b[k] == true) cnt++;
    return cnt;
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI=false, passedP=false, passedT=false, passedR=false, passed6=false,
	passedU=false, passedV=false;

    char     lstn_ip[INET6_ADDRSTRLEN], dst_ip[INET6_ADDRSTRLEN]; // listening, target ip
    uint16_t lstn_prt, dst_prt;                          // listening, target ports

    while ((optc = getopt(argc, argv, "i:p:t:r:6uv")) != -1)
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
            strcpy(lstn_ip, (const char *) optarg) ;
            passedI = true;
            break;
        case 'p':
            lstn_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            break;
         case 't':
            strcpy(dst_ip, (const char *) optarg) ;
            passedT = true;
            break;
        case 'r':
            dst_prt = (uint16_t) atoi((const char *) optarg) ;
            passedR = true;
            break;
        case 'u':
            passedU = true;
            break;
        case 'v':
            passedV = true;
            break;
        case '?':
            fprintf (stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }

    if(!(passedI &&  passedP)) { Usage(); exit(1); }
    if(  passedT && !passedR)  { Usage(); exit(1); }

    // pre-open all data_id streams for tick
    ofstream rs[max_data_ids];
    if(!passedT) {
        for(uint16_t s = 0; s < max_data_ids; s++) {
            char x[64];
            sprintf(x,"/tmp/rs_%d",s);
            rs[s].open(x,std::ios::binary | std::ios::out);
        }
    }

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

        /*Configure settings in address struct*/
        lstn_addr.sin_family = AF_INET;
        lstn_addr.sin_port = htons(lstn_prt); // "LB"
        lstn_addr.sin_addr.s_addr = inet_addr(lstn_ip); //indra-s2
        memset(lstn_addr.sin_zero, '\0', sizeof lstn_addr.sin_zero);

        /*Bind socket with address struct*/
        bind(lstn_sckt, (struct sockaddr *) &lstn_addr, sizeof(lstn_addr));
    }

//===================== data destination setup ===================================
    int dst_sckt;
    struct sockaddr_in6 dst_addr6;
    struct sockaddr_in dst_addr;

    if (passed6 && passedT) {
        /* create a socket in the INET6 protocol */
        if ((dst_sckt = socket(AF_INET6, passedU?SOCK_DGRAM:SOCK_STREAM, 0)) < 0) {
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

    } else if (passedT){
        // Create  socket in the INET protocol
        if ((dst_sckt = socket(PF_INET, passedU?SOCK_DGRAM:SOCK_STREAM, 0)) < 0) {
	        perror("creating dst socket");
	        exit(1);
	    }

        // Configure settings in address struct
        dst_addr.sin_family = AF_INET;
        dst_addr.sin_port = htons(dst_prt); // data consumer port to send to
        dst_addr.sin_addr.s_addr = inet_addr(dst_ip); // indra-s3 as data consumer
        memset(dst_addr.sin_zero, '\0', sizeof dst_addr.sin_zero);

        // Initialize size variable to be used later on
        socklen_t addr_size = sizeof dst_addr;
    }
    if(!passedU && passedT) if (connect(dst_sckt, 
                            passed6?(struct sockaddr *)&dst_addr6:(struct sockaddr *)&dst_addr, 
                            passed6?sizeof dst_addr6:sizeof dst_addr) < 0) {
        perror("connecting to dst socket");
        exit(1);
    }

//=======================================================================

    // RE neta data is at front of in_buff
    uint8_t* pBufRe = in_buff;

    // start all data_id streams at seq = 0
    for(uint16_t i = 0; i < max_data_ids; i++) {
        data_ids_inuse[i] = false;
        lst_pkt_rcd[i] = false;
        max_seq[i] = -1; // -1 indicates max seq no. not yet known
        for(uint16_t j = 0; j < max_ooo_pkts; j++)  {
            pckt_cache_inuse[i][j] = false;
            pckt_sz[i][j] = 0;
        }
    }
    uint16_t  num_data_ids = 0;  // number of data_ids encountered in this session

    uint64_t* pReTick = (uint64_t*) &in_buff[mdlen-sizeof(uint64_t)];
    uint32_t* pSeq    = (uint32_t*) &in_buff[mdlen-sizeof(uint64_t)-sizeof(uint32_t)];
    uint16_t* pDid    = (uint16_t*) &in_buff[mdlen-sizeof(uint64_t)-sizeof(uint32_t)-sizeof(uint16_t)];

    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        nBytes = recvfrom(lstn_sckt, in_buff, sizeof(in_buff), 0, (struct sockaddr *)&src_addr, &addr_size);

        // decode to host encoding
        uint64_t retick  = NTOHLL(*pReTick);
        uint32_t seq     = ntohl(*pSeq);
        uint16_t data_id = ntohs(*pDid);
        uint8_t  vrsn    = (pBufRe[0] & 0xf0) >> 4;
        uint8_t  frst    = (pBufRe[1] & 0x02) >> 1;
        uint8_t  lst     =  pBufRe[1] & 0x01;

        char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV)) {
            perror("getnameinfo ");
        }
        if(passedV) fprintf ( stderr, "Received %d bytes from source %s / %s : ", nBytes, gtnm_ip, gtnm_srvc);
        if(passedV) fprintf ( stderr, "frst = %d / lst = %d ", frst, lst); 
        if(passedV) fprintf ( stderr, " / data_id = %d / seq = %d ", data_id, seq);	
        if(passedV) fprintf ( stderr, "tick = %" PRIu64 "\n", retick);

        if(data_id >= max_data_ids) { if(passedV) cerr << "packet data_id exceeds bounds"; exit(1); }
        data_ids_inuse[data_id] = true;
        lst_pkt_rcd[data_id] = lst == 1; // assumes in-order !!!!  - FIX THIS
        if(lst) max_seq[data_id] = seq;
        if(seq >= max_ooo_pkts) { if(passedV) cerr << "packet buffering capacity exceeded"; exit(1); }	
        memmove(pckt_cache[data_id][seq], &in_buff[mdlen], nBytes-relen);
        pckt_sz[data_id][seq] = nBytes-relen;
        pckt_cache_inuse[data_id][seq] = true;

        if(passedV) fprintf ( stderr, "cnt_trues %d max_seq[%i] = %d\n", 
	        cnt_trues(pckt_cache_inuse[data_id], max_ooo_pkts), data_id, max_seq[data_id]);

        if(cnt_trues(pckt_cache_inuse[data_id], max_seq[data_id]== -1?max_ooo_pkts:max_seq[data_id] + 1) 
                                                    == max_seq[data_id] + 1)  { //build blob and transfer
            uint16_t evnt_sz = 0;
            for(uint8_t i = 0; i <= max_seq[data_id]; i++) {
                 //setup egress buffer for ERSAP
                memmove(&out_buff[sizeof(uint16_t) + evnt_sz], pckt_cache[data_id][i], pckt_sz[data_id][i]);
                evnt_sz += pckt_sz[data_id][i];
                if(passedV) fprintf ( stderr, "reassembling seq# %d size = %d\n", i, pckt_sz[data_id][i]);
            }
            // put event size in reserved short sized slot at head of out_buf
            if(passedV) fprintf ( stderr, "evnt6_sz = %d or on network =  %x\n", evnt_sz, htons(evnt_sz));
            *((uint16_t*) out_buff) = (evnt_sz); 
            // forward data to sink skipping past lb meta data
            ssize_t rtCd = 0;
            if(passedU) {
                if ((rtCd = sendto(dst_sckt, out_buff, evnt_sz + sizeof(uint16_t), 0, 
                            passed6?(struct sockaddr *)&dst_addr6:(struct sockaddr *)&dst_addr, 
                            passed6?sizeof dst_addr6:sizeof dst_addr)) < 0) {
                    perror("sendto failed");
                    exit(4);
                }
            } else {
                if ((rtCd = send(dst_sckt, out_buff, evnt_sz + sizeof(uint16_t), 0)) < 0) {
                    perror("send failed");
                    exit(4);
                }
            }
            // start all data_id streams at seq = 0
            for(uint16_t i = 0; i < max_data_ids; i++) {
                data_ids_inuse[i] = false;
                lst_pkt_rcd[i] = false;
                max_seq[i] = -1;
                for(uint16_t j = 0; j < max_ooo_pkts; j++)  {
                    pckt_cache_inuse[i][j] = false;
                    pckt_sz[i][j] = 0;
                }
            }
            num_data_ids = 0;  // number of data_ids encountered in this session
        }
    } while(1);
    return 0;
}
