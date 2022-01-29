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

using namespace std;

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const unsigned int max_pckt_sz = 1024;
const unsigned int max_data_ids = 100;  // support up to 100 data_ids
const unsigned int max_ooo_pkts = 10;  // support up to 10 out of order packets

void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -i listening ipv4 address (string)  \n\
        -p listening ipv4 port (number)  \n\
        -h help \n\n";
        cout<<usage_str;
        cout<<"Required: -s\n";
}

unsigned int cnt_trues(bool b[], unsigned int n) // returns count of true values in array
{
    unsigned int cnt = 0;
    for(unsigned int k = 0; k<n; k++) if(b[k] == true) cnt++;
    return cnt;
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI, passedP = false;

    char ip[64];  // listening ip, port
    uint16_t prt; // listening ip, port

    while ((optc = getopt(argc, argv, "i:p:")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'i':
            strcpy(ip, (const char *) optarg) ;
            passedI = true;
            break;
        case 'p':
            prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            break;
        case '?':
            cerr<<"Unrecognised option: -"<<optopt<<'\n';
            Usage();
            exit(1);
        }
    }

    if(!(passedI && passedP)) { Usage(); exit(1); }

    ofstream rs[max_data_ids];
    for(unsigned int s = 0; s < max_data_ids; s++) {
        char x[64];
        sprintf(x,"/tmp/rs_%d",s);
        rs[s].open(x,std::ios::binary | std::ios::out);
    }

//===================== data source setup ===================================
    int udpSocket, nBytes;
    struct sockaddr_in srcAddr;
    struct sockaddr_storage srcRcvBuf;
    socklen_t addr_size;

    /*Create UDP socket for reception from sender */
    udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

    /*Configure settings in address struct*/
    srcAddr.sin_family = AF_INET;
    srcAddr.sin_port = htons(prt); // "LB"
    srcAddr.sin_addr.s_addr = inet_addr(ip); //indra-s2
    //srcAddr.sin_addr.s_addr = INADDR_ANY;
    memset(srcAddr.sin_zero, '\0', sizeof srcAddr.sin_zero);

    /*Bind socket with address struct*/
    bind(udpSocket, (struct sockaddr *) &srcAddr, sizeof(srcAddr));

    /*Initialize size variable to be used later on*/
    addr_size = sizeof srcRcvBuf;

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
    size_t relen =  sizeof(union re);
    char buffer[max_pckt_sz + relen];
    union re* premd = (union re*)buffer; // RE neta data is at front of buffer
    unsigned int seq[max_data_ids];
    for(unsigned int i = 0; i < max_data_ids; i++) seq[i] = 1; // start all data_id streams at seq = 1
    // set up some cachd buffers for out-of-sequence work
    char pckt_cache[max_data_ids][max_ooo_pkts][max_pckt_sz + relen];  // crashes here
    bool pckt_cache_inuse[max_data_ids][max_ooo_pkts];
    unsigned int pckt_sz[max_data_ids][max_ooo_pkts];
    bool data_ids_inuse[max_data_ids];
    bool lst_pkt_rcd[max_data_ids];
    for(unsigned int i = 0; i < max_data_ids; i++) {
        data_ids_inuse[i] = false;
        lst_pkt_rcd[i] = false;
        for(unsigned int j = 0; j < max_ooo_pkts; j++)  {
            pckt_cache_inuse[i][j] = false;
            pckt_sz[i][j] = 0;
        }
    }
    uint16_t num_data_ids = 0;  // number of data_ids encountered in this session
    do {
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        nBytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&srcRcvBuf, &addr_size);

        cerr << "Received " <<  nBytes << " bytes from source\n";
        cerr << "frst = " << premd->remdbf.frst << " / lst = " << premd->remdbf.lst 
            << " / data_id = " << premd->remdbf.data_id << " / seq = " << premd->remdbf.seq << '\n';
        if(premd->remdbf.data_id >= max_data_ids) { cerr << "packet data_id exceeds bounds"; exit(1); }
        data_ids_inuse[premd->remdbf.data_id] = true;
        lst_pkt_rcd[premd->remdbf.data_id] = (premd->remdbf.lst == 1);
        if(premd->remdbf.seq == seq[premd->remdbf.data_id]) { //the seq # we were expecting
            cerr << "writing seq " <<  premd->remdbf.seq << " size = " << int(nBytes-relen) << endl;
            rs[premd->remdbf.data_id].write((char*)&buffer[relen], nBytes-relen);
            rs[premd->remdbf.data_id].flush();
            while(pckt_cache_inuse[premd->remdbf.data_id][++seq[premd->remdbf.data_id]] == true) { // while we can find cached packets
                union re* premd1 = (union re*)pckt_cache[premd->remdbf.data_id][seq[premd->remdbf.data_id]];
                cerr << "writing seq " <<  premd1->remdbf.seq << " from slot " << seq[premd->remdbf.data_id] 
                     << " size = " << pckt_sz[seq[premd->remdbf.data_id]]-relen << endl;
                rs[premd->remdbf.data_id].write((char*)&pckt_cache[premd->remdbf.data_id][seq[premd->remdbf.data_id]][relen], pckt_sz[premd->remdbf.data_id][seq[premd->remdbf.data_id]]-relen);
                rs[premd->remdbf.data_id].flush();
                pckt_cache_inuse[premd->remdbf.data_id][seq[premd->remdbf.data_id]] = false;
            }
        } else { // out of expected order - save packet in associated slot
            if(premd->remdbf.seq >= max_ooo_pkts) { cerr << "out of order packet seq exceeds bounds"; exit(1); }	
            memmove(pckt_cache[premd->remdbf.data_id][premd->remdbf.seq], buffer, nBytes);
            pckt_sz[premd->remdbf.data_id][premd->remdbf.seq] = nBytes;
            pckt_cache_inuse[premd->remdbf.data_id][premd->remdbf.seq] = true;
            cerr << "Received packet out of sequence: expected " <<  seq[premd->remdbf.data_id] << " recd " << premd->remdbf.seq << '\n';
            cerr << "store pckt " <<  premd->remdbf.seq << " in slot " << premd->remdbf.seq << endl;
        }
    } while(cnt_trues(data_ids_inuse, max_data_ids) != cnt_trues(lst_pkt_rcd, max_data_ids));
    return 0;
}
