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
#include <netdb.h>

#define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
#define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))

#ifdef __APPLE__
#include <ctype.h>
#endif

const size_t max_pckt_sz = 1024;
const size_t lblen       = 12;
const size_t relen       = 8+8;
const size_t mdlen       = lblen + relen;


void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (string)  \n\
        -p listen port (number)  \n\
        -t send address (string)  \n\
        -r send port (number)  \n\
        -h help \n\n";
        fprintf(stdout, "%s", usage_str);
        fprintf(stdout, "Required: -i -p -t -r\n");
}

int main (int argc, char *argv[])
{
    int optc;
    extern char *optarg;
    extern int   optind, optopt;

    bool passedI=false, passedP=false, passedT=false, passedR=false, 
        passed6=false, passedV=false;

    char     lstn_ip[INET6_ADDRSTRLEN], dst_ip[INET6_ADDRSTRLEN]; // listening, target ip
    uint16_t lstn_prt = 0x4c42, dst_prt;                          // listening, target ports

    while ((optc = getopt(argc, argv, "i:p:t:r:6v")) != -1)
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
        case 'v':
            passedV = true;
            break;
        case '?':
            fprintf(stderr, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }

    if(!(passedI && passedP && passedT && passedR)) { Usage(); exit(1); }

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
 
    /*Initialize size variable to be used later on*/
    addr_size = sizeof src_addr;

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
//=======================================================================

    uint8_t buffer[mdlen + max_pckt_sz];

    uint8_t*  pBufLb =  buffer;
    uint8_t*  pBufRe = &buffer[lblen];
    uint64_t* pTick   = (uint64_t*) &buffer[lblen-sizeof(uint64_t)];
    uint64_t* pReTick = (uint64_t*) &buffer[mdlen-sizeof(uint64_t)];
    uint32_t* pSeq    = (uint32_t*) &buffer[mdlen-sizeof(uint64_t)-sizeof(uint32_t)];
    uint16_t* pDid    = (uint16_t*) &buffer[mdlen-sizeof(uint64_t)-sizeof(uint32_t)-sizeof(uint16_t)];

    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        // locate ingress data after lb+re meta data regions
        if ((nBytes = recvfrom(lstn_sckt, buffer, mdlen + max_pckt_sz, 0, 
                        (struct sockaddr *)&src_addr, &addr_size)) < 0) {
            perror("recvfrom src socket");
            exit(1);
        }

        // decode to host encoding
        uint64_t tick    = NTOHLL(*pTick);
        uint64_t retick  = NTOHLL(*pReTick);
        uint32_t seq     = ntohl(*pSeq);
        uint16_t data_id = ntohs(*pDid);
        uint8_t vrsn     = (pBufRe[0] & 0xf0) >> 4;
        uint8_t frst     = (pBufRe[1] & 0x02) >> 1;
        uint8_t lst      =  pBufRe[1] & 0x01;

        char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV) ) {
            perror("getnameinfo ");
        }
        if(passedV) {
            fprintf( stderr, "Received %d bytes from source %s / %s : ", nBytes, gtnm_ip, gtnm_srvc);
            fprintf( stderr, "l = %c / b = %c ", pBufLb[0], pBufLb[1]);
            fprintf( stderr, "tick = %" PRIu64 " ", tick);
            fprintf( stderr, "frst = %d / lst = %d ", frst, lst); 
            fprintf( stderr, " / data_id = %d / seq = %d\n", data_id, seq);	
            fprintf( stderr, "tick = %" PRIx64 " ", retick);
        }
        
        // forward data to sink skipping past lb meta data
        /* now send a datagram */
	    ssize_t rtCd = 0;
        if (passed6) {
            if ((rtCd = sendto(dst_sckt, &buffer[lblen], nBytes-lblen, 0, 
                            (struct sockaddr *)&dst_addr6, sizeof dst_addr6)) < 0) {
                perror("sendto failed");
                exit(4);
            }
        } else {
            if ((rtCd = sendto(dst_sckt, &buffer[lblen], nBytes-lblen, 0, 
                            (struct sockaddr *)&dst_addr, sizeof dst_addr)) < 0) {
                perror("sendto failed");
                exit(4);
            }
        }
        if(passedV) fprintf( stderr, "Sent %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt);

/*** why is this not working ?
        if (getnameinfo((struct sockaddr*) &dst_addr6, sizeof(dst_addr6), gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV) < 0) {
            perror("sendto socket");
            exit(1);
        }
         fprintf( stderr, "Sending %d bytes to %s : %s\n", int(nBytes-lblen), gtnm_ip, gtnm_srvc);
***/
    }

    return 0;
}
