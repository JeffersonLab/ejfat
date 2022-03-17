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
#ifndef EJFAT_PACKETIZE_ERSAP_H
#define EJFAT_PACKETIZE_ERSAP_H



#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <getopt.h>
#include <cinttypes>
#include <chrono>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
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
    #define HEADER_BYTES    28
#else
    #define LB_HEADER_BYTES 0
    #define HEADER_BYTES    16
#endif


#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


#ifndef _BYTESWAP_H
    #define _BYTESWAP_H

    static inline uint16_t bswap_16(uint16_t x) {
        return (x>>8) | (x<<8);
    }

    static inline uint32_t bswap_32(uint32_t x) {
        return (bswap_16(x&0xffff)<<16) | (bswap_16(x>>16));
    }

    static inline uint64_t bswap_64(uint64_t x) {
        return (((uint64_t)bswap_32(x&0xffffffffull))<<32) |
                          (bswap_32(x>>32));
    }
#endif

#define btoa(x) ((x)?"true":"false")
#define INPUT_LENGTH_MAX 256



namespace ersap {
namespace ejfat {

    static int getMTU(const char* interfaceName, bool debug) {
        // Default MTU
        int mtu = 1500;

        int sock = socket(PF_INET, SOCK_DGRAM, 0);
        struct ifreq ifr;
        strcpy(ifr.ifr_name, interfaceName);
        if (!ioctl(sock, SIOCGIFMTU, &ifr)) {
            mtu = ifr.ifr_mtu;
            if (debug) fprintf(stderr, "ioctl says MTU = %d\n", mtu);
        }
        else {
            if (debug) fprintf(stderr, "Using default MTU\n");
        }
        close(sock);
        return mtu;
    }


    /**
     * Attempt to set the MTU value for UDP packets on the given interface.
     * Miminum 500, maximum 9000.
     *
     * @param interfaceName name of network interface (e.g. eth0).
     * @param sock UDP socket on which to set mtu value.
     * @param mtu the successfully set mtu value or -1 if could not be set.
     * @param debug true for debug output.
     * @return
     */
    static int setMTU(const char* interfaceName, int sock, int mtu, bool debug) {

        if (mtu < 500) {
            mtu = 500;
        }
        if (mtu > 9000) {
            mtu = 9000;
        }

        struct ifreq ifr;
        strcpy(ifr.ifr_name, interfaceName);
        ifr.ifr_mtu = mtu;

        if(!ioctl(sock, SIOCSIFMTU, &ifr)) {
            // Mtu changed successfully
            mtu = ifr.ifr_mtu;
            if (debug) fprintf(stderr, "set MTU to %d\n", mtu);
        }
        else {
            if (!ioctl(sock, SIOCGIFMTU, &ifr)) {
                mtu = ifr.ifr_mtu;
                if (debug) fprintf(stderr, "Failed to set mtu, using default = %d\n", mtu);
            }
            else {
                if (debug) fprintf(stderr, "Using default MTU\n");
                return -1;
            }
        }

#ifdef __linux__
        // For jumbo (> 1500 B) frames we need to set the "no fragment" flag.
        // Only possible on linux, not mac.
        if (mtu > 1500) {
            int val = IP_PMTUDISC_DO;
            setsockopt(sock, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
        }
#endif

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

            *buffer     = 'L';
            *(buffer+1) = 'B';
            *(buffer+2) = version;
            *(buffer+3) = protocol;
            // Put the data in network byte order (big endian)
            *((uint64_t *)(buffer + 4)) = htonll(tick);
        }

    #else

        static void setLbMetadata(char* buffer, uint64_t tick, int version, int protocol) {}

    #endif



        /**
         * <p>Set the Reassembly Header data.
         * The first 16 bits go as ordered. The dataId is put in network byte order.
         * The offset and tick are also put into network byte order.</p>
         * Implemented <b>without</b> using C++ bit fields.
         *
         * @param buffer  buffer in which to write the header.
         * @param first   is this the first packet?
         * @param last    is this the last packet?
         * @param tick    64 bit tick number used to tell the load balancer
         *                which backend host to direct the packet to. Necessary to
         *                disentangle packets from different ticks at one destination
         *                as there may be overlap in time.
         * @param offset  the packet sequence number.
         * @param version the version of this software.
         * @param dataId  the data source id number.
         */
        static void setReMetadata(char* buffer, bool first, bool last,
                                  uint64_t tick, uint32_t offset,
                                  int version, uint16_t dataId) {

            // protocol 'Version:4, Rsvd:10, First:1, Last:1, Data-ID:16, Offset:32'
            // 0                   1                   2                   3
            // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |Version|        Rsvd       |F|L|            Data-ID            |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |                  UDP Packet Offset                            |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
            // |                                                               |
            // +                              Tick                             +
            // |                                                               |
            // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

            buffer[0] = version << 4;
            buffer[1] = (first << 1) + last;

            *((uint16_t *)(buffer + 2)) = htons(dataId);
            *((uint32_t *)(buffer + 4)) = htonl(offset);
            *((uint64_t *)(buffer + 8)) = htonll(tick);
        }


     /** <p>
      * Send a buffer to a given destination by breaking it up into smaller
      * packets and sending these by UDP. This buffer may contain only part
      * of a larger buffer that needs to be sent. This method can then be called
      * in a loop, with the offset arg providing necessary feedback.
      * The receiver is responsible for reassembling these packets back into the original data.</p>
      *
      * Optimize by minimizing copying of data and calling "send" on a connected socket.
      * The very first packet is sent in buffer of copied data.
      * However, for subsequent writes it places the pointer (to read from) HEADER_BYTES before data to be sent,
      * writes the new header there, and then sends.
      * <b>Be warned that the original buffer will be changed after calling this routine!</b>
      * In ERSAP, the packetizer is a terminal service, so we can modify the buffer with the data in it.
      *
      * @param dataBuffer     data to be sent.
      * @param dataLen        number of bytes to be sent.
      * @param maxUdpPayload  maximum number of bytes to place into one UDP packet.
      * @param clientSocket   UDP sending socket.
      * @param tick           value used by load balancer in directing packets to final host.
      * @param protocol       protocol in laad balance header.
      * @param version        version in reassembly header.
      * @param dataId         data id in reassembly header.
      * @param offset         value-result parameter that passes in the sequence number of first packet
      *                       and returns the sequence to use for next packet to be sent.
      * @param delay          delay in millisec between each packet being sent.
      * @param firstBuffer    if true, this is the first buffer to send in a sequence.
      * @param lastBuffer     if true, this is the  last buffer to send in a sequence.
      * @param debug          turn debug printout on & off.
      * @param packetsSent    filled with number of packets sent over network (valid even if error returned).
      *
      * @return 0 if OK, -1 if error when sending packet. Use errno for more details.
      */
    static int sendPacketizedBufferFast(char* dataBuffer, size_t dataLen, int maxUdpPayload,
                                        int clientSocket, uint64_t tick, int protocol,
                                        int version, uint16_t dataId,
                                        uint32_t *offset, uint32_t delay,
                                        bool firstBuffer, bool lastBuffer, bool debug,
                                        int64_t *packetsSent) {

        int err;
        int64_t sentPackets=0;
        size_t bytesToWrite;

        // The very first packet goes in here
        char packetStorage[maxUdpPayload + HEADER_BYTES];
        char *writeHeaderTo = packetStorage;

        // If this packet is the very first packet sent in this series of data buffers(offset = 0)
        bool veryFirstPacket = false;
        // If this packet is the very last packet sent in this series of data buffers
        bool veryLastPacket  = false;

        if (firstBuffer) {
            veryFirstPacket = true;
        }

        uint32_t packetCounter = *offset;
        // Use this flag to allow transmission of a single zero-length buffer
        bool firstLoop = true;

        startAgain:
        while (firstLoop || dataLen > 0) {

            // The number of regular data bytes to write into this packet
            bytesToWrite = dataLen > maxUdpPayload ? maxUdpPayload : dataLen;

            // Is this the very last packet for all buffers?
            if ((bytesToWrite == dataLen) && lastBuffer) {
                veryLastPacket = true;
            }

            if (debug) fprintf(stderr, "Send %lu bytes, last buf = %s, very first = %s, very last = %s\n",
                              bytesToWrite, btoa(lastBuffer), btoa(veryFirstPacket), btoa(veryLastPacket));

            // Write LB meta data into buffer
            setLbMetadata(writeHeaderTo, tick, version, protocol);

            // Write RE meta data into buffer
            setReMetadata(writeHeaderTo + LB_HEADER_BYTES,
                          veryFirstPacket, veryLastPacket,
                          tick, packetCounter++, version, dataId);

            if (firstLoop) {
                // Copy data for very first packet only
                memcpy(writeHeaderTo + HEADER_BYTES, dataBuffer, bytesToWrite);
            }

            // "UNIX Network Programming" points out that a connect call made on a UDP client side socket
            // figures out and stores all the state about the destination socket address in advance
            // (masking, selecting interface, etc.), saving the cost of doing so on every ::sendto call.
            // This book claims that ::send vs ::sendto can be up to 3x faster because of this reduced overhead -
            // data can go straight to the NIC driver bypassing most IP stack processing.
            // In our case, the calling function connected the socket, so we call "send".

            // Send message to receiver
            err = send(clientSocket, writeHeaderTo, bytesToWrite + HEADER_BYTES, 0);
            if (err == -1) {
                if ((errno == EMSGSIZE) && (veryFirstPacket)) {
                    // The UDP packet is too big, so we need to reduce it.
                    // If this is still the first packet, we can try again. Try 20% reduction.
                    maxUdpPayload = maxUdpPayload * 8 / 10;
                    veryLastPacket = false;
                    packetCounter--;
                    if (debug) fprintf(stderr, "\n******************  START AGAIN ********************\n\n");
                    goto startAgain;
                }
                else {
                    // All other errors are unrecoverable
                    *packetsSent = sentPackets;
                    return (-1);
                }
            }

            if (firstLoop) {
                // Switch from local array to writing from dataBuffer for rest of packets
                writeHeaderTo = dataBuffer - HEADER_BYTES;
            }

            sentPackets++;

            // delay if any
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            dataLen -= bytesToWrite;
            writeHeaderTo += bytesToWrite;
            veryFirstPacket = false;
            firstLoop = false;

            if (debug) fprintf(stderr, "Sent pkt %u, remaining bytes = %lu\n\n",
                              (packetCounter - 1), dataLen);
        }

        *offset = packetCounter;
        *packetsSent = sentPackets;
        if (debug) fprintf(stderr, "Set next offset to = %d\n", packetCounter);

        return 0;
    }


    /** <p>
     * Send a buffer to a given destination by breaking it up into smaller
     * packets and sending these by UDP. This buffer may contain only part
     * of a larger buffer that needs to be sent. This method can then be called
     * in a loop, with the offset arg providing necessary feedback.
     * The receiver is responsible for reassembling these packets back into the original data.</p>
     *
     * This routine calls "send" on a connected socket.
     * All data (header and actual data from dataBuffer arg) are copied into a separate
     * buffer and sent. Unlike the {@link #sendPacketizedBufferFastd} routine, the
     * original data is unchanged.
     *
     * @param dataBuffer     data to be sent.
     * @param dataLen        number of bytes to be sent.
     * @param maxUdpPayload  maximum number of bytes to place into one UDP packet.
     * @param clientSocket   UDP sending socket.
     * @param tick           value used by load balancer in directing packets to final host.
     * @param protocol       protocol in laad balance header.
     * @param version        version in reassembly header.
     * @param dataId         data id in reassembly header.
     * @param offset         value-result parameter that passes in the sequence number of first packet
     *                       and returns the sequence to use for next packet to be sent.
     * @param delay          delay in millisec between each packet being sent.
     * @param firstBuffer    if true, this is the first buffer to send in a sequence.
     * @param lastBuffer     if true, this is the  last buffer to send in a sequence.
     * @param debug          turn debug printout on & off.
     * @param packetsSent    filled with number of packets sent over network (valid even if error returned).
     *
     * @return 0 if OK, -1 if error when sending packet. Use errno for more details.
     */
    static int sendPacketizedBufferSend(const char* dataBuffer, size_t dataLen, int maxUdpPayload,
                                        int clientSocket, uint64_t tick, int protocol,
                                        int version, uint16_t dataId,
                                        uint32_t *offset, uint32_t delay,
                                        bool firstBuffer, bool lastBuffer, bool debug,
                                        int64_t *packetsSent) {

        int err;
        int64_t sentPackets=0;
        size_t bytesToWrite, totalDataBytesSent = 0, remainingBytes = dataLen;
        const char *getDataFrom = dataBuffer;
        // Allocate something that'll hold one packet, possibly jumbo.
        // This will have LB and RE headers and the payload data.
        char buffer[10000];

        // If this packet is the very first packet sent in this series of data buffers(offset = 0)
        bool veryFirstPacket = false;
        // If this packet is the very last packet sent in this series of data buffers
        bool veryLastPacket  = false;

        if (firstBuffer) {
            veryFirstPacket = true;
        }

        uint32_t packetCounter = *offset;
        // Use this flag to allow transmission of a single zero-length buffer
        bool firstLoop = true;

        startAgain:
        while (firstLoop || remainingBytes > 0) {

            firstLoop = false;

            // The number of regular data bytes to write into this packet
            bytesToWrite = remainingBytes > maxUdpPayload ? maxUdpPayload : remainingBytes;

            // Is this the very last packet for all buffers?
            if ((bytesToWrite == remainingBytes) && lastBuffer) {
                veryLastPacket = true;
            }

            if (debug) fprintf(stderr, "Send %lu bytes, last buf = %s, very first = %s, very last = %s\n",
                               bytesToWrite, btoa(lastBuffer), btoa(veryFirstPacket), btoa(veryLastPacket));

            // Write LB meta data into buffer
            setLbMetadata(buffer, tick, version, protocol);

            // Write RE meta data into buffer
            setReMetadata(buffer + LB_HEADER_BYTES,
                          veryFirstPacket, veryLastPacket,
                          tick, packetCounter++, version, dataId);

            // This is where and how many bytes to write for data
            memcpy(buffer + HEADER_BYTES, (const void *)getDataFrom, bytesToWrite);

            // "UNIX Network Programming" points out that a connect call made on a UDP client side socket
            // figures out and stores all the state about the destination socket address in advance
            // (masking, selecting interface, etc.), saving the cost of doing so on every ::sendto call.
            // This book claims that ::send vs ::sendto can be up to 3x faster because of this reduced overhead -
            // data can go straight to the NIC driver bypassing most IP stack processing.
            // In our case, the calling function connected the socket, so we call "send".

            // Send message to receiver
            err = send(clientSocket, buffer, bytesToWrite + HEADER_BYTES, 0);
            if (err == -1) {
                if ((errno == EMSGSIZE) && (veryFirstPacket)) {
                    // The UDP packet is too big, so we need to reduce it.
                    // If this is still the first packet, we can try again. Try 20% reduction.
                    maxUdpPayload = maxUdpPayload * 8 / 10;
                    veryLastPacket = false;
                    packetCounter--;
                    if (debug) fprintf(stderr, "\n******************  START AGAIN ********************\n\n");
                    goto startAgain;
                }
                else {
                    // All other errors are unrecoverable
                    *packetsSent = sentPackets;
                    return (-1);
                }
            }

            sentPackets++;

            // delay if any
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            totalDataBytesSent += bytesToWrite;
            remainingBytes -= bytesToWrite;
            getDataFrom += bytesToWrite;
            veryFirstPacket = false;

            if (debug) fprintf(stderr, "Sent pkt %u, total %lu, remaining bytes = %lu\n\n",
                               (packetCounter - 1), totalDataBytesSent, remainingBytes);
        }

        *offset = packetCounter;
        *packetsSent = sentPackets;
        if (debug) fprintf(stderr, "Set next offset to = %d\n", packetCounter);

        return 0;
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
     * @param destination    destination host and port of packets.
     * @param tick           value used by load balancer in directing packets to final host.
     * @param protocol       protocol in laad balance header.
     * @param version        version in reassembly header.
     * @param dataId         data id in reassembly header.
     * @param offset         value-result parameter that passes in the sequence number of first packet
     *                       and returns the sequence to use for next packet to be sent.
     * @param delay          delay in millisec between each packet being sent.
     * @param firstBuffer    if true, this is the first buffer to send in a sequence.
     * @param lastBuffer     if true, this is the last buffer to send in a sequence.
     * @param debug          turn debug printout on & off.
     * @param packetsSent    filled with number of packets sent over network (valid even if error returned).
     *
     * @return 0 if OK, -1 if error when sending packet. Use errno for more details.
     */
        static int sendPacketizedBufferSendto(const char* dataBuffer, size_t dataLen, int maxUdpPayload,
                                              int clientSocket, struct sockaddr_in* destination,
                                              uint64_t tick, int protocol, int version, uint16_t dataId,
                                              uint32_t *offset, uint32_t delay,
                                              bool firstBuffer, bool lastBuffer, bool debug,
                                              int64_t *packetsSent) {

            int err;
            int64_t sentPackets=0;
            size_t bytesToWrite, totalDataBytesSent = 0, remainingBytes = dataLen;
            const char *getDataFrom = dataBuffer;
            // Allocate something that'll hold one packet, possibly jumbo.
            // This will have LB and RE headers and the payload data.
            char buffer[10000];

            // If this packet is the very first packet sent in this series of data buffers(offset = 0)
            bool veryFirstPacket = false;
            // If this packet is the very last packet sent in this series of data buffers
            bool veryLastPacket  = false;

            if (firstBuffer) {
                veryFirstPacket = true;
            }

            uint32_t packetCounter = *offset;
            // Use this flag to allow transmission of a single zero-length buffer
            bool firstLoop = true;

            startAgain:
            while (firstLoop || remainingBytes > 0) {

                firstLoop = false;

                // The number of regular data bytes to write into this packet
                bytesToWrite = remainingBytes > maxUdpPayload ? maxUdpPayload : remainingBytes;

                // Is this the very last packet for all buffers?
                if ((bytesToWrite == remainingBytes) && lastBuffer) {
                    veryLastPacket = true;
                }

                if (debug) fprintf(stderr, "Send %lu bytes, last buf = %s, very first = %s, very last = %s\n",
                                   bytesToWrite, btoa(lastBuffer), btoa(veryFirstPacket), btoa(veryLastPacket));

                // Write LB meta data into buffer
                setLbMetadata(buffer, tick, version, protocol);

                // Write RE meta data into buffer
                setReMetadata(buffer + LB_HEADER_BYTES,
                              veryFirstPacket, veryLastPacket,
                              tick, packetCounter++, version, dataId);

                // This is where and how many bytes to write for data
                memcpy(buffer + HEADER_BYTES, (const void *)getDataFrom, bytesToWrite);

                // Send message to receiver
                err = sendto(clientSocket, buffer, bytesToWrite + HEADER_BYTES, 0,
                             (const struct sockaddr *)destination, sizeof(struct sockaddr_in));
                if (err == -1) {
                    if ((errno == EMSGSIZE) && (veryFirstPacket)) {
                        // The UDP packet is too big, so we need to reduce it.
                        // If this is still the first packet, we can try again. Try 20% reduction.
                        maxUdpPayload = maxUdpPayload * 8 / 10;
                        veryLastPacket = false;
                        packetCounter--;
                        if (debug) fprintf(stderr, "\n******************  START AGAIN ********************\n\n");
                        goto startAgain;
                    }
                    else {
                        // All other errors are unrecoverable
                        *packetsSent = sentPackets;
                        return (-1);
                    }
                }

                sentPackets++;

                // delay if any
                if (delay > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                }

                totalDataBytesSent += bytesToWrite;
                remainingBytes -= bytesToWrite;
                getDataFrom += bytesToWrite;
                veryFirstPacket = false;

                if (debug) fprintf(stderr, "Sent pkt %u, total %lu, remaining bytes = %lu\n\n",
                                   (packetCounter - 1), totalDataBytesSent, remainingBytes);
            }

            *offset = packetCounter;
            *packetsSent = sentPackets;
            if (debug) fprintf(stderr, "Set next offset to = %d\n", packetCounter);

            return 0;
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
     * @param delay          delay in millisec between each packet being sent.
     * @param firstBuffer    if true, this is the first buffer to send in a sequence.
     * @param lastBuffer     if true, this is the last buffer to send in a sequence.
     * @param debug          turn debug printout on & off.
     * @param packetsSent    filled with number of packets sent over network (valid even if error returned).
     *
     * @return 0 if OK, -1 if error when sending packet. Use errno for more details.
     */
    static int sendPacketizedBufferSendmsg(const char* dataBuffer, size_t dataLen, int maxUdpPayload,
                                           int clientSocket, struct sockaddr_in* destination,
                                           uint64_t tick, int protocol, int version, uint16_t dataId,
                                           uint32_t *offset, uint32_t delay,
                                           bool firstBuffer, bool lastBuffer, bool debug,
                                           int64_t *packetsSent) {

        int64_t sentPackets=0;
        int totalDataBytesSent = 0;
        int remainingBytes = dataLen;
        const char *getDataFrom = dataBuffer;
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

        // Use this flag to allow transmission of a single zero-length buffer
        bool firstLoop = true;

        startAgain:
        while (firstLoop || remainingBytes > 0) {

            firstLoop = false;

            // The number of regular data bytes to write into this packet
            bytesToWrite = remainingBytes > maxUdpPayload ? maxUdpPayload : remainingBytes;

            // Is this the very last packet for all buffers?
            if ((bytesToWrite == remainingBytes) && lastBuffer) {
                veryLastPacket = true;
            }

            if (debug) fprintf(stderr, "Send %d bytes, last buf = %s, very first = %s, very last = %s\n",
                               bytesToWrite, btoa(lastBuffer), btoa(veryFirstPacket), btoa(veryLastPacket));

            // Write LB meta data into buffer
            setLbMetadata(headerBuffer, tick, version, protocol);

            // Write RE meta data into buffer
            setReMetadata(headerBuffer + LB_HEADER_BYTES,
                          veryFirstPacket, veryLastPacket,
                          tick, packetCounter++, version, dataId);

            // This is where and how many bytes to write for data
            iov[1].iov_base = (void *)getDataFrom;
            iov[1].iov_len = bytesToWrite;

            // Send message to receiver
            int err = sendmsg(clientSocket, &msg, 0);
            if (err == -1) {
                if ((errno == EMSGSIZE) && (veryFirstPacket)) {
                    // The UDP packet is too big, so we need to reduce it.
                    // If this is still the first packet, we can try again. Try 20% reduction.
                    maxUdpPayload = maxUdpPayload * 8 / 10;
                    veryLastPacket = false;
                    packetCounter--;
                    if (debug) fprintf(stderr, "\n******************  START AGAIN ********************\n\n");
                    goto startAgain;
                }
                else {
                    // All other errors are unrecoverable
                    *packetsSent = sentPackets;
                    return (-1);
                }
            }

            sentPackets++;

            // delay if any
            if (delay > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            totalDataBytesSent += bytesToWrite;
            remainingBytes -= bytesToWrite;
            getDataFrom += bytesToWrite;
            veryFirstPacket = false;

            if (debug) fprintf(stderr, "Sent pkt %u, total %d, remaining bytes = %d\n\n",
                               (packetCounter - 1), totalDataBytesSent, remainingBytes);
        }

        *offset = packetCounter;
        *packetsSent = sentPackets;
        if (debug) fprintf(stderr, "Set next offset to = %d\n", packetCounter);

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
      * @param delay      delay in millisec between each packet being sent.
      * @param debug      turn debug printout on & off.
      * @param fast       if true, call {@link #sendPacketizedBufferFast}, else call
      *                   {@link #sendPacketizedBufferSend}. Be warned that the "Fast"
      *                   routine changes the data in buffer.
      *
      * @return 0 if OK, -1 if error when sending packet. Use errno for more details.
      */
    static int sendBuffer(char *buffer, uint32_t bufLen, std::string & host, const std::string & interface,
                          int mtu, uint16_t port, uint64_t tick, int protocol,
                          int version, uint16_t dataId, uint32_t delay,
                          bool debug, bool fast) {

        if (host.empty()) {
            // Default to sending to local host
            host = "127.0.0.1";
        }

        // Break data into multiple packets of max MTU size.
        // If the mtu was not set, attempt to get it progamatically.
        if (mtu == 0) {
            if (interface.empty()) {
                mtu = getMTU("eth0", debug);
            }
            else {
                mtu = getMTU(interface.c_str(), debug);
            }
        }

        // If we still can't figure this out, set it to a safe value.
        if (mtu == 0) {
            mtu = 1400;
        }

        // 20 bytes = normal IPv4 packet header, 8 bytes = max UDP packet header
        int maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;
        uint32_t offset = 0;
        int64_t packetsSent = 0;

        // Create UDP socket
        int clientSocket = socket(PF_INET, SOCK_DGRAM, 0);

        // Configure settings in address struct
        struct sockaddr_in serverAddr;
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        serverAddr.sin_addr.s_addr = inet_addr(host.c_str());
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

        int err = connect(clientSocket, (const sockaddr *)&serverAddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            if (debug) perror("Error connecting UDP socket:");
            close(clientSocket);
            return err;
        }

        // Set the MTU, which is necessary for jumbo (> 1500, 9000 max) packets. Don't exceed this limit.
        // If trying to set Jumbo packet, set no-fragment flag on linux.
        mtu = setMTU(interface.c_str(), clientSocket, mtu, debug);

        if (debug) fprintf(stderr, "Setting max UDP payload size to %d bytes, MTU = %d\n", maxUdpPayload, mtu);

        if (fast) {
            err = sendPacketizedBufferFast(buffer, bufLen, maxUdpPayload, clientSocket,
                                             tick, protocol, version, dataId, &offset, delay,
                                             true, true, debug, &packetsSent);
        }
        else {
            err = sendPacketizedBufferSend(buffer, bufLen, maxUdpPayload, clientSocket,
                                           tick, protocol, version, dataId, &offset, delay,
                                           true, true, debug, &packetsSent);
        }
         close(clientSocket);
         return err;
     }

}
}

#endif // EJFAT_PACKETIZE_ERSAP_H