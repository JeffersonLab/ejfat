//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Receive generated data sent by packetBlaster.c program.
 * This assumes there is an emulator or FPGA between this and the sending program.
 */

#include <cstdlib>
#include <iostream>
#include <time.h>
#include <thread>
#include <cmath>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <errno.h>
#include <cinttypes>

#include <sys/types.h>   /* basic system data types */
#include <sys/socket.h>  /* basic socket definitions */
#include <sys/ioctl.h>   /* find broacast addr */
#include <netinet/tcp.h>
#include <netdb.h>

#include "ejfat_assemble_ersap.hpp"

#ifdef __linux__
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #include <sched.h>
    #include <pthread.h>
#endif


using namespace ejfat;


//-----------------------------------------------------------------------
// Be sure to print to stderr as this program pipes data to stdout!!!
//-----------------------------------------------------------------------


#define INPUT_LENGTH_MAX 256


/**
 * Print out help.
 * @param programName name to use for this program.
 */
static void printHelp(char *programName) {
    fprintf(stderr,
            "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n%s\n\n",
            programName,
            "        [-h] [-v] [-ip6]",
            "        [-a <listening IP address (defaults to INADDR_ANY)>]",
            "        [-p <listening UDP port>]",
            "        [-b <internal buffer byte sizez>]",
            "        [-r <UDP receive buffer byte size>]",
            "        [-fifo (Use SCHED_FIFO realtime scheduler for process - linux)]",
            "        [-rr (Use SCHED_RR realtime scheduler for process - linux)]",
            "        [-pri <realtime process priority, default = max>]",
            "        [-f <file for stats>]",
            "        [-s_host <TCP server host>]",
            "        [-s_port <TCP server port>]",
            "        [-s_if <interface (dot-decimal)) to access TCP server thru>]",
            "        [-cores <comma-separated list of cores to run on>]",
            "        [-tpre <tick prescale (1,2, ... expected tick increment for each buffer)>]");

    fprintf(stderr, "        This is an EJFAT UDP packet receiver made to work with packetBlaster.\n");
}


/**
 * Parse all command line options.
 *
 * @param argc          arg count from main().
 * @param argv          arg list from main().
 * @param bufSize       filled with buffer size.
 * @param recvBufSize   filled with UDP receive buffer size.
 * @param tickPrescale  expected increase in tick with each incoming buffer.
 * @param cores         array of core ids on which to run assembly thread.
 * @param port          filled with UDP port to listen on.
 * @param debug         filled with debug flag.
 * @param useIPv6       filled with use IP version 6 flag.
 * @param listenAddr    filled with IP address to listen on.
 * @param filename      filled with name of file in which to write stats.
 * @param server        IP address of TCP server to send buffers to.
 * @param serverPort    port of TCP server to send buffers to.
 * @param serverIP      IP address of interface thru which to talk to TCP server.
 */
static void parseArgs(int argc, char **argv,
                      int* bufSize, int *recvBufSize, int *tickPrescale,
                      int *cores, uint16_t* port, int *rtPriority,
                      bool *debug, bool *useIPv6, bool *useFIFO, bool *useRR,
                      char *listenAddr, char *filename,
                      char *server, uint16_t *serverPort, char *serverIF) {

    int c, i_tmp;
    bool help = false;

    /* 4 multiple character command-line options */
    static struct option long_options[] =
            {             {"tpre",  1, NULL, 1},
                          {"ip6",  0, NULL, 2},
                          {"cores",  1, NULL, 3},
                          {"fifo",  0, NULL, 4},
                          {"rr",  0, NULL, 5},
                          {"pri",  1, NULL, 6},
                          {"s_host",  1, NULL, 7},
                          {"s_port",  1, NULL, 8},
                          {"s_if",  1, NULL, 9},
                          {0,       0, 0,    0}
            };


    while ((c = getopt_long_only(argc, argv, "vhp:b:a:r:f:", long_options, 0)) != EOF) {

        if (c == -1)
            break;

        switch (c) {

            case 'p':
                // PORT
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *port = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'b':
                // BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 10000) {
                    *bufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -b, internal buf size >= 10kB\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'r':
                // UDP RECEIVE BUFFER SIZE
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 220000) {
                    *recvBufSize = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -r, UDP recv buf size >= 220kB\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 'a':
                // LISTENING IP ADDRESS
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "listening IP address is bad\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(listenAddr, optarg);
                break;

            case 'f':
                // output stat file
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "Output file name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(filename, optarg);
                break;

            case 1:
                // Tick prescale
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *tickPrescale = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -tpre, tpre >= 1\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 2:
                // use IP version 6
                fprintf(stderr, "SETTING TO IP version 6\n");
                *useIPv6 = true;
                break;

            case 3:
                // Cores to run on
                if (strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }


                {
                    // split into ints
                    std::string s = optarg;
                    std::string delimiter = ",";

                    size_t pos = 0;
                    std::string token;
                    char *endptr;
                    int index = 0;
                    bool oneMore = true;

                    while ((pos = s.find(delimiter)) != std::string::npos) {
                        //fprintf(stderr, "pos = %llu\n", pos);
                        token = s.substr(0, pos);
                        errno = 0;
                        cores[index] = (int) strtol(token.c_str(), &endptr, 0);

                        if ((token.c_str() - endptr) == 0) {
                            //fprintf(stderr, "two commas next to eachother\n");
                            oneMore = false;
                            break;
                        }
                        index++;
                        //std::cout << token << std::endl;
                        s.erase(0, pos + delimiter.length());
                        if (s.length() == 0) {
                            //fprintf(stderr, "break on zero len string\n");
                            oneMore = false;
                            break;
                        }
                    }

                    if (oneMore) {
                        errno = 0;
                        cores[index] = (int) strtol(s.c_str(), nullptr, 0);
                        if (errno == EINVAL || errno == ERANGE) {
                            fprintf(stderr, "Invalid argument to -cores, need comma-separated list of core ids\n\n");
                            printHelp(argv[0]);
                            exit(-1);
                        }
                        index++;
                        //std::cout << s << std::endl;
                    }
                }
                break;

            case 4:
                // use FIFO realtime scheduler
                if (*useRR) {
                    fprintf(stderr, "Cannot specify both FIFO and RR\n");
                }
                *useFIFO = true;
                break;

            case 5:
                // use RR (round robin) realtime scheduler
                if (*useFIFO) {
                    fprintf(stderr, "Cannot specify both FIFO and RR\n");
                }
                *useRR = true;
                break;

            case 6:
                // Realtime priority
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp >= 1) {
                    *rtPriority = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -pri, pri >= 1\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 7:
                // TCP server host
                if (strlen(optarg) > 100 || strlen(optarg) < 1) {
                    fprintf(stderr, "Invalid arg to -s_host, server host name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(server, optarg);
                break;

            case 8:
                // TCP server port
                i_tmp = (int) strtol(optarg, nullptr, 0);
                if (i_tmp > 1023 && i_tmp < 65535) {
                    *serverPort = i_tmp;
                }
                else {
                    fprintf(stderr, "Invalid argument to -s_port, 1023 < port < 65536\n\n");
                    printHelp(argv[0]);
                    exit(-1);
                }
                break;

            case 9:
                // Dot decimal interface thru which to talk to TCP server
                if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                    fprintf(stderr, "Invalid arg to -s_if, dot decimal interface name too long/short, %s\n\n", optarg);
                    printHelp(argv[0]);
                    exit(-1);
                }
                strcpy(serverIF, optarg);
                break;

            case 'v':
                // VERBOSE
                *debug = true;
                break;

            case 'h':
                help = true;
                break;

            default:
                printHelp(argv[0]);
                exit(2);
        }

    }

    if (help) {
        printHelp(argv[0]);
        exit(2);
    }
}




// Statistics
static volatile uint64_t totalBytes=0, totalPackets=0;
static volatile int cpu=-1;
static std::atomic<uint32_t> droppedPackets;
static std::atomic<uint32_t> droppedTicks;
typedef struct threadStruct_t {
    char filename[101];
} threadStruct;


// Thread to send to print out rates
static void *thread(void *arg) {

    uint64_t packetCount, byteCount;
    uint64_t prevTotalPackets, prevTotalBytes;
    uint64_t currTotalPackets, currTotalBytes;
    // Ignore first rate calculation as it's most likely a bad value
    bool skipFirst = true;


    // File writing stuff
    bool writeToFile = false;
    threadStruct *targ = static_cast<threadStruct *>(arg);
    char *filename = targ->filename;
    FILE *fp;
    if (strlen(filename) > 0) {
        // open file
        writeToFile = true;
        fp = fopen (filename, "w");
        // validate file open for writing
        if (!fp) {
            fprintf(stderr, "file open failed: %s\n", strerror(errno));
            return (NULL);
        }

        // Write column headers
        fprintf(fp, "Sec,PacketRate(kHz),DataRate(MB/s),Dropped,TotalDropped,CPU\n");
    }


    double pktRate, pktAvgRate, dataRate, dataAvgRate, totalRate, totalAvgRate;
    int64_t totalT = 0, time, droppedPkts, totalDroppedPkts = 0, droppedTiks, totalDroppedTiks = 0;
    struct timespec t1, t2, firstT;

    // Get the current time
    clock_gettime(CLOCK_MONOTONIC, &t1);
    firstT = t1;

    while (true) {

        prevTotalBytes   = totalBytes;
        prevTotalPackets = totalPackets;

        // Delay 4 seconds between printouts
        std::this_thread::sleep_for(std::chrono::seconds(4));

        // Read time
        clock_gettime(CLOCK_MONOTONIC, &t2);
        time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
        totalT = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

        currTotalBytes   = totalBytes;
        currTotalPackets = totalPackets;

        if (skipFirst) {
            // Don't calculate rates until data is coming in
            if (currTotalPackets > 0) {
                skipFirst = false;
            }
            firstT = t1 = t2;
            totalT = totalBytes = totalPackets = 0;
            continue;
        }

        // Use for instantaneous rates
        byteCount   = currTotalBytes   - prevTotalBytes;
        packetCount = currTotalPackets - prevTotalPackets;

        // Reset things if #s rolling over
        if ( (byteCount < 0) || (totalT < 0) )  {
            totalT = totalBytes = totalPackets = 0;
            firstT = t1 = t2;
            continue;
        }

        // Dropped stuff rates
        droppedPkts = droppedPackets;
        droppedPackets.store(0);
        totalDroppedPkts += droppedPkts;

        droppedTiks = droppedTicks;
        droppedTicks.store(0);
        totalDroppedTiks += droppedTiks;

        pktRate = 1000000.0 * ((double) packetCount) / time;
        pktAvgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
        if (packetCount == 0 && droppedPkts == 0) {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = 0?/everything? ", pktRate, pktAvgRate);
        }
        else {
            printf(" Packets:  %3.4g Hz,  %3.4g Avg, dropped pkts = %" PRId64 " ", pktRate, pktAvgRate, droppedPkts);
        }
        printf(": Dropped Ticks = %" PRId64 ", total = %" PRId64 "\n", droppedTiks, totalDroppedTiks);

        // Actual Data rates (no header info)
        dataRate = ((double) byteCount) / time;
        dataAvgRate = ((double) currTotalBytes) / totalT;
        printf(" Data:     %3.4g MB/s,  %3.4g Avg, cpu %d, dropped pkts %" PRId64 ", total %" PRId64 "\n",
               dataRate, dataAvgRate, cpu, droppedPkts, totalDroppedPkts);

        totalRate = ((double) (byteCount + HEADER_BYTES*packetCount)) / time;
        totalAvgRate = ((double) (currTotalBytes + HEADER_BYTES*currTotalPackets)) / totalT;
        printf(" Total:    %3.4g MB/s,  %3.4g Avg\n\n", totalRate, totalAvgRate);

        if (writeToFile) {
            fprintf(fp, "%" PRId64 ",%d,%d,%" PRId64 ",%" PRId64 ",%d\n", totalT/1000000, (int)(pktRate/1000), (int)(dataRate),
                    droppedPkts, totalDroppedPkts, cpu);
            fflush(fp);
        }

        t1 = t2;
    }

    fclose(fp);
    return (NULL);
}

//*****************************************************
//  Networking (TCP client) code taken from ET system
//*****************************************************

/**
 * Routine that translates integer error into string.
 *
 * @param err integer error
 * @returns error string
 */
static const char *netStrerror(int err) {
    if (err == 0)
        return("no error");

    if (err == HOST_NOT_FOUND)
        return("Unknown host");

    if (err == TRY_AGAIN)
        return("Temporary error on name server - try again later");

    if (err == NO_RECOVERY)
        return("Unrecoverable name server error");

    if (err == NO_DATA)
        return("No address associated with name");

    return("unknown error");
}


/**
 * This routine tells whether the given ip address is in dot-decimal notation or not.
 *
 * @param ipAddress ip address in string form
 * @param decimals  pointer to array of 4 ints which gets filled with ip address ints if not NULL
 *                  and address is in dotted decimal form (left most first)
 *
 * @returns 1    if address is in dot decimal format
 * @returns 0    if address is <b>NOT</b> in dot decimal format
 */
static int isDottedDecimal(const char *ipAddress, int *decimals) {
    int i[4], j, err, isDottedDecimal = 0;

    if (ipAddress == NULL) return(0);

    err = sscanf(ipAddress, "%d.%d.%d.%d", &i[0], &i[1], &i[2], &i[3]);
    if (err == 4) {
        isDottedDecimal = 1;
        for (j=0; j < 4; j++) {
            if (i[j] < 0 || i[j] > 255) {
                isDottedDecimal = 0;
                break;
            }
        }
    }

    if (isDottedDecimal && decimals != NULL) {
        for (j=0; j < 4; j++) {
            *(decimals++) = i[j];
        }
    }

    return(isDottedDecimal);
}


/**
 * Function to take a string IP address, either an alphabetic host name such as
 * mycomputer.jlab.org or one in presentation format such as 129.57.120.113,
 * and convert it to binary numeric format and place it in a sockaddr_in
 * structure.
 *
 * @param ip_address string IP address of a host
 * @param addr pointer to struct holding the binary numeric value of the host
 *
 * @returns 0    if successful
 * @returns -1   if ip_address is null, out of memory, or
 *               the numeric address could not be obtained/resolved
 */
static int stringToNumericIPaddr(const char *ip_address, struct sockaddr_in *addr) {
    int isDotDecimal;
    struct in_addr      **pptr;
    struct hostent      *hp;

    if (ip_address == NULL) {
        fprintf(stderr, "stringToNumericIPaddr: null argument\n");
        return(-7);
    }

    /*
     * Check to see if ip_address is in dotted-decimal form. If so, use different
     * routines to process that address than if it were a name.
     */
    isDotDecimal = isDottedDecimal(ip_address, NULL);

    if (isDotDecimal) {
        if (inet_pton(AF_INET, ip_address, &addr->sin_addr) < 1) {
            return(-1);
        }
        return(0);
    }

    if ((hp = gethostbyname(ip_address)) == NULL) {
        fprintf(stderr, "stringToNumericIPaddr: hostname error - %s\n", netStrerror(h_errno));
        return(-1);
    }

    pptr = (struct in_addr **) hp->h_addr_list;

    for ( ; *pptr != NULL; pptr++) {
        memcpy(&addr->sin_addr, *pptr, sizeof(struct in_addr));
        break;
    }

    return(0);
}


/**
 * This routine chooses a particular network interface for a TCP socket
 * by having the caller provide a dotted-decimal ip address. Port is set
 * to an ephemeral port.
 *
 * @param fd file descriptor for TCP socket
 * @param ip_address IP address in dotted-decimal form
 *
 * @returns 0    if successful
 * @returns -1   if ip_address is null, out of memory, or
 *               the numeric address could not be obtained/resolved
 */
static int setInterface(int fd, const char *ip_address) {
    int err;
    struct sockaddr_in netAddr;

    memset(&netAddr, 0, sizeof(struct sockaddr_in));

    err = stringToNumericIPaddr(ip_address, &netAddr);
    if (err != 0) {
        return err;
    }
    netAddr.sin_family = AF_INET; /* ipv4 */
    netAddr.sin_port = 0;         /* choose ephemeral port # */

    err = bind(fd, (struct sockaddr *) &netAddr, sizeof(netAddr));
    if (err != 0) perror("error in setInterface: ");
    return 0;
}


/**
 * This routine makes a TCP connection to a server.
 *
 * @param inetaddr    binary numeric address of host to connect to
 * @param interface   interface (dotted-decimal ip address) to connect through
 * @param port        port to connect to
 * @param sendBufSize size of socket's send buffer in bytes
 * @param rcvBufSize  size of socket's receive buffer in bytes
 * @param noDelay     false if socket TCP_NODELAY is off, else it's on
 * @param fd          pointer which gets filled in with file descriptor
 * @param localPort   pointer which gets filled in with local (ephemeral) port number
 *
 * @returns 0                          if successful
 * @returns -1   if out of memory,
 *               if socket could not be created or socket options could not be set, or
 *               if host name could not be resolved or could not connect
 *
 */
static int tcpConnect2(uint32_t inetaddr, const char *interface, unsigned short port,
                      int sendBufSize, int rcvBufSize, bool noDelay, int *fd, int *localPort) {
    int                 sockfd, err;
    const int           on=1;
    struct sockaddr_in  servaddr;


    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "tcpConnect2: socket error, %s\n", strerror(errno));
        return(-1);
    }

    // don't wait for messages to cue up, send any message immediately
    if (noDelay) {
        err = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*) &on, sizeof(on));
        if (err < 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect2: setsockopt error\n");
            return(-1);
        }
    }

    // set send buffer size unless default specified by a value <= 0
    if (sendBufSize > 0) {
        err = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*) &sendBufSize, sizeof(sendBufSize));
        if (err < 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect2: setsockopt error\n");
            return(-1);
        }
    }

    // set receive buffer size unless default specified by a value <= 0
    if (rcvBufSize > 0) {
        err = setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char*) &rcvBufSize, sizeof(rcvBufSize));
        if (err < 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect2: setsockopt error\n");
            return(-1);
        }
    }

    // set the outgoing network interface
    if (interface != NULL && strlen(interface) > 0) {
        err = setInterface(sockfd, interface);
        if (err != 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect2: error choosing network interface\n");
            return(-1);
        }
    }

    memset((void *)&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port   = htons(port);
    servaddr.sin_addr.s_addr = inetaddr;

    if ((err = connect(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr))) < 0) {
        fprintf(stderr, "tcpConnect2: error attempting to connect to server\n");
    }

    /* if there's no error, find & return the local port number of this socket */
    if (err != -1 && localPort != NULL) {
        socklen_t len;
        struct sockaddr_in ss;

        len = sizeof(ss);
        if (getsockname(sockfd, (struct sockaddr *) &ss, &len) == 0) {
            *localPort = (int) ntohs(ss.sin_port);
        }
        else {
            *localPort = 0;
        }
    }

    if (err == -1) {
        close(sockfd);
        fprintf(stderr, "tcpConnect2: socket connect error, %s\n", strerror(errno));
        return(-1);
    }

    if (fd != NULL)  *fd = sockfd;
    return(0);
}


/**
 * This routine makes a TCP connection to a server.
 *
 * @param ip_address  name of host to connect to (may be dotted-decimal)
 * @param interface   interface (dotted-decimal ip address) to connect through
 * @param port        port to connect to
 * @param sendBufSize size of socket's send buffer in bytes
 * @param rcvBufSize  size of socket's receive buffer in bytes
 * @param noDelay     false if socket TCP_NODELAY is off, else it's on
 * @param fd          pointer which gets filled in with file descriptor
 * @param localPort   pointer which gets filled in with local (ephemeral) port number
 *
 * @returns 0    if successful
 * @returns -1   if ip_adress or fd args are NULL, out of memory,
 *               socket could not be created or socket options could not be set,
 *               host name could not be resolved or could not connect
 *
 */
static int tcpConnect(const char *ip_address, const char *interface, unsigned short port,
                      int sendBufSize, int rcvBufSize, bool noDelay, int *fd, int *localPort) {
    int                 sockfd, err=0, isDotDecimal=0;
    const int           on=1;
    struct sockaddr_in  servaddr;
    struct in_addr      **pptr;
    struct hostent      *hp;
    int h_errnop        = 0;

    if (ip_address == NULL || fd == NULL) {
        fprintf(stderr, "tcpConnect: null argument(s)\n");
        return(-1);
    }

    /* Check to see if ip_address is in dotted-decimal form. If so, use different
     * routine to process that address than if it were a name. */
    isDotDecimal = isDottedDecimal(ip_address, NULL);
    if (isDotDecimal) {
        uint32_t inetaddr;
        if (inet_pton(AF_INET, ip_address, &inetaddr) < 1) {
            fprintf(stderr, "tcpConnect: unknown address for host %s\n", ip_address);
            return(-1);
        }
        return tcpConnect2(inetaddr, interface, port, sendBufSize, rcvBufSize, noDelay, fd, localPort);
    }


    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "tcpConnect: socket error, %s\n", strerror(errno));
        return(-1);
    }

    /* don't wait for messages to cue up, send any message immediately */
    if (noDelay) {
        err = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*) &on, sizeof(on));
        if (err < 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect: setsockopt error\n");
            return(-1);
        }
    }

    /* set send buffer size unless default specified by a value <= 0 */
    if (sendBufSize > 0) {
        err = setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, (char*) &sendBufSize, sizeof(sendBufSize));
        if (err < 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect: setsockopt error\n");
            return(-1);
        }
    }

    /* set receive buffer size unless default specified by a value <= 0  */
    if (rcvBufSize > 0) {
        err = setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, (char*) &rcvBufSize, sizeof(rcvBufSize));
        if (err < 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect: setsockopt error\n");
            return(-1);
        }
    }

    /* set the outgoing network interface */
    if (interface != NULL && strlen(interface) > 0) {
        err = setInterface(sockfd, interface);
        if (err != 0) {
            close(sockfd);
            fprintf(stderr, "tcpConnect: error choosing network interface\n");
            return(-1);
        }
    }

    memset((void *)&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port   = htons(port);

    if ((hp = gethostbyname(ip_address)) == NULL) {
        close(sockfd);
        fprintf(stderr, "tcpConnect: hostname error - %s\n", netStrerror(h_errnop));
        return(-1);
    }
    pptr = (struct in_addr **) hp->h_addr_list;

    for ( ; *pptr != NULL; pptr++) {
        memcpy(&servaddr.sin_addr, *pptr, sizeof(struct in_addr));
        if ((err = connect(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr))) < 0) {
            fprintf(stderr, "tcpConnect: error attempting to connect to server, %s\n", strerror(errno));
        }
        else {
            fprintf(stderr, "tcpConnect: connected to server\n");
            break;
        }
    }

    /* if there's no error, find & return the local port number of this socket */
    if (err != -1 && localPort != NULL) {
        socklen_t len;
        struct sockaddr_in ss;

        len = sizeof(ss);
        if (getsockname(sockfd, (struct sockaddr *) &ss, &len) == 0) {
            *localPort = (int) ntohs(ss.sin_port);
        }
        else {
            *localPort = 0;
        }
    }

    if (err == -1) {
        close(sockfd);
        fprintf(stderr, "tcpConnect: socket connect error\n");
        return(-1);
    }

    if (fd != NULL)  *fd = sockfd;
    return(0);
}



/**
 * This routine is a convenient wrapper for the write function
 * which writes a given number of bytes from a single buffer
 * over a single file descriptor. Will write all data unless
 * error occurs.
 *
 * @param fd      file descriptor
 * @param vptr    pointer to buffer to write
 * @param n       number of bytes to write
 *
 * @returns total number of bytes written, else -1 if error (errno is set)
 */
static int tcpWrite(int fd, const void *vptr, int n) {
    int		nleft;
    int		nwritten;
    const char	*ptr;

    ptr = (char *) vptr;
    nleft = n;

    while (nleft > 0) {
        if ( (nwritten = write(fd, (char*)ptr, nleft)) <= 0) {
            if (errno == EINTR) {
                nwritten = 0;		/* and call write() again */
            }
            else {
                return(nwritten);	/* error */
            }
        }

        nleft -= nwritten;
        ptr   += nwritten;
    }
    return(n);
}

//*****************************************************
// End of networking code
//*****************************************************



int main(int argc, char **argv) {

    int udpSocket;
    ssize_t nBytes;
    // Set this to max expected data size
    int bufSize = 1020000;
    int recvBufSize = 0;
    int tickPrescale = 1;
    int rtPriority = 0;
    uint16_t port = 7777;
    uint16_t serverPort = 8888;
    int cores[10];
    bool debug = false;
    bool useIPv6 = false;
    bool useFIFO = false;
    bool useRR = false;

    char listeningAddr[16];
    memset(listeningAddr, 0, 16);
    char filename[101];
    memset(filename, 0, 101);
    char server[101];
    memset(server, 0, 101);
    char interface[16];
    memset(interface, 0, 16);
    bool sendToServer = false;

    for (int i=0; i < 10; i++) {
        cores[i] = -1;
    }

    parseArgs(argc, argv, &bufSize, &recvBufSize, &tickPrescale, cores, &port, &rtPriority, &debug,
              &useIPv6, &useFIFO, &useRR, listeningAddr, filename, server, &serverPort, interface);

#ifdef __linux__

    if (cores[0] > -1) {
        // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);

        if (debug) {
            for (int i=0; i < 10; i++) {
                     std::cerr << "core[" << i << "] = " << cores[i] << "\n";
            }
        }

        for (int i=0; i < 10; i++) {
            if (cores[i] >= 0) {
                std::cerr << "Run reassembly thread on core " << cores[i] << "\n";
                CPU_SET(cores[i], &cpuset);
            }
            else {
                break;
            }
        }
        pthread_t current_thread = pthread_self();
        int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
        if (rc != 0) {
            std::cerr << "Error calling pthread_setaffinity_np: " << rc << std::endl;
        }
    }

    if (useFIFO || useRR) {
        // Using the actual pid will set priority of main thd.
        // Using 0 will set priority of calling thd.
        pid_t myPid = getpid();
        // myPid = 0;

        struct sched_param param;
        int policy = useFIFO ? SCHED_FIFO : SCHED_RR;

        // Set process to correct priority for given scheduler
        int priMax = sched_get_priority_max(policy);
        int priMin = sched_get_priority_min(policy);

        // If error
        if (priMax == -1 || priMin == -1) {
            perror("Error reading priority");
            exit(EXIT_FAILURE);
        }

        if (rtPriority < 1 || rtPriority > priMax) {
            rtPriority = priMax;
        }
        else if (rtPriority < priMin) {
            rtPriority = priMin;
        }

        // Current scheduler policy
        int currPolicy = sched_getscheduler(myPid);
        if (currPolicy < 0) {
            perror("Error reading policy");
            exit(EXIT_FAILURE);
        }
        std::cerr << "Current Scheduling Policy: " << currPolicy <<
                     " (RR = " << SCHED_RR << ", FIFO = " << SCHED_FIFO <<
                     ", OTHER = " << SCHED_OTHER << ")" << std::endl;

        // Set new scheduler policy
        std::cerr << "Setting Scheduling Policy to: " << policy << ", pri = " << rtPriority << std::endl;
        param.sched_priority = rtPriority;
        int errr = sched_setscheduler(myPid, policy, &param);
        if (errr < 0) {
            perror("Error setting scheduler policy");
            exit(EXIT_FAILURE);
        }

        errr = sched_getparam(myPid, &param);
        if (errr < 0) {
            perror("Error getting priority");
            exit(EXIT_FAILURE);
        }

        currPolicy = sched_getscheduler(myPid);
        if (currPolicy < 0) {
            perror("Error reading policy");
            exit(EXIT_FAILURE);
        }

        std::cerr << "New Scheduling Policy: " << currPolicy << ", pri = " <<  param.sched_priority <<std::endl;
    }

#endif


#ifdef __APPLE__
    // By default set recv buf size to 7.4 MB which is the highest
    // it wants to go before before reverting back to 787kB.
    recvBufSize = 7400000;
#else
    // By default set recv buf size to 25 MB
    recvBufSize = recvBufSize <= 0 ? 25000000 : recvBufSize;
#endif

    // Do we connect to a TCP server and send the data there?
    if (strlen(server) > 0 && serverPort > 1023) {
        sendToServer = true;
    }

    if (useIPv6) {
        struct sockaddr_in6 serverAddr6{};

        // Create IPv6 UDP socket
        if ((udpSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
            perror("creating IPv6 client socket");
            return -1;
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);
        if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

        // Configure settings in address struct
        // Clear it out
        memset(&serverAddr6, 0, sizeof(serverAddr6));
        // it is an INET address
        serverAddr6.sin6_family = AF_INET6;
        // the port we are going to receiver from, in network byte order
        serverAddr6.sin6_port = htons(port);
        if (strlen(listeningAddr) > 0) {
            inet_pton(AF_INET6, listeningAddr, &serverAddr6.sin6_addr);
        }
        else {
            serverAddr6.sin6_addr = in6addr_any;
        }

        // Bind socket with address struct
        int err = bind(udpSocket, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
        if (err != 0) {
            if (debug) fprintf(stderr, "bind socket error\n");
            return -1;
        }
    }
    else {
        // Create UDP socket
        if ((udpSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("creating IPv4 client socket");
            return -1;
        }

        // Set & read back UDP receive buffer size
        socklen_t size = sizeof(int);
        setsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, sizeof(recvBufSize));
        recvBufSize = 0;
        getsockopt(udpSocket, SOL_SOCKET, SO_RCVBUF, &recvBufSize, &size);
        fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufSize);

        // Configure settings in address struct
        struct sockaddr_in serverAddr{};
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        if (strlen(listeningAddr) > 0) {
            serverAddr.sin_addr.s_addr = inet_addr(listeningAddr);
        }
        else {
            serverAddr.sin_addr.s_addr = INADDR_ANY;
        }
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

        // Bind socket with address struct
        int err = bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
        if (err != 0) {
            fprintf(stderr, "bind socket error\n");
            return -1;
        }
    }

    // Start thread to do rate printout
    threadStruct *targ = (threadStruct *)malloc(sizeof(threadStruct));
    if (targ == nullptr) {
        fprintf(stderr, "out of mem\n");
        return -1;
    }
    std::memset(targ, 0, sizeof(threadStruct));
    if (strlen(filename) > 0) {
        std::memcpy(targ->filename, filename, sizeof(filename));
    }

    pthread_t thd;
    int status = pthread_create(&thd, NULL, thread, (void *) targ);
    if (status != 0) {
        fprintf(stderr, "\n ******* error creating thread\n\n");
        return -1;
    }

    // Start with offset 0 in very first packet to be read
    uint64_t tick = 0L;
    uint16_t dataId;

    // If bufSize gets too big, it exceeds stack limits, so lets malloc it!
    char *dataBuf = (char *) malloc(bufSize);
    if (dataBuf == NULL) {
        fprintf(stderr, "cannot allocate internal buffer memory of %d bytes\n", bufSize);
        return -1;
    }

    fprintf(stderr, "Internal buffer size = %d bytes\n", bufSize);

    /*
     * Map to hold out-of-order packets.
     * map key = sequence/offset from incoming packet
     * map value = tuple of (buffer of packet data which was allocated), (bufSize in bytes),
     * (is last packet), (is first packet).
     */
    std::map<uint32_t, std::tuple<char *, uint32_t, bool, bool>> outOfOrderPackets;

    // Track cpu by calling sched_getcpu roughly once per sec
    int cpuLoops = 50000;
    int loopCount = cpuLoops;

    // Statistics
    std::shared_ptr<packetRecvStats> stats = std::make_shared<packetRecvStats>();
    droppedTicks.store(0);
    droppedPackets.store(0);

    int tcpSocket = 0;

    if (sendToServer) {
        // Connect to server
        if (tcpConnect(server, interface, (unsigned short) serverPort,
                       recvBufSize, 0, true, &tcpSocket, NULL) != 0) {
            fprintf(stderr, "cannot connect to TCP server\n");
            return -1;
        }
    }


    while (true) {

        clearStats(stats);
        uint64_t diff, prevTick = tick;

        // Fill with data
        nBytes = getCompletePacketizedBuffer(dataBuf, bufSize, udpSocket,
                                             debug, &tick, &dataId, stats,
                                             tickPrescale, outOfOrderPackets);
        if (nBytes < 0) {
            if (nBytes == BUF_TOO_SMALL) {
                fprintf(stderr, "Receiving buffer is too small (%d)\n", bufSize);
            }
            else {
                fprintf(stderr, "Error in getCompletePacketizedBuffer, %ld\n", nBytes);
            }
            close(udpSocket);
            if (tcpSocket > 0) {
                close(tcpSocket);
            }
            return (0);
        }

        diff = tick - prevTick;
        if (diff != 0) {
            fprintf(stderr, "Error in tick increment, %" PRIu64 "\n", diff);
        }

        totalBytes   += nBytes;
        totalPackets += stats->acceptedPackets;

        // atomic
        droppedTicks   += stats->droppedTicks;
        droppedPackets += stats->droppedPackets;

        // The tick returned is what was just built.
        // Now give it the next expected tick.
        tick += tickPrescale;

#ifdef __linux__
        if (loopCount-- < 1) {
            cpu = sched_getcpu();
            loopCount = cpuLoops;
        }
#endif

        // Send to server if desired
        if (sendToServer) {
            // We first send the size of the buffer in bytes (in BIG endian).
            int32_t bytes = nBytes;
            bytes = htonl(bytes);

            if (tcpWrite(tcpSocket, (void *) &bytes, 4) != 4) {
                close(udpSocket);
                close(tcpSocket);
                return(-1);
            }

            // Then we send the buffer itself
            if (tcpWrite(tcpSocket, (void *) &dataBuf, nBytes) != nBytes) {
                close(udpSocket);
                close(tcpSocket);
                return(-1);
            }
        }

    }

    return 0;
}
