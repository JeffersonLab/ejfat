/************* Load Balancer Emulation  *******************/

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
#include <cinttypes>
#include <netdb.h>
#include <ctime>

#ifdef __linux__
    #define HTONLL(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define NTOHLL(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif

#ifdef __APPLE__
#include <cctype>
#endif

#define btoa(x) ((x)?"true":"false")


const size_t max_pckt_sz = 9000-20-8;  // = MTU - IP header - UDP header
const size_t lblen       = 16;
const size_t relen       = 20;
const size_t mdlen       = lblen + relen;


void   Usage()
{
    char usage_str[] =
        "\nUsage: \n\
        -6 Use IPV6 \n\
        -i listen address (default INADDR_ANY) \n\
        -p listen port (default 19522) \n\
        -t send address   \n\
        -r send base port \n\
        -e num base port entropy bits (0) \n\
        -v verbose mode (quiet)  \n\
        -h help \n\n";
        fprintf(stdout, "%s", usage_str);
        fprintf(stdout, "Required: -t -r\n");
}

int main (int argc, char *argv[])
{
    int optc;

    bool passedI=false, passedP=false, passedT=false, passedR=false, 
        passed6=false, passedV=false, passedE=false;

    char     re_lstn_ip[INET6_ADDRSTRLEN], dst_ip[INET6_ADDRSTRLEN]; // listening, target ip
    re_lstn_ip[0] = 0;
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
    if(!(passedT && passedR)) { Usage(); exit(1); }

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
        if (strlen(re_lstn_ip) > 0) {
            inet_pton(AF_INET6, re_lstn_ip, &re_lstn_addr6.sin6_addr);  // LB address
        }
        else {
            re_lstn_addr6.sin6_addr = in6addr_any;
        }

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
        if (strlen(re_lstn_ip) > 0) {
            fprintf (stderr, "Using addr = %s\n", re_lstn_ip);
            re_lstn_addr.sin_addr.s_addr = inet_addr(re_lstn_ip); //indra-s2
        }
        else {
            fprintf (stderr, "Using INADDR_ANY\n");
            re_lstn_addr.sin_addr.s_addr = INADDR_ANY;
        }

        memset(re_lstn_addr.sin_zero, '\0', sizeof re_lstn_addr.sin_zero);
//fprintf( stdout, "Receiving on port %hu / host %s", re_lstn_prt, re_lstn_ip);

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
//            fprintf( stdout, "sending to port %hu ", (uint16_t)(dst_prt + k));
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
//            fprintf( stdout, "Sending to port %d / host %s", (dst_prt+k), dst_ip);

            // Initialize size variable to be used later on
            socklen_t addr_size = sizeof dst_addr[k];
        }
    }
//=======================================================================

    uint8_t   buffer[max_pckt_sz];
    uint8_t*  pBufLb =  buffer;
    uint8_t*  pBufRe = &buffer[lblen];

    auto* pLbEntrp = (uint16_t*) &buffer[lblen-sizeof(uint16_t)-sizeof(uint64_t)];
    auto* pLbTick  = (uint64_t*) &buffer[lblen-sizeof(uint64_t)];

    auto* pReDid   = (uint16_t*) &buffer[mdlen-sizeof(uint64_t)-2*sizeof(uint32_t)-sizeof(uint16_t)];
    auto* pReOff   = (uint32_t*) &buffer[mdlen-sizeof(uint64_t)-2*sizeof(uint32_t)];
    auto* pReLen   = (uint32_t*) &buffer[mdlen-sizeof(uint64_t)-sizeof(uint32_t)];
    auto* pReTick  = (uint64_t*) &buffer[mdlen-sizeof(uint64_t)];

    while(true){
        // Try to receive any incoming UDP datagram. Address and port of
        //  requesting client will be stored on src_addr variable

        // locate ingress data after lb+re meta data regions
        if ((nBytes = recvfrom(re_lstn_sckt, buffer, max_pckt_sz, 0, 
                        (struct sockaddr *)&src_addr, &addr_size)) < 0) {
            perror("recvfrom src socket");
            exit(1);
        }

        // decode to host encoding
        uint16_t lb_entrp = ntohs(*pLbEntrp);

        if(passedV) {
            uint64_t lb_tick    = NTOHLL(*pLbTick);
            uint32_t re_len     = ntohl(*pReLen);
            uint32_t re_off     = ntohl(*pReOff);
            uint16_t re_data_id = ntohs(*pReDid);
#ifdef __APPLE__
            // Mac's NTOHLL swaps actual arg which is bad since we gotta pass that data on
            uint64_t tick_net   = *pReTick;
            uint64_t re_tick    = NTOHLL(tick_net);
#else
            uint64_t re_tick    = NTOHLL(*pReTick);
#endif
            uint8_t re_vrsn     = (pBufRe[0] >> 4) & 0xf;

            bool re_frst = (re_off == 0);
            // This is only true if last packet is not out of order
            bool re_lst  = (re_off + nBytes - mdlen) >= re_len;

            char gtnm_ip[NI_MAXHOST], gtnm_srvc[NI_MAXSERV];
            if (getnameinfo((struct sockaddr*) &src_addr, addr_size, gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                            sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV) ) {
                perror("getnameinfo ");
            }

            fprintf( stdout, "Received %d bytes from source %s / %s : ", nBytes, gtnm_ip, gtnm_srvc);
            fprintf( stdout, "l = %c / b = %c ", pBufLb[0], pBufLb[1]);
            fprintf( stdout, "lb_tick = %" PRIu64 " ", lb_tick);
            fprintf( stdout, "lb_entrp = %d ", lb_entrp);
            fprintf( stdout, "re_frst = %s / re_lst = %s ", btoa(re_frst), btoa(re_lst));
            fprintf( stdout, " / re_data_id = %hu / re_off = %u / re_len = %u\n", re_data_id, re_off, re_len);
            fprintf( stdout, "re_tick = %" PRIu64 " ", re_tick);
        }
        
        // forward data to sink skipping past lb meta data
        /* now send a datagram */
        ssize_t rtCd = 0;
        int index = lb_entrp % nm_rcv_prts;
        if (passed6) {
            if ((rtCd = sendto(dst_sckt[index], &buffer[lblen], nBytes-lblen, 0,
                    (struct sockaddr *)&dst_addr6[index], sizeof dst_addr6[index])) < 0) {
                perror("sendto failed");
                exit(4);
            }
        } else {
            if ((rtCd = sendto(dst_sckt[index], &buffer[lblen], nBytes-lblen, 0,
                    (struct sockaddr *)&dst_addr[index], sizeof dst_addr[index])) < 0) {
                perror("sendto failed");
                exit(4);
            }
        }
        if(passedV) fprintf( stdout, "Sent %d bytes to %s : %u\n", uint16_t(rtCd), dst_ip, dst_prt+index);

/*** why is this not working ?
        if (getnameinfo((struct sockaddr*) &dst_addr6, sizeof(dst_addr6), gtnm_ip, sizeof(gtnm_ip), gtnm_srvc,
                       sizeof(gtnm_srvc), NI_NUMERICHOST | NI_NUMERICSERV) < 0) {
            perror("sendto socket");
            exit(1);
        }
         fprintf( stdout, "Sending %d bytes to %s : %s\n", int(nBytes-lblen), gtnm_ip, gtnm_srvc);
***/
            // `time_t` is an arithmetic time type
            time_t now;
         
            // Obtain current time
            // `time()` returns the current time of the system as a `time_t` value
            time(&now);
         
            // Convert to local time format and print to stdout
        //    fprintf ( stdout, "Today is %s", ctime(&now));
    }

    return 0;
}
