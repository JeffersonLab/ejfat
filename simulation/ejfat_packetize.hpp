//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100



/**
 * @file Contains routines to packetize (break into smaller UDP packets)
 * a buffer, adding header information that will direct it to and through
 * a special FPGA router. These packets will eventually be received at a given
 * UDP destination equipped to reassemble it.
 */
#ifndef EJFAT_PACKETIZE_H
#define EJFAT_PACKETIZE_H



#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <getopt.h>
#include <cinttypes>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>

#ifdef __APPLE__
#include <cctype>
#endif



// Is this going to an FPGA or FPGA simulator?
// i.e. will the LB header need to added?
#define ADD_LB_HEADER 1

#ifdef ADD_LB_HEADER
    #define LB_HEADER_BYTES 12
    #define HEADER_BYTES    20
#else
    #define LB_HEADER_BYTES 0
    #define HEADER_BYTES    8
#endif


#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif

#define btoa(x) ((x)?"true":"false")
#define INPUT_LENGTH_MAX 256



namespace ersap {
namespace ejfat {


    /** Union to facilitate unpacking of RE UDP header. */
    union reHeader {
        struct __attribute__((packed))re_hdr {
            uint32_t version    : 4;
            uint32_t reserved   : 10;
            uint32_t first      : 1;
            uint32_t last       : 1;
            uint32_t data_id    : 16;
            uint32_t sequence   : 32;
        } reFields;

        uint32_t reWords[2];
    };


    static int getMTU(const char* interfaceName, bool debug) {
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


    #ifdef ADD_LB_HEADER

        /**
         * Set the Load Balancer header data.
         * The first four bytes go as ordered.
         * The tick goes as a single, network byte ordered, 64-bit int.
         *
         * @param buffer   buffer in which to write the header.
         * @param tick     unsigned 64 bit tick number used to tell the load balancer
         *                 which backend host to direct the packet to.
         * @param version  version of this software.
         * @param protocol protocol this software uses.
         */
        static void setLbMetadata(char* buffer, uint64_t tick, int version, int protocol) {
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

            //if (debug) printf("LB first word = 0x%x\n", firstWord);
            //if (debug) printf("LB tick = 0x%llx (%llu)\n\n", tick, tick);

            // Put the data in network byte order (big endian)
            *buffer     = 'L';
            *(buffer+1) = 'B';
            *(buffer+2) = version;
            *(buffer+3) = protocol;
            *((uint64_t *)(buffer + 4)) = htonll(tick);
        }

    #else

        static void setLbMetadata(char* buffer, uint64_t tick, int version, int protocol) {}

    #endif


    /**
     * Set the Reassembly Header data.
     * The first 16 bits go as ordered. The dataId is put in network byte order.
     * The offset is also put into network byte order.
     * @param buffer  buffer in which to write the header.
     * @param first   is this the first packet?
     * @param last    is this the last packet?
     * @param offset  the packet sequence number.
     * @param version the version of this software.
     * @param dataId  the data source id number.
     */
    static void setReMetadata(char* buffer, bool first, bool last,
                              uint32_t offset, int version, uint16_t dataId) {
        // Put 2 32-bit words in network byte order

        // protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
        // 0                   1                   2                   3
        // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
        // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        // |Version|        Rsvd       |F|L|            Data-ID            |
        // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
        // |                  UDP Packet Offset                            |
        // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

        union reHeader* header = (union reHeader*)buffer;

        header->reFields.version = version;
        header->reFields.first = first;
        header->reFields.last = last;
        header->reFields.data_id = htons(dataId);
        header->reFields.sequence = htonl(offset);
    }



     /**
      * Send a buffer to a given destination by breaking it up into smaller
      * packets and sending these by UDP. This buffer may contain only part
      * of a larger buffer that needs to be sent. This method can then be called
      * in a loop, with the offset arg providing necessary feedback.
      * The receiver is responsible for reassembling these packets back into the original data.
      *
      * @param dataBuffer     data to be sent.
      * @param dataLen        number of bytes to be sent.
      * @param maxUdpPayload  maximum number of bytes to place into one UDP packet.
      * @param clientSocket   UDP sending socket.
      * @param destination    FPGA address.
      * @param tick           value used by load balancer in directing packets to final host.
      * @param protocol       protocol in laad balance header.
      * @param version        version in reassembly header.
      * @param dataId         data id in reassembly header.
      * @param offset         value-result parameter that passes in the sequence number of first packet
      *                       and returns the sequence to use for next packet to be sent.
      * @param firstBuffer    if true, this is the first buffer to send in a sequence.
      * @param lastBuffer     if true, this is the last buffer to send in a sequence.
      * @param debug         turn debug printout on & off.
      *
      * @return 0 if OK, -1 if error when sending packet.
      */
    static int sendPacketizedBuffer(char* dataBuffer, size_t dataLen, int maxUdpPayload,
                                   int clientSocket, struct sockaddr_in* destination,
                                   uint64_t tick, int protocol, int version, uint16_t dataId, uint32_t *offset,
                                   bool firstBuffer, bool lastBuffer, bool debug) {

        int totalDataBytesSent = 0;
        int remainingBytes = dataLen;
        char *getDataFrom = dataBuffer;
        int bytesToWrite;
        char headerBuffer[HEADER_BYTES];

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

        // If this packet is the very first packet sent in this series of data buffers(offset = 0)
        bool veryFirstPacket = false;
        // If this packet is the very last packet sent in this series of data buffers
        bool veryLastPacket  = false;

        if (firstBuffer) {
            veryFirstPacket = true;
        }

        uint32_t packetCounter = *offset;

        // This is where and how many bytes to write for a packet's combined LB and RE headers
        iov[0].iov_base = (void *)headerBuffer;
        iov[0].iov_len = HEADER_BYTES;

        startAgain:
        while (remainingBytes > 0) {

            // The number of regular data bytes to write into this packet
            bytesToWrite = remainingBytes > maxUdpPayload ? maxUdpPayload : remainingBytes;

            // Is this the very last packet for all buffers?
            if ((bytesToWrite == remainingBytes) && lastBuffer) {
                veryLastPacket = true;
            }

            if (debug) printf("Send %d bytes, last buf = %s, very first = %s, very last = %s\n",
                              bytesToWrite, btoa(lastBuffer), btoa(veryFirstPacket), btoa(veryLastPacket));

            // Write LB meta data into buffer
            setLbMetadata(headerBuffer, tick, version, protocol);

            // Write RE meta data into buffer
            setReMetadata(headerBuffer + LB_HEADER_BYTES, veryFirstPacket, veryLastPacket,
                          packetCounter++, version, dataId);

            // This is where and how many bytes to write for data
            iov[1].iov_base = (void *)getDataFrom;
            iov[1].iov_len = bytesToWrite;

            // Send message to receiver
            int err = sendmsg(clientSocket, &msg, 0);
            if (err == -1) {
                // All other errors are unrecoverable
                if (debug) perror("error sending message: ");
                if (debug) printf("total msg len = %d\n", (bytesToWrite + HEADER_BYTES));

                if ((errno == EMSGSIZE) && (veryFirstPacket)) {
                    // The UDP packet is too big, so we need to reduce it.
                    // If this is still the first packet, we can try again. Try 10% reduction.
                    maxUdpPayload = maxUdpPayload * 9 / 10;
                    veryLastPacket = false;
                    packetCounter--;
                    if (debug) printf("\n******************  START AGAIN ********************\n\n");
                    goto startAgain;
                }
                else {
                    // All other errors are unrecoverable
                    return (-1);
                }
            }

            totalDataBytesSent += bytesToWrite;
            remainingBytes -= bytesToWrite;
            getDataFrom += bytesToWrite;
            veryFirstPacket = false;

            if (debug) printf("Sent pkt, total %d, remaining bytes = %d\n\n", totalDataBytesSent, remainingBytes);
        }

        *offset = packetCounter;
        if (debug) printf("Set next offset to = %d\n", packetCounter);

        return 0;
    }


     /**
      * Send this entire buffer to the host and port of an FPGA-based load balancer.
      * This is done by breaking it up into smaller size UDP packets - each with 2 headers.
      * The first header is meta data used by the load balancer and stripped off before
      * the data reaches its final destination.
      * The second allows for its reassembly by the receiver.
      *
      * @param buffer     data to be sent.
      * @param bufLen     number of bytes to be sent.
      * @param host       IP address of the host to send to (defaults to loopback).
      * @param interface  name if interface of outgoing packets (defaults to eth0).
      *                   This is used to find the MTU.
      * @param mtu        the max number of bytes to send per UDP packet,
      *                   which includes IP and UDP headers.
      * @param port       UDP port to send to.
      * @param tick       tick value for Load Balancer header used in directing packets to final host.
      * @param version    version in reassembly header.
      * @param dataId     data id in reassembly header.
      * @param debug     turn debug printout on & off.
      *
      * @return 0 if OK, -1 if error when sending packet.
      */
    static int sendBuffer(char *buffer, uint32_t bufLen, std::string & host, const std::string & interface,
                          int mtu, unsigned short port, uint64_t tick, int protocol,
                          int version, uint16_t dataId, bool debug) {

        if (host.empty()) {
            // Default to sending to local host
            host = "127.0.0.1";
        }

        // Break data into multiple packets of max MTU size.
        // If the mtu was not set, attempt to get it progamatically.
        if (mtu == 0) {
            if (interface.empty()) {
                //interface = "eth0";
                mtu = getMTU("eth0", debug);
            }
            else {
                mtu = getMTU(interface.c_str(), debug);
            }
        }

        // Jumbo (> 1500) ethernet frames are 9000 bytes max. Don't exceed this limit.
        if (mtu > 9000) {
            mtu = 9000;
        }
        else if (mtu == 0) {
            // If we still can't figure this out, set it to a safe value.
            mtu = 1400;
        }

        // 60 bytes = max IPv4 packet header, 8 bytes = max UDP packet header
        int maxUdpPayload = mtu - 60 - 8 - HEADER_BYTES;
        uint32_t offset = 0;

        // Create UDP socket
        int clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

        // Configure settings in address struct
        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        serverAddr.sin_addr.s_addr = inet_addr(host.c_str());
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);


        if (debug) printf("Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

        int err =  sendPacketizedBuffer(buffer, bufLen, maxUdpPayload, clientSocket, &serverAddr,
                                        tick, protocol, version, dataId, &offset, true, true, debug);
        return err;
    }

}
}

#endif // EJFAT_PACKETIZE_H
