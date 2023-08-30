//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "PacketRefItem.h"


namespace ejfat {

    // Static init
    std::shared_ptr<Supplier<PacketStoreItem>> PacketRefItem::defaultSupply = nullptr;


    /**
     * Method to set PacketsItem parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each PacketsItem, must be passed in as global parameters.
     *
     * @param pktCount number of UDP packets that can be stored in this item.
     */
    void PacketRefItem::setDefaultFactorySettings(std::shared_ptr<Supplier<PacketStoreItem>> supplier) {
        PacketRefItem::defaultSupply = supplier;
    }


    /** Function to create PacketHoldItems by RingBuffer. */
    const std::function< std::shared_ptr<PacketRefItem> () >& PacketRefItem::eventFactory() {
        static std::function< std::shared_ptr<PacketRefItem> () > result([]  {
            return std::make_shared<PacketRefItem>();
        });
        return result;
    }


    /**
     * Print a couple things from the given packet in an PacketRefItem.
     * @param item shared pointer to PacketRefItem to be printed.
     */
    void PacketRefItem::printItem(std::shared_ptr<PacketRefItem> item) {
        if (item == nullptr) {
            fprintf(stderr, "printPacketItem: item arg is null\n");
            return;
        }

        ejfat::printReHeader(item->packet->getHeader());
    }


    /** Default constructor. */
    PacketRefItem::PacketRefItem() : SupplyItem() {
        supply = defaultSupply;
        myId = idValue++;
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    PacketRefItem::PacketRefItem(const PacketRefItem & item) : SupplyItem(item) {
        // Avoid self copy ...
        if (this != &item) {
            supply = item.supply;
            packet = item.packet;
        }
    }


    /** Destructor. */
    PacketRefItem::~PacketRefItem() {}


    /** Method to reset this item each time it is retrieved from the supply. */
    void PacketRefItem::reset() {
        SupplyItem::reset();
        supply = nullptr;
        packet = nullptr;
    }


    /**
     * Set shared pointer to packet data.
     * @param pkt shared pointer to packet data to store internally.
     */
    void PacketRefItem::setPacket(std::shared_ptr<PacketStoreItem> pkt) {packet = pkt;}


    /**
     * Set shared pointer to PacketStoreItem supply.
     * @param sup shared pointer to PacketStoreItem supply to store internally.
     */
    void PacketRefItem::setSupply(std::shared_ptr<Supplier<PacketStoreItem>> sup) {supply = sup;}


    /**
     * Get shared pointer to packet data.
     * @return shared pointer to packet data.
     */
    std::shared_ptr<PacketStoreItem> PacketRefItem::getPacket() {return packet;}


    /**
     * Get shared pointer to packet supply.
     * @return shared pointer to packet supply.
     */
    std::shared_ptr<Supplier<PacketStoreItem>> PacketRefItem::getSupply() {return supply;}


    /**
     * Release the locally stored packet back into its supply for reuse.
     * After calling this, the packet object is not available until set with the
     * {@link #setPacket} method.
     */
    void PacketRefItem::releasePacket() {
        if (packet == nullptr || supply == nullptr) return;
        supply->release(packet);
        packet = nullptr;
    }

}
