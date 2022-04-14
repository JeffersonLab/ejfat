//       Reassembly Engine - Volkswagon  Quality
//
// reads binary from well known ip/port
// writes reassembled binary data to stdout

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

const size_t max_pckt_sz  = 9000-20-8;  // = MTU - IP header - UDP header
const size_t max_data_ids = 100;   // support up to 10 data_ids
const size_t max_ooo_pkts = 1000;  // support up to 100 out of order packets
const size_t relen        = 8;     // 8 for flags, data_id
const size_t mdlen        = relen;

// set up some cachd buffers for out-of-sequence work
char     pckt_cache[      max_data_ids][max_ooo_pkts][max_pckt_sz];
bool     pckt_cache_inuse[max_data_ids][max_ooo_pkts];
uint16_t pckt_sz[         max_data_ids][max_ooo_pkts];
bool     data_ids_inuse[  max_data_ids];
bool     lst_pkt_rcd[     max_data_ids];
int8_t   max_seq[         max_data_ids];

uint8_t  in_buff[max_pckt_sz];
uint8_t out_buff[max_ooo_pkts*max_pckt_sz];

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (string)  \n\
        -p listen port (number)  \n\
        -v verbose mode (default is quiet)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -i -p\n";
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

    bool passedI=false, passedP=false, passed6=false,
	passedV=false;

    char     lstn_ip[INET6_ADDRSTRLEN]; // listening ip
    uint16_t lstn_prt;                  // listening port

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
            fprintf(stdout, "-i ");
            break;
        case 'p':
            lstn_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            fprintf(stdout, "-p ");
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
        fprintf(stdout, "%s ", optarg);
    }
    fprintf(stdout, "\n");
    if(!(passedI && passedP)) { Usage(); exit(1); }

    // pre-open all data_id streams for tick
    ofstream rs[max_data_ids];
    for(uint16_t s = 0; s < max_data_ids; s++) {
        char x[64];
        sprintf(x,"/tmp/rs_%d_%d",lstn_prt,s);
        rs[s].open(x,std::ios::binary | std::ios::out);
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

//=======================================================================

    // RE meta data is at front of in_buff
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
    uint16_t num_data_ids = 0;  // number of data_ids encountered in this session

    uint32_t* pSeq    = (uint32_t*) &in_buff[mdlen-sizeof(uint32_t)];
    uint16_t* pDid    = (uint16_t*) &in_buff[mdlen-sizeof(uint32_t)-sizeof(uint16_t)];

    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        nBytes = recvfrom(lstn_sckt, in_buff, sizeof(in_buff), 0, (struct sockaddr *)&src_addr, &addr_size);

        // decode to host encoding
        uint32_t seq     = ntohl(*pSeq);
        uint16_t data_id = ntohs(*pDid);
        uint8_t vrsn     = pBufRe[0] & 0xf;
        uint8_t frst     = pBufRe[1] == 0x2; //(pBufRe[1] & 0x02) >> 1;
        uint8_t lst      = pBufRe[1] == 0x1; // pBufRe[1] & 0x01;

        char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV)) {
            perror("getnameinfo ");
        }
        if(passedV) {
            fprintf ( stdout, "Received %d bytes from source %s / %s : ", nBytes, gtnm_ip, gtnm_srvc);
            fprintf ( stdout, "frst = %d / lst = %d ", frst, lst); 
            fprintf ( stdout, " / data_id = %d / seq = %d ", data_id, seq);	
        }

        if(data_id >= max_data_ids) { if(passedV) cerr << "packet data_id exceeds bounds\n"; exit(1); }
        data_ids_inuse[data_id] = true;
        lst_pkt_rcd[data_id] = lst == 1; // assumes in-order !!!!  - FIX THIS
        if(lst) max_seq[data_id] = seq;
        if(seq >= max_ooo_pkts) { if(passedV) cerr << "packet buffering capacity exceeded\n"; exit(1); }	
        memmove(pckt_cache[data_id][seq], &in_buff[mdlen], nBytes-relen);
        pckt_sz[data_id][seq] = nBytes-relen;
            pckt_cache_inuse[data_id][seq] = true;

        if(passedV) fprintf (stdout, "cnt_trues %d max_seq[%i] = %d\n", 
                        cnt_trues(pckt_cache_inuse[data_id], max_ooo_pkts), data_id, max_seq[data_id]);

        if(cnt_trues(pckt_cache_inuse[data_id], max_seq[data_id]== -1?max_ooo_pkts:max_seq[data_id] + 1) 
                                                    == max_seq[data_id] + 1)  { //build blob and transfer
            uint16_t evnt_sz = 0;
            for(uint8_t i = 0; i <= max_seq[data_id]; i++) {
                 //setup egress buffer for ERSAP
                memmove(&out_buff[sizeof(uint16_t) + evnt_sz], pckt_cache[data_id][i], pckt_sz[data_id][i]);
                evnt_sz += pckt_sz[data_id][i];
                if(passedV) fprintf ( stdout, "reassembling seq# %d size = %d\n", i, pckt_sz[data_id][i]);
            }
            // forward data to sink skipping past lb meta data
            if(passedV) fprintf (stdout, "writing seq %d  size = %d\n", seq, evnt_sz);
            rs[data_id].write((char*)&out_buff[relen], evnt_sz);
            rs[data_id].flush();
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
        if(passedV) cout << endl;
    } while(1);
    return 0;
}
