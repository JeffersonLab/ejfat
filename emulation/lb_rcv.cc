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

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const unsigned int max_pckt_sz = 1024;

#include <fstream>
#include <iostream>

using namespace std;

int main () 
{
    ofstream f2("/dev/stdout",std::ios::binary | std::ios::out);
//===================== data source setup ===================================
    int udpSocket, nBytes;
    struct sockaddr_in srcAddr;
    struct sockaddr_storage srcRcvBuf;
    socklen_t addr_size;

    /*Create UDP socket for reception from sender */
    udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

    /*Configure settings in address struct*/
    srcAddr.sin_family = AF_INET;
    srcAddr.sin_port = htons(7777); // "LB"
    srcAddr.sin_addr.s_addr = inet_addr("129.57.29.232"); //indra-s2
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
    unsigned int seq = 1;
    // set up some cachd buffers for out-of-sequence work
    char pckt_cache[100][max_pckt_sz + relen];
    bool pckt_cache_inuse[100] = {false};
    unsigned int pckt_sz[100];
    unsigned int pci = 0; //packet cache index
    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        nBytes = recvfrom(udpSocket, buffer, sizeof(buffer), 0, (struct sockaddr *)&srcRcvBuf, &addr_size);

        cerr << "Received " <<  nBytes << " bytes from source for seq # " << premd->remdbf.seq << '\n';
        if(premd->remdbf.seq == seq) { //the seq # we were expecting
            cerr << "writing seq " <<  premd->remdbf.seq << " size = " << int(nBytes-relen) << endl;
            f2.write((char*)&buffer[relen], nBytes-relen);
            f2.flush();
            while(pckt_cache_inuse[++seq] == true) { // while we can find cached packets
                union re* premd1 = (union re*)pckt_cache[seq];
                cerr << "writing seq " <<  premd1->remdbf.seq << " from slot " << seq << " size = " << pckt_sz[seq]-relen << endl;
                f2.write((char*)&pckt_cache[seq][relen], pckt_sz[seq]-relen);
                f2.flush();
                pckt_cache_inuse[seq] = false;
            }
        } else { // out of expected order - save packet in associated slot
            memmove(pckt_cache[premd->remdbf.seq], buffer, nBytes);
            pckt_sz[premd->remdbf.seq] = nBytes;
            pckt_cache_inuse[premd->remdbf.seq] = true;
            cerr << "Received packet out of sequence: expected " <<  seq << " recd " << premd->remdbf.seq << '\n';
            cerr << "store pckt " <<  premd->remdbf.seq << " in slot " << premd->remdbf.seq << endl;
        }
    }
    return 0;
}

