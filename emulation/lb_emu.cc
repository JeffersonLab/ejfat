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

const size_t max_pckt_sz = 9000-20-8;  // = MTU - IP header - UDP header
const size_t lblen       = 16;
const size_t relen       = 8;
const size_t mdlen       = lblen + relen;


void   Usage(void)
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address \n\
        -p listen port   \n\
        -t send address   \n\
        -r send base port \n\
        -e num base port entropy bits (0) \n\
        -v verbose mode (quiet)  \n\
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
        passed6=false, passedV=false, passedE=false;

    char     re_lstn_ip[INET6_ADDRSTRLEN], dst_ip[INET6_ADDRSTRLEN]; // listening, target ip
    uint16_t re_lstn_prt = 0x4c42, dst_prt;                          // listening, target ports
    uint8_t  nm_entrp_bts = 0;                                       // number of entropy bits

    while ((optc = getopt(argc, argv, "i:p:t:r:e:6v")) != -1)
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
            strcpy(re_lstn_ip, (const char *) optarg) ;
            passedI = true;
            fprintf(stdout, "-i %s ", re_lstn_ip);
            break;
        case 'p':
            re_lstn_prt = (uint16_t) atoi((const char *) optarg) ;
            passedP = true;
            fprintf(stdout, "-p %d", re_lstn_prt);
            break;
         case 't':
            strcpy(dst_ip, (const char *) optarg) ;
            passedT = true;
            fprintf(stdout, "-t %s ", dst_ip);
            break;
        case 'r':
            dst_prt = (uint16_t) atoi((const char *) optarg) ;
            passedR = true;
            fprintf(stdout, "-r %d ", dst_prt);
            break;
        case 'e':
            nm_entrp_bts = (uint8_t) atoi((const char *) optarg) ;
            passedE = true;
            fprintf(stdout, "-e %d ", nm_entrp_bts);
            break;
        case 'v':
            passedV = true;
            fprintf(stdout, "-v ");
            break;
        case '?':
            fprintf(stdout, "Unrecognised option: %d\n", optopt);
            Usage();
            exit(1);
        }
    }
    fprintf(stdout, "\n");
    if(!(passedI && passedP && passedT && passedR)) { Usage(); exit(1); }

//===================== data reception setup ===================================
    int re_lstn_sckt, nBytes;
    socklen_t addr_size;
    struct sockaddr_storage src_addr;

    if (passed6) {
        struct sockaddr_in6 re_lstn_addr6;

        /*Create UDP socket for reception from sender */
        if ((re_lstn_sckt = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
            perror("creating src socket");
            exit(1);
        }

        /* increase UDP receive buffer size */
        int recvBufSize = 0;
#ifdef __APPLE__
        // By default set recv buf size to 7.4 MB which is the highest
        // it wants to go before before reverting back to 787kB.
        recvBufSize = 7400000;
#else
        // By default set recv buf size to 25 MB
        recvBufSize = 25000000;
#endif
        setsockopt(re_lstn_sckt, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));

        /*Configure settings in address struct*/
        /* clear it out */
        memset(&re_lstn_addr6, 0, sizeof(re_lstn_addr6));
        /* it is an INET address */
        re_lstn_addr6.sin6_family = AF_INET6; 
        /* the port we are going to send to, in network byte order */
        re_lstn_addr6.sin6_port = htons(re_lstn_prt);           // "LB" = 0x4c42 by spec (network order)
        /* the server IP address, in network byte order */
        inet_pton(AF_INET6, re_lstn_ip, &re_lstn_addr6.sin6_addr);  // LB address
        ///bind(re_lstn_sckt, (struct sockaddr *) &re_lstn_addr6, sizeof(re_lstn_addr6));
        bind(re_lstn_sckt, (struct sockaddr *) &re_lstn_addr6, sizeof(re_lstn_addr6));
    } else {
        struct sockaddr_in re_lstn_addr;
        socklen_t addr_size;

        /*Create UDP socket for reception from sender */
        re_lstn_sckt = socket(PF_INET, SOCK_DGRAM, 0);

            /* increase UDP receive buffer size */
            int recvBufSize = 25000000;
            setsockopt(re_lstn_sckt, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));

        /*Configure settings in address struct*/
        re_lstn_addr.sin_family = AF_INET;
        re_lstn_addr.sin_port = htons(re_lstn_prt); // "LB"
        re_lstn_addr.sin_addr.s_addr = inet_addr(re_lstn_ip); //indra-s2
        memset(re_lstn_addr.sin_zero, '\0', sizeof re_lstn_addr.sin_zero);

        /*Bind socket with address struct*/
        bind(re_lstn_sckt, (struct sockaddr *) &re_lstn_addr, sizeof(re_lstn_addr));
    }
 
    /*Initialize size variable to be used later on*/
    addr_size = sizeof src_addr;

//===================== data destination setup ===================================
    uint16_t nm_rcv_prts = 1 << nm_entrp_bts;  //int(std::power(2,nm_entrp_bts));
    fprintf(stdout, "\nnm_rcv_prts %d\n", nm_rcv_prts);
    int dst_sckt[nm_rcv_prts];
    struct sockaddr_in6 dst_addr6[nm_rcv_prts];
    struct sockaddr_in dst_addr[nm_rcv_prts];

    if (passed6) {
        for(uint16_t k=0; k<nm_rcv_prts; k++) {
            /* create a DGRAM (UDP) socket in the INET/INET6 protocol */
            if ((dst_sckt[k] = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating dst socket");
                exit(1);
            }

            /* Increase recv buf size */
            int recvBufBytes = 25000000;
            setsockopt(dst_sckt[k], SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));

            /*Configure settings in address struct*/
            /* clear it out */
            memset(&dst_addr6[k], 0, sizeof(dst_addr6[k]));
            /* it is an INET address */
            dst_addr6[k].sin6_family = AF_INET6; 
            /* the port we are going to send to, in network byte order */
            dst_addr6[k].sin6_port = htons(dst_prt+k);           // "LB" = 0x4c42 by spec (network order)
            /* the server IP address, in network byte order */
            inet_pton(AF_INET6, dst_ip, &dst_addr6[k].sin6_addr);  // LB address
        }
    } else {
        for(uint16_t k=0; k<nm_rcv_prts; k++) {
            // Create UDP socket for transmission to sender
            dst_sckt[k] = socket(PF_INET, SOCK_DGRAM, 0);

            /* Increase recv buf size */
            int recvBufBytes = 25000000;
            setsockopt(dst_sckt[k], SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));

            // Configure settings in address struct
            dst_addr[k].sin_family = AF_INET;
            dst_addr[k].sin_port = htons(dst_prt+k); // data consumer port to send to
            dst_addr[k].sin_addr.s_addr = inet_addr(dst_ip); // data consumer
            memset(dst_addr[k].sin_zero, '\0', sizeof dst_addr[k].sin_zero);

            // Initialize size variable to be used later on
            socklen_t addr_size = sizeof dst_addr[k];
        }
    }
//=======================================================================

    uint8_t buffer[max_pckt_sz];

    uint8_t*  pBufLb =  buffer;
    uint8_t*  pBufRe = &buffer[lblen];
    uint16_t* pLbEntrp = (uint16_t*) &buffer[lblen-sizeof(uint16_t)-sizeof(uint64_t)];
    uint64_t* pLbTick  = (uint64_t*) &buffer[lblen-sizeof(uint64_t)];
    uint32_t* pReSeq   = (uint32_t*) &buffer[mdlen-sizeof(uint32_t)];
    uint16_t* pReDid   = (uint16_t*) &buffer[mdlen-sizeof(uint32_t)-sizeof(uint16_t)];

    while(1){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        // locate ingress data after lb+re meta data regions
        if ((nBytes = recvfrom(re_lstn_sckt, buffer, max_pckt_sz, 0, 
                        (struct sockaddr *)&src_addr, &addr_size)) < 0) {
            perror("recvfrom src socket");
            exit(1);
        }

        // decode to host encoding
        uint16_t lb_entrp   = ntohs(*pLbEntrp);
        uint64_t lb_tick    = NTOHLL(*pLbTick);
        uint32_t re_seq     = ntohl(*pReSeq);
        uint16_t re_data_id = ntohs(*pReDid);
        uint8_t re_vrsn     = (pBufRe[0] & 0xf0) >> 4;
        uint8_t re_frst     = (pBufRe[1] & 0x02) >> 1;
        uint8_t re_lst      =  pBufRe[1] & 0x01;

        char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
        if (getnameinfo((struct sockaddr*) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV) ) {
            perror("getnameinfo ");
        }
        if(passedV) {
            fprintf( stdout, "Received %d bytes from source %s / %s : ", nBytes, gtnm_ip, gtnm_srvc);
            fprintf( stdout, "l = %c / b = %c ", pBufLb[0], pBufLb[1]);
            fprintf( stdout, "lb_tick = %" PRIu64 " ", lb_tick);
            fprintf( stdout, "lb_entrp = %d ", lb_entrp);
            fprintf( stdout, "re_frst = %d / re_lst = %d ", re_frst, re_lst); 
            fprintf( stdout, " / re_data_id = %d / re_seq = %d\n", re_data_id, re_seq);	
        }
        
        // forward data to sink skipping past lb meta data
        /* now send a datagram */
	    ssize_t rtCd = 0;
        if (passed6) {
            if ((rtCd = sendto(dst_sckt[lb_entrp % nm_rcv_prts], &buffer[lblen], nBytes-lblen, 0, 
                    (struct sockaddr *)&dst_addr6[lb_entrp % nm_rcv_prts], sizeof dst_addr6[lb_entrp % nm_rcv_prts])) < 0) {
                perror("sendto failed");
                exit(4);
            }
        } else {
            if ((rtCd = sendto(dst_sckt[lb_entrp % nm_rcv_prts], &buffer[lblen], nBytes-lblen, 0, 
                    (struct sockaddr *)&dst_addr[lb_entrp % nm_rcv_prts], sizeof dst_addr[lb_entrp % nm_rcv_prts])) < 0) {
                perror("sendto failed");
                exit(4);
            }
    }
    if(passedV) fprintf( stdout, "Sent %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt+(lb_entrp % nm_rcv_prts));

/*** why is this not working ?
        if (getnameinfo((struct sockaddr*) &dst_addr6, sizeof(dst_addr6), gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV) < 0) {
            perror("sendto socket");
            exit(1);
        }
         fprintf( stdout, "Sending %d bytes to %s : %s\n", int(nBytes-lblen), gtnm_ip, gtnm_srvc);
***/
    }

    return 0;
}
