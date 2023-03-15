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

// Number of bytes in ERSAP reassembly header
#define RE_HEADER_BYTES 20


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

        static size_t factoryPacketCount;


        // Structure to hold reassembly header info
        typedef struct reHeader_t {
            uint8_t  version;
            uint16_t dataId;
            uint32_t offset;
            uint32_t length;
            uint64_t tick;
        } reHeader;


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


        /** Place to store UDP packet data. */
        struct mmsghdr *packets;

        /** Place to store parsed reassembly header info. */
        reHeader *headers;

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

        struct mmsghdr * getPacket(uint32_t index);
        reHeader * getHeader(uint32_t index);

        size_t getMaxPacketCount();
        size_t getPacketsFilled();

        void reset();
    };

}


#endif // UTIL_PACKETSITEM_H
