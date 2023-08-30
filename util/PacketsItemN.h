//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_PACKETSITEMN_H
#define UTIL_PACKETSITEMN_H


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


namespace ejfat {

    /**
     * This class defines the UDP packet containing items which are supplied by the SupplierN class.
     * This differs from the PacketsItem class in the following way. This class is made to work
     * with the SupplierN class instead of the Supplier class.
     * This class is able to store information about multiple
     * (up to 8) ring buf consumers that may be using this item simultaneously. It does this by
     * passing the consumerCount on to its base class, SupplyItem.
     * In reality, consumers are programmed to look at different items even tho they have
     * access to all.
     * These classes are designed to work with the linux routine, recvmmsg, which gets many
     * packets in one call. Will compile but not work on MAC.
     *
     * @date 03/27/2023
     * @author timmer
     */
    class PacketsItemN : public SupplyItem {

    public:

        /** Set dynamically by calling {@link #setEventFactorySettings(size_t)}. */
        static size_t factoryPacketCount;
        static uint32_t consumerCount;

    private:

        /** Place to store UDP packet data. */
        struct mmsghdr *packets = nullptr;

        /** Place to store parsed reassembly header info. */
        reHeader *headers = nullptr;

        /** Max number of UDP packets that can be stored. */
        size_t maxPktCount;

        /** Actual number of UDP packets (read from network) stored here. */
        size_t pktsFilled = 0;

    public:

        static void setEventFactorySettings(size_t pktCount, uint32_t consumerCount = 1);
        static const std::function< std::shared_ptr<PacketsItemN> () >& eventFactory();
        static void printPacketItem(std::shared_ptr<PacketsItemN> item, int index);


        PacketsItemN();
        PacketsItemN(const PacketsItemN & item);
        ~PacketsItemN();

        PacketsItemN & operator=(const PacketsItemN & other) = delete;

        struct mmsghdr * getPackets();
        reHeader * getHeaders();

        struct mmsghdr * getPacket(uint32_t index);
        reHeader * getHeader(uint32_t index);
        int getSource(uint32_t index);
        int getRecvFlag(uint32_t index);
        bool dataDiscarded();

        size_t getMaxPacketCount();

        size_t getPacketsFilled();
        void setPacketsFilled(size_t count);

        void reset();
    };
}


#endif // UTIL_PACKETSITEMN_H
