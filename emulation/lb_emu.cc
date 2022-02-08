/************* Load Balancer Emulation  *******************/

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
#include <inttypes.h>

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const size_t max_pckt_sz = 1024;
const size_t lblen       = 12;
const size_t relen       = 8;
const size_t mdlen       = lblen + relen;


void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -i listen address (string)  \n\
        -p listen port (number)  \n\
        -n listen ipv6  \n\
        -t send address (string)  \n\
        -r send port (number)  \n\
        -o send ipv6  \n\
        -h help \n\n";
        fprintf(stdout, "%s", usage_str);
        fprintf(stdout, "Required: -i -p -t -r\n");
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI, passedP, passedT, passedR, passedN, passedO = false;

    char in_ip[INET6_ADDRSTRLEN], out_ip[INET6_ADDRSTRLEN]; // listening, target ip
    uint16_t in_prt = 0x4c42, out_prt;   // listening, target ports

    while ((optc = getopt(argc, argv, "i:p:t:r:no")) != -1)
    {
        switch (optc)
        {
        case 'h':
            Usage();
            exit(1);
        case 'i':
            strcpy(in_ip, (const char *) optarg) ;
            passedI = true;
            break;
        case 'p':
            in_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            break;
        case 'n':
            passedN = true;
            break;
        case 't':
            strcpy(out_ip, (const char *) optarg) ;
            passedT = true;
            break;
        case 'r':
            out_prt = (uint16_t) atoi((const char *) optarg) ;
            passedR = true;
            break;
        case 'o':
            passedO = true;
            break;
        case '?':
            fprintf(stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }

    if(!(passedI && passedP && passedT && passedR)) { Usage(); exit(1); }

//===================== data source setup ===================================
    int lstn_sckt, nBytes;

    struct sockaddr_in6 lstnAddr6;
    struct sockaddr_in lstnAddr;
    struct sockaddr_storage srcRcvBuf;
    socklen_t addr_size;

    /*Create UDP socket for reception from sender */
    if ((lstn_sckt = socket(passedN ? AF_INET6 : AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("creating src socket");
        exit(1);
    }

    /*Configure settings in address struct*/
    if(passedN) {
        /* clear it out */
        memset(&lstnAddr6, 0, sizeof(lstnAddr6));
        /* it is an INET address */
        lstnAddr6.sin6_family = AF_INET6; 
        /* the port we are going to send to, in network byte order */
        lstnAddr6.sin6_port = htons(in_prt);           // "LB" = 0x4c42 by spec (network order)
        /* the server IP address, in network byte order */
        inet_pton(AF_INET6, in_ip, &lstnAddr6.sin6_addr);  // LB address
        bind(lstn_sckt, (struct sockaddr *) &lstnAddr6, sizeof(lstnAddr6));
    } else {
        /* clear it out */
        memset(&lstnAddr, 0, sizeof(lstnAddr));
        /* it is an INET address */
        lstnAddr.sin_family = AF_INET; 
        /* the port we are going to send to, in network byte order */
        lstnAddr.sin_port = htons(in_prt);           // "LB" = 0x4c42 by spec (network order)
        /* the server IP address, in network byte order */
        inet_pton(AF_INET, in_ip, &lstnAddr.sin_addr);  // LB address
        /*Bind socket with address struct*/
        bind(lstn_sckt, (struct sockaddr *) &lstnAddr, sizeof(lstnAddr));
  }


    /*Initialize size variable to be used later on*/
    addr_size = sizeof srcRcvBuf;

//===================== data sink setup ===================================
    int dst_sckt;

    struct sockaddr_in snkAddr;

    /* create a DGRAM (UDP) socket in the INET/INET6 protocol */
    if ((dst_sckt = socket(passedO ? AF_INET6 : AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("creating dst socket");
        exit(1);
    }

    // Configure settings in address struct
    snkAddr.sin_family = passedO ? AF_INET6 : AF_INET;
    snkAddr.sin_port = htons(out_prt);           // Data Sink
    snkAddr.sin_addr.s_addr = inet_addr(out_ip); // Data Sink
    memset(snkAddr.sin_zero, '\0', sizeof snkAddr.sin_zero);

    // Initialize size variable to be used later on
    addr_size = sizeof snkAddr;

//=======================================================================

    uint8_t buffer[mdlen + max_pckt_sz];

    uint8_t* pBuf   = buffer;
    uint8_t* pBufLb = buffer;
    uint8_t* pBufRe = &buffer[lblen];
    uint64_t* pTick = (uint64_t*) &buffer[lblen-sizeof(uint64_t)];
    uint32_t* pSeq  = (uint32_t*) &buffer[mdlen-sizeof(uint32_t)];
    uint16_t* pDid  = (uint16_t*) &buffer[mdlen-sizeof(uint32_t)-sizeof(uint16_t)];

    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on srcRcvBuf variable

        // locate ingress data after lb+re meta data regions
        if ((nBytes = recvfrom(lstn_sckt, buffer, sizeof(buffer), 0, 
                        (struct sockaddr *)&srcRcvBuf, &addr_size)) < 0) {
            perror("recvfrom src socket");
            exit(1);
        }

        // decode to host encoding
        uint64_t tick    = NTOHLL(*pTick);
        uint32_t seq     = ntohl(*pSeq);
        uint16_t data_id = ntohs(*pDid);
        uint8_t vrsn     = (pBufRe[0] & 0xf0) >> 4;
        uint8_t frst     = (pBufRe[1] & 0x02) >> 1;
        uint8_t lst      =  pBufRe[1] & 0x01;

        fprintf( stderr, "Received %d bytes from source: ", nBytes);
        fprintf( stderr, "l = %c / b = %c ", pBufLb[0], pBufLb[1]);
        fprintf( stderr, "tick = %" PRIu64 " ", tick);
        fprintf( stderr, "tick = %" PRIx64 " ", tick);
        fprintf( stderr, "frst = %d / lst = %d ", frst, lst); 
        fprintf( stderr, " / data_id = %d / seq = %d\n", data_id, seq);	
        
        // forward data to sink skipping past lb meta data
        fprintf( stderr, "Sending %d bytes to sink\n", int(nBytes-lblen));
        /* now send a datagram */
        if (sendto(dst_sckt, &buffer[lblen], nBytes-lblen, 0, 
                    (struct sockaddr *)&snkAddr, addr_size) < 0) {
            perror("sendto failed");
            exit(4);
        }
    }

    return 0;
}
