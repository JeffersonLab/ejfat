//       Reassembly Engine Emulation
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
#include <netdb.h>

using namespace std;

#ifdef __APPLE__
#include <ctype.h>
#endif

const size_t max_pckt_sz = 1024;
const size_t max_data_ids = 100;  // support up to 100 data_ids
const size_t max_ooo_pkts = 10;  // support up to 10 out of order packets
const size_t relen        = 8;
const size_t mdlen        = relen;

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (string)  \n\
        -p listen port (number)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -s\n";
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

    bool passedI, passedP, passed6 = false;

    char lstn_ip[INET6_ADDRSTRLEN]; // listening ip
    uint16_t lstn_prt;   // listening ports

    while ((optc = getopt(argc, argv, "i:p:6")) != -1)
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
        case '?':
            fprintf(stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }

    if(!(passedI && passedP)) { Usage(); exit(1); }

    ofstream rs[max_data_ids];
    for(uint16_t s = 0; s < max_data_ids; s++) {
        char x[64];
        sprintf(x,"/tmp/rs_%d",s);
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

    uint8_t buffer[max_pckt_sz + relen];
    uint8_t* pBufRe = buffer;

    // RE neta data is at front of buffer
    union re* premd = (union re*)buffer; 
    uint16_t did_seq[max_data_ids];
    // start all data_id streams at seq = 0
    for(uint16_t i = 0; i < max_data_ids; i++) did_seq[i] = 0; 
    // set up some cachd buffers for out-of-sequence work
    char pckt_cache[max_data_ids][max_ooo_pkts][max_pckt_sz + relen];
    bool pckt_cache_inuse[max_data_ids][max_ooo_pkts];
    uint16_t pckt_sz[max_data_ids][max_ooo_pkts];
    bool data_ids_inuse[max_data_ids];
    bool lst_pkt_rcd[max_data_ids];
    for(uint16_t i = 0; i < max_data_ids; i++) {
        data_ids_inuse[i] = false;
        lst_pkt_rcd[i] = false;
        for(uint16_t j = 0; j < max_ooo_pkts; j++)  {
            pckt_cache_inuse[i][j] = false;
            pckt_sz[i][j] = 0;
        }
    }
    uint16_t num_data_ids = 0;  // number of data_ids encountered in this session

    uint32_t* pSeq  = (uint32_t*) &buffer[mdlen-sizeof(uint32_t)];
    uint16_t* pDid  = (uint16_t*) &buffer[mdlen-sizeof(uint32_t)-sizeof(uint16_t)];

    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        nBytes = recvfrom(lstn_sckt, buffer, sizeof(buffer), 0, (struct sockaddr *)&src_addr, &addr_size);

        // decode to host encoding
        uint32_t seq     = ntohl(*pSeq);
        uint16_t data_id = ntohs(*pDid);
        uint8_t vrsn     = (pBufRe[0] & 0xf0) >> 4;
        uint8_t frst     = (pBufRe[1] & 0x02) >> 1;
        uint8_t lst      =  pBufRe[1] & 0x01;

        char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV)) {
            perror("getnameinfo ");
        }
        fprintf( stderr, "Received %d bytes from source %s / %s : ", nBytes, gtnm_ip, gtnm_srvc);
        fprintf( stderr, "frst = %d / lst = %d ", frst, lst); 
        fprintf( stderr, " / data_id = %d / seq = %d\n", data_id, seq);	

        if(data_id >= max_data_ids) { cerr << "packet data_id exceeds bounds"; exit(1); }
        data_ids_inuse[data_id] = true;
        lst_pkt_rcd[data_id] = lst == 1;
        if(seq == did_seq[data_id]) { //the seq # we were expecting
            cerr << "writing seq " <<  seq << " size = " << int(nBytes-relen) << endl;
            rs[data_id].write((char*)&buffer[relen], nBytes-relen);
            rs[data_id].flush();
            // while we can find cached packets
            while(pckt_cache_inuse[data_id][++did_seq[data_id]] == true) { 
                union re* premd1 = (union re*)pckt_cache[data_id][did_seq[data_id]];
                cerr << "writing seq " <<  seq << " from slot " << did_seq[data_id] 
                     << " size = " << pckt_sz[did_seq[data_id]]-relen << endl;
                rs[data_id].write((char*)&pckt_cache[data_id][did_seq[data_id]][relen], pckt_sz[data_id][did_seq[data_id]]-relen);
                rs[data_id].flush();
                pckt_cache_inuse[data_id][did_seq[data_id]] = false;
            }
        } else { // out of expected order - save packet in associated slot
            if(seq >= max_ooo_pkts) { cerr << "out of order packet seq exceeds bounds"; exit(1); }	
            memmove(pckt_cache[data_id][seq], buffer, nBytes);
            pckt_sz[data_id][seq] = nBytes;
            pckt_cache_inuse[data_id][seq] = true;
            cerr << "Received packet out of sequence: expected " <<  did_seq[data_id] << " recd " << seq << '\n';
            cerr << "store pckt " <<  seq << " in slot " << seq << endl;
        }
    } while(cnt_trues(data_ids_inuse, max_data_ids) != cnt_trues(lst_pkt_rcd, max_data_ids));
    return 0;
}
