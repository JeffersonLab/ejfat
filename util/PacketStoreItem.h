//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_PACKETSTOREITEM_H
#define UTIL_PACKETSTOREITEM_H


#include  <memory>
#include <atomic>
#include <functional>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "SupplyItem.h"
#include "ejfat_assemble_ersap.hpp"


#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


namespace ejfat {

    /**
     * This class defines a single empty UDP packet and associated qunatities
     * which are supplied by the Supplier class.
     *
     * @date 08/16/2023
     * @author timmer
     */
    class PacketStoreItem : public SupplyItem {

    public:
        static const uint32_t size = 10000;

    private:

        /** Place to store UDP packet data. */
        char packet[size];

        /** Length of the event this packet is a part of in bytes. */
        uint32_t eventLength {0};

        /** Length of valid data in this packet in bytes. */
        uint32_t length {0};

        /** Offset of this packet in event data. */
        uint32_t offset {0};

        /** Place to store parsed reassembly header info. */
        reHeader header;

    public:

        static const std::function< std::shared_ptr<PacketStoreItem> () >& eventFactory();
        static void printItem(std::shared_ptr<PacketStoreItem> item);

        PacketStoreItem();
        PacketStoreItem(const PacketStoreItem & item);
        ~PacketStoreItem();

        PacketStoreItem & operator=(const PacketStoreItem & other) = delete;

        char* getPacket();
        reHeader* getHeader();
        uint32_t getPacketOffset();
        uint32_t getEventLength();

        uint32_t getPacketDataLength();
        void setPacketDataLength(uint32_t len);

        void reset();
    };
}


#endif
