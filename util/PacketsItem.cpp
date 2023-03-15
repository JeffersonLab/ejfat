//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "PacketsItem.h"


namespace ejfat {


    //--------------------------------
    // STATIC INITIALIZATION
    //--------------------------------

    size_t    PacketsItem::factoryPacketCount {200};

//    uint64_t  SupplyItem::idValue = 0;
//    bool      SupplyItem::factoryOrderedRelease = false;


    /**
     * Parse the reassembly header at the start of the given buffer.
     * Return parsed values in pointer arg.
     *
     * <pre>
     *  protocol 'Version:4, Rsvd:12, Data-ID:16, Offset:32, Length:32, Tick:64'
     *
     *  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |Version|        Rsvd           |            Data-ID            |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                         Buffer Offset                         |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                         Buffer Length                         |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |                                                               |
     *  +                             Tick                              +
     *  |                                                               |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer to parse.
     * @param header   pointer to struct to be filled with RE header info.
     */
    static void parseReHeader(const char* buffer, PacketsItem::reHeader* header)
    {
        // Now pull out the component values
        if (header != nullptr) {
            header->version =                       (buffer[0] >> 4) & 0xf;
            header->dataId  = ntohs(*((uint16_t *)  (buffer + 2)));
            header->offset  = ntohl(*((uint32_t *)  (buffer + 4)));
            header->length  = ntohl(*((uint32_t *)  (buffer + 8)));
            header->tick    = ntohll(*((uint64_t *) (buffer + 12)));
        }
    }


    /**
     * Method to set PacketsItem parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each PacketsItem, must be passed in as global parameters.
     *
     * @param pktCount number of UDP packets that can be stored in this item.
     */
    void PacketsItem::setEventFactorySettings(size_t pktCount) {
        PacketsItem::factoryPacketCount = pktCount;
    }


    /** Function to create BufferSupplyItems by RingBuffer. */
    const std::function< std::shared_ptr<PacketsItem> () >& PacketsItem::eventFactory() {
        static std::function< std::shared_ptr<PacketsItem> () > result([]  {
            return std::move(std::make_shared<PacketsItem>());
        });
        return result;
    }


    /**
     * Default constructor which uses values set by {@link #setEventFactorySetting()}.
     */
    PacketsItem::PacketsItem() : SupplyItem() {
        maxPktCount = factoryPacketCount;
        pktsFilled  = 0;
        myId        = idValue++;

        // Allocate array of reHeader structs each containing a single parsed RE header.
        // One of these for each UDP packet read in.
        headers = new reHeader[maxPktCount];

        // Allocate array of mmsghdr structs each containing a single UDP packet
        // (spread over 2 buffers, 1 for hdr, 1 for data).
        packets = new mmsghdr[maxPktCount];

        for (int i = 0; i < maxPktCount; i++) {
            packets[i].msg_hdr.msg_iov = new struct iovec[2];
            packets[i].msg_hdr.msg_iovlen = 2;

            // Where RE header goes
            packets[i].msg_hdr.msg_iov[0].iov_base = new uint8_t[RE_HEADER_BYTES];
            packets[i].msg_hdr.msg_iov[0].iov_len = RE_HEADER_BYTES;

            // Where data goes (can hold jumbo frame)
            packets[i].msg_hdr.msg_iov[1].iov_base = new uint8_t[9000];
            packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
        }
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    PacketsItem::PacketsItem(const PacketsItem & item) : SupplyItem(item) {

        // Avoid self copy ...
        if (this != &item) {
            maxPktCount = item.maxPktCount;
            pktsFilled = item.pktsFilled;

            // Allocate mem for parsed hdrs
            headers = new reHeader[maxPktCount];
            // Copy over valid headers
            memcpy(headers, item.headers, pktsFilled * sizeof(reHeader));

            // Allocate mem for packets
            packets = new mmsghdr[maxPktCount];

            for (int i = 0; i < maxPktCount; i++) {
                packets[i].msg_hdr.msg_iov = new struct iovec[2];
                packets[i].msg_hdr.msg_iovlen = 2;

                // Where RE header goes
                packets[i].msg_hdr.msg_iov[0].iov_base = new uint8_t[RE_HEADER_BYTES];
                packets[i].msg_hdr.msg_iov[0].iov_len = RE_HEADER_BYTES;

                // Where data goes
                packets[i].msg_hdr.msg_iov[1].iov_base = new uint8_t[9000];
                packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
            }

            // Copy over packet data
            for (int i = 0; i < pktsFilled; i++) {
                memcpy(packets[i].msg_hdr.msg_iov[0].iov_base,
                       item.packets[i].msg_hdr.msg_iov[0].iov_base, RE_HEADER_BYTES);

                memcpy(packets[i].msg_hdr.msg_iov[1].iov_base,
                       item.packets[i].msg_hdr.msg_iov[1].iov_base, 9000);
            }
        }
    }


    /** Destructor. */
    PacketsItem::~PacketsItem() {
        delete headers;

        for (int i = 0; i < maxPktCount; i++) {
            delete reinterpret_cast<uint8_t *>(packets[i].msg_hdr.msg_iov[0].iov_base);
            delete reinterpret_cast<uint8_t *>(packets[i].msg_hdr.msg_iov[1].iov_base);
            delete packets[i].msg_hdr.msg_iov;
        }

        delete packets;
    }


    /** Method to reset this item each time it is retrieved from the supply. */
    void PacketsItem::reset() {
        SupplyItem::reset();
        pktsFilled = 0;
    }


    /**
     * Get pointer to struct with packet data in it.
     * @param index index into array of data.
     * @return pointer to struct with packet data in it, or nullptr if none.
     */
    struct mmsghdr * PacketsItem::getPacket(uint32_t index) {
        if (index >= pktsFilled) {
            return nullptr;
        }
        return &(packets[index]);
    }


    /**
     * Get pointer to struct with reassembly info in it.
     * @param index index into array of info.
     * @return pointer to struct with reassembly info in it, or nullptr if none.
     */
    PacketsItem::reHeader * PacketsItem::getHeader(uint32_t index) {
        if (index >= pktsFilled) {
            return nullptr;
        }
        return &(headers[index]);
    }


    /**
     * Get the max number of packets that can be stored in this item.
     * @return max number of packets that can be stored in this item.
     */
    size_t PacketsItem::getMaxPacketCount() {return maxPktCount;}


    /**
     * Get the current number of valid packets that are stored in this item.
     * @return current number of valid packets that are stored in this item.
     */
    size_t PacketsItem::getPacketsFilled() {return pktsFilled;}
}