//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_PACKETHOLDITEM_H
#define UTIL_PACKETHOLDITEM_H


#include  <memory>
#include <atomic>
#include <functional>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "Supplier.h"
#include "PacketStoreItem.h"
#include "ejfat_assemble_ersap.hpp"


#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif


namespace ejfat {

    /**
     * This class defines a place to store an PacketStoreItem and the Supplier it came from.
     * This holds them temporarily until the packet is used to reconstruct a complete event buffer,
     * then the packet can be released back to the supply and eventually reused.
     *
     * @date 08/16/2023
     * @author timmer
     */
    class PacketRefItem : public SupplyItem {

    private:

        /** Set dynamically by calling {@link #setDefaultFactorySettings}. */
        static std::shared_ptr<Supplier<PacketStoreItem>> defaultSupply;

        /** Shared pointer to item supply which contains packet object. */
        std::shared_ptr<Supplier<PacketStoreItem>> supply;

        /** Shared pointer to item containing UDP packet. */
        std::shared_ptr<PacketStoreItem> packet;

    public:

        static void setDefaultFactorySettings(std::shared_ptr<Supplier<PacketStoreItem>> supplier);
        static const std::function< std::shared_ptr<PacketRefItem> () >& eventFactory();
        static void printItem(std::shared_ptr<PacketRefItem> item);

        PacketRefItem();
        PacketRefItem(const PacketRefItem & item);
        ~PacketRefItem();

        PacketRefItem & operator=(const PacketRefItem & other) = delete;

        void setPacket(std::shared_ptr<PacketStoreItem> pkt);
        void setSupply(std::shared_ptr<Supplier<PacketStoreItem>> sup);

        std::shared_ptr<PacketStoreItem> getPacket();
        std::shared_ptr<Supplier<PacketStoreItem>> getSupply();

        void releasePacket();

        void reset();
    };
}


#endif
