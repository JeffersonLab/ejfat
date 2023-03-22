//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_PACKETSITEM_H
#define UTIL_PACKETSITEM_H


#include <memory>
#include <atomic>
#include <functional>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "SupplyItem.h"
#include "ejfat_assemble_ersap.hpp"


#ifdef __linux__
    // for recvmmsg
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif

    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


#ifdef __APPLE__

// Put this here so we can compile on MAC
struct mmsghdr {
    struct msghdr msg_hdr;  /* Message header */
    unsigned int  msg_len;  /* Number of received bytes for header */
};

extern int recvmmsg(int sockfd, struct mmsghdr *msgvec, unsigned int vlen,
                    int flags, struct timespec *timeout);

#endif


namespace ejfat {

    /**
     * This class defines the UDP packet containing items which are supplied by the Supplier class.
     *
     * @date 03/13/2023
     * @author timmer
     */
    class PacketsItem : public SupplyItem {

    public:

        /** Set dynamically by calling {@link #setEventFactorySettings(size_t)}. */
        static size_t factoryPacketCount;

    private:


    //            struct iovec {
    //                ptr_t iov_base; /* Starting address */
    //                size_t iov_len; /* Length in bytes */
    //            }


    //     struct msghdr {
    //             void            *msg_name;      /* optional address */
    //             socklen_t       msg_namelen;    /* size of address */
    //             struct          iovec *msg_iov; /* scatter/gather array */
    //             size_t          msg_iovlen;     /* # elements in msg_iov */
    //             void            *msg_control;   /* ancillary data, see below */
    //             size_t          msg_controllen; /* ancillary data buffer len */
    //             int             msg_flags;      /* flags on received message */
    //     };


//        struct mmsghdr {
//            struct msghdr msg_hdr;  /* Message header */
//            unsigned int  msg_len;  /* Number of received bytes for header */
//        };


        /** Place to store UDP packet data. */
        struct mmsghdr *packets;

        /** Place to store parsed reassembly header info. */
        reHeader *headers;


        reHeader hdrs[200];
        char rcvBuf[200][9000];
        char rcvHdr[200][20];
        struct iovec iovecs[400];
        struct mmsghdr msgs[200];


        /** Max number of UDP packets that can be stored. */
        size_t maxPktCount;

        /** Actual number of UDP packets (read from network) stored here. */
        size_t pktsFilled;

    public:

        static void setEventFactorySettings(size_t pktCount);
        static const std::function< std::shared_ptr<PacketsItem> () >& eventFactory();


        PacketsItem();
        PacketsItem(const PacketsItem & item);
        ~PacketsItem();

        PacketsItem & operator=(const PacketsItem & other) = delete;

        struct mmsghdr * getPackets();
        reHeader * getHeaders();

        struct mmsghdr * getPacket(uint32_t index);
        reHeader * getHeader(uint32_t index);
        int getSource(uint32_t index);

        size_t getMaxPacketCount();
        size_t getPacketsFilled();

        void reset();
    };
}


#endif // UTIL_PACKETSITEM_H
