/************* UDP CLIENT CODE *******************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>


// Add boolean support to C, as type and to print
#include <stdbool.h>
#define btoa(x) ((x)?"true":"false")

static bool debug = 1;


#define LB_HEADER_BYTES   12
#define RE_HEADER_BYTES    8
#define PKT_HEADER_BYTES  20

#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif



/**
   * This method takes a pointer and prints out the desired number of bytes
   * from the given position, in hex.
   *
   * @param data      data to print out
   * @param bytes     number of bytes to print in hex
   * @param label     a label to print as header
   */
static void printBytes(const char *data, uint32_t bytes, const char *label) {

    if (label != NULL) printf("%s:\n", label);

    if (bytes < 1) {
        printf("<no bytes to print ...>\n");
        return;
    }

    int i;
    for (i=0; i < bytes; i++) {
        if (i%10 == 0) {
            printf("\n  Buf(%3d - %3d) =  ", (i+1), (i + 10));
        }
        else if (i%5 == 0) {
            printf("  ");
        }

        // Accessing buf in this way does not change position or limit of buffer
        printf("  0x%02x ", (int)(*((data + i))));
    }

    printf("\n\n");
}



static int getMTU(char* interfaceName) {
    // Default MTU
    int mtu = 1500;

    int sock = socket(PF_INET, SOCK_DGRAM, 0);
    //int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct ifreq ifr;
    strcpy(ifr.ifr_name, interfaceName);
    if (!ioctl(sock, SIOCGIFMTU, &ifr)) {
        mtu = ifr.ifr_mtu;
        if (debug) printf("ioctl says MTU = %d\n", mtu);
    }
    else {
        if (debug) printf("Using default MTU = %d\n", mtu);
    }
    close(sock);
    return mtu;
}



void setLbMetadata(char* buffer, uint64_t tick) {
    // Put 1 32-bit word, followed by 1 64-bit word in network byte order, followed by 32 bits of data

    // protocol 'L:8, B:8, Version:8, Protocol:8, Tick:64'
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |       L       |       B       |    Version    |    Protocol   |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                                                               |
    // +                              Tick                             +
    // |                                                               |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    uint32_t L = 'L';
    uint32_t B = 'B';
    uint32_t version = 1;
    uint32_t protocol = 123;

    int firstWord = L | (B << 8) | (version << 16) | (protocol << 24);

    //if (debug) printf("LB first word = 0x%x\n", firstWord);
    //if (debug) printf("LB tick = 0x%llx (%llu)\n\n", tick, tick);

    // Put the data in network byte order (big endian)
    *((uint32_t *)buffer) = htonl(firstWord);
    *((uint64_t *)(buffer + 4)) = htonll(tick);
}


void setReMetadata(char* buffer, bool first, bool last, uint32_t offsetVal) {
    // Put 2 32-bit words in network byte order

    // protocol 'Version:4, Rsvd:10, First:1, Last:1, ROC-ID:16, Offset:32'
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |Version|        Rsvd       |F|L|            Data-ID            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |                  UDP Packet Offset                            |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    uint32_t version = 1;
    uint32_t dataId  = 0x7777;

    int firstWord = version | (first << 14) | (last << 15) | (dataId << 16);

    if (debug) printf("RE first word = 0x%x\n", firstWord);
    if (debug) printf("RE offset = %u\n", offsetVal);

    // Put the data in network byte order (big endian)
    *((uint32_t *)buffer) = htonl(firstWord);
    *((uint32_t *)(buffer + 4)) = htonl(offsetVal);

}



int sendPacketizedBuffer(char* dataBuffer, size_t dataLen, int maxUdpPayload,
                         int clientSocket, struct sockaddr_in* destination,
                         uint64_t tick) {

    int totalDataBytesSent = 0;
    int remainingBytes = dataLen;
    char *getDataFrom = dataBuffer;
    char go[100];
    int bytesToWrite;
    uint32_t packetCounter = 0;
    char hdrBuffer[PKT_HEADER_BYTES];

    // Prepare a msghdr structure to send 2 buffers with one system call.
    // One buffer has LB and RE headers and the other with data to be sent.
    // Doing things this way also eliminates having to copy all the data.
    // Note that in Linux, "send" and "sendto" are just wrappers for sendmsg
    // that build the struct msghdr for you. So no loss of efficiency to do it this way.
    struct msghdr msg;
    struct iovec  iov[2];

    memset(&msg, 0, sizeof(msg));
    msg.msg_name = (void *) destination;
    msg.msg_namelen = sizeof(struct sockaddr_in);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    bool first = true;
    bool last  = false;

    // This is where and how many bytes to write for a packet's combined LB and RE headers
    iov[0].iov_base = (void *)hdrBuffer;
    iov[0].iov_len = PKT_HEADER_BYTES;

//    int loop = 0;
    startAgain:
    while (remainingBytes > 0) {

        // The number of regular data bytes to write into this packet
        bytesToWrite = remainingBytes > maxUdpPayload ? maxUdpPayload : remainingBytes;

        // Is this the last packet?
        if (bytesToWrite == remainingBytes) last = true;

        if (debug) printf("Send %d bytes, last packet = %s\n", bytesToWrite, btoa(last));

        // Write LB meta data into buffer
        setLbMetadata(hdrBuffer, tick);

        // Write RE meta data into buffer
        setReMetadata(hdrBuffer + LB_HEADER_BYTES, first, last, packetCounter++);

        // This is where and how many bytes to write for data
        iov[1].iov_base = (void *)getDataFrom;
        iov[1].iov_len = bytesToWrite;

//        if (loop++ == 0) {
//            printBytes(getDataFrom, bytesToWrite + 100, "packet");
//        }
//        else {
//            printBytes(getDataFrom, bytesToWrite, "packet");
//        }

//        printf("Send to server by hitting return\n");
//        fgets(go,100,stdin);

        // Send message to receiver
        int err = sendmsg(clientSocket, &msg, 0);
        if (err == -1) {
            // All other errors are unrecoverable
            perror("error sending message: ");

            printf("total msg len = %d\n", (bytesToWrite + PKT_HEADER_BYTES));
            if ((errno == EMSGSIZE) && (first)) {
                // The UDP packet is too big, so we need to reduce it.
                // If this is still the first packet, we can try again. Try 10% reduction.
                maxUdpPayload *= .9;
                last = false;
                packetCounter--;
                printf("\n******************START AGAIN\n\n");
                goto startAgain;
            }
            else {
                // All other errors are unrecoverable
                exit(-1);
            }
        }

        totalDataBytesSent += bytesToWrite;

        remainingBytes -= bytesToWrite;
        getDataFrom += bytesToWrite;

        first = false;

        if (debug) printf("Sent pkt, total %d, remaining bytes = %d\n\n", totalDataBytesSent, remainingBytes);
    }

    return 0;
}



int main11 (int argc, char **argv) {

    uint16_t port = 7891;
    uint16_t tick = 0xc0da;
    const size_t dataLen = 380;
    char buffer[dataLen];

    memset(buffer, 1, 100);
    memset(buffer+100, 2, 100);
    memset(buffer+200, 3, 100);
    memset(buffer+300, 4, 80);

    // Break data into multiple packets of max MTU size
    int mtu = getMTU("lo0");

    if (mtu > 9000) {
        // Jumbo (> 1500) ethernet frames are 9000 bytes max.
        // Don't exceed this limit.
        mtu = 9000;
    }

    // Set this by hand for now
    int maxUdpPayload = 100;

    // Create UDP socket
    int clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

    sendPacketizedBuffer(buffer, dataLen, maxUdpPayload, clientSocket, &serverAddr, tick);

    return 0;
}



/**
 * Doing things this way is like reading a buffer bit-by-bit
 * and passing it off to the parser bit-by-bit
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char **argv) {

    uint16_t port = 7891;
    uint16_t tick = 0xc0da;

    // Break data into multiple packets of max MTU size
    int mtu = getMTU("lo0");

    if (mtu > 9000) {
        // Jumbo (> 1500) ethernet frames are 9000 bytes max.
        // Don't exceed this limit.
        mtu = 9000;
    }

    // 60 bytes = max IPv4 packet header, 8 bytes = max UDP packet header
    // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
    int maxUdpPayload = mtu - 60 - 8 - LB_HEADER_BYTES - RE_HEADER_BYTES;

    if (debug) printf("Setting max UDP payload size to %d bytes\n", maxUdpPayload);

    // Create UDP socket
    int clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

    // Configure settings in address struct
    struct sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);


    // use filename provided as 1st argument (stdin by default)
    bool readingFromFile = false;

    FILE *fp = NULL;
    if (argc > 1) {
        fp = fopen (argv[1], "r");
        readingFromFile = true;
    }
    else {
        fp = stdin;
    }

    // validate file open for reading
    if (!fp) {
        perror ("file open failed");
        return 1;
    }

    // Read from either file or stdin.
    // Reading from file systems, it's efficient to read in blocks of 16MB
    int BUFSIZE = 100000;
    char buf[BUFSIZE];
    ssize_t n = 0;
    // change from file pointer to descriptor
    int fd = fileno(fp);

    while ((n = read(fd, buf, BUFSIZE)) > 0) {
        sendPacketizedBuffer(buf, n, maxUdpPayload, clientSocket, &serverAddr, tick);
    }

    if (n == -1) {
        perror("read: ");
        exit(1);
    }

    if (readingFromFile && (close(fd) == -1)) {
        perror("close");
        exit(1);
    }

    return 0;
}

/* 
 * will egress traffic out with the lowest numbered interface:
 */

//    server_addr.sin_addr.s_addr = INADDR_ANY;

/* 
 * But if you need to explicitely assign your IP address, then you will have to create another sin.addr construct and give it the IP in network byte order then: 
 */

//    destination.sin_addr.s_addr = inet_addr("10.10.10.100");

/* 
 * Then call your sendto to use the struct with the above declaration. Please note that the sendto does have an extra struct argument that allows for this.
 * Added sockaddr *myDestination here as an example 
 */

//   sendto(int fd,const void *msg, int len, unsigned int flags, const struct sockaddr *myDestination, int tolen);
