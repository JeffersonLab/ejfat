//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "PacketStoreItem.h"


namespace ejfat {

    /** Function to create EmptyPacketItems by RingBuffer. */
    const std::function< std::shared_ptr<PacketStoreItem> () >& PacketStoreItem::eventFactory() {
        static std::function< std::shared_ptr<PacketStoreItem> () > result([]  {
            return std::make_shared<PacketStoreItem>();
        });
        return result;
    }


    /**
     * Print a couple things from the given packet in an PacketStoreItem.
     * @param item shared pointer to PacketStoreItem to be printed.
     */
    void PacketStoreItem::printItem(std::shared_ptr<PacketStoreItem> item) {
        if (item == nullptr) {
            fprintf(stderr, "printPacketItem: item arg is null\n");
            return;
        }

        ejfat::printReHeader(&(item->header));
    }


    /** Default constructor. */
    PacketStoreItem::PacketStoreItem() : SupplyItem() {
        myId = idValue++;
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    PacketStoreItem::PacketStoreItem(const PacketStoreItem & item) : SupplyItem(item) {
        // Avoid self copy ...
        if (this != &item) {
            offset = item.offset;
            length = item.length;
            header = item.header;
            memcpy(packet, item.packet, size);
        }
    }


    /** Destructor. */
    PacketStoreItem::~PacketStoreItem() {}


    /** Method to reset this item each time it is retrieved from the supply. */
    void PacketStoreItem::reset() {
        SupplyItem::reset();
        offset = 0;
        length = 0;
        ejfat::clearHeader(&header);
    }


    /**
     * Get pointer to packet data.
     * @return pointer to packet data.
     */
    char* PacketStoreItem::getPacket() {return packet;}


    /**
     * Get pointer to struct with reassembly info in it.
     * @return pointer to struct with reassembly info in it.
     */
    reHeader* PacketStoreItem::getHeader() {return &header;}


    /**
     * Get the byte offset into packet of the valid data.
     * @return byte offset into packet of the valid data.
     */
    uint32_t PacketStoreItem::getOffset() {return offset;}


    /**
     * Get the byte length of the valid data.
     * @return byte length of the valid data.
     */
    uint32_t PacketStoreItem::getLength() {return length;}
}
