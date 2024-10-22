//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "PacketsItemN.h"


namespace ejfat {

    //--------------------------------
    // STATIC INITIALIZATION
    //--------------------------------

    size_t PacketsItemN::factoryPacketCount {200};
    uint32_t  PacketsItemN::consumerCount {1};


    /**
     * Method to set PacketsItemN parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each PacketsItemN, must be passed in as global parameters.
     *
     * @param pktCount number of UDP packets that can be stored in this item.
     * @param consumers number consumers whose related info will be stored in this item (8 max, 1 min).
     */
    void PacketsItemN::setEventFactorySettings(size_t pktCount, uint32_t consumers) {
        PacketsItemN::factoryPacketCount = pktCount;
        consumers = consumers > 8 ? 8 : consumers;
        consumers = consumers < 1 ? 1 : consumers;
        PacketsItemN::consumerCount = consumers;
    }


    /** Function to create PacketsItemsN by RingBuffer. */
    const std::function< std::shared_ptr<PacketsItemN> () >& PacketsItemN::eventFactory() {
        static std::function< std::shared_ptr<PacketsItemN> () > result([]  {
            return std::make_shared<PacketsItemN>();
        });
        return result;
    }


    /**
     * Print a couple things from the given packet in a PacketItem.
     * @param index index of specific packet in PacketItem.
     */
    void PacketsItemN::printPacketItem(std::shared_ptr<PacketsItemN> item, int index) {
        if (item == nullptr) {
            fprintf(stderr, "printPacketItem: item arg is null\n");
            return;
        }

        if (item->getPacket(index) == nullptr) {
            fprintf(stderr, "printPacketItem: no item data\n");
            return;
        }

        struct mmsghdr *hdr = item->getPacket(index);
#ifdef __APPLE__
        fprintf(stderr, "%u bytes for item %d, %d bufs,\n", hdr->msg_len, index, hdr->msg_hdr.msg_iovlen );
#else
        fprintf(stderr, "%u bytes for item %d, %zu bufs,\n", hdr->msg_len, index, hdr->msg_hdr.msg_iovlen );
#endif
        for (int i=0; i < hdr->msg_hdr.msg_iovlen; i++) {
            fprintf(stderr, "   buf %d: %zu bytes\n", i, hdr->msg_hdr.msg_iov[i].iov_len);
        }
    }


    /**
     * Default constructor which uses values set by {@link #setEventFactorySetting()}.
     */
    PacketsItemN::PacketsItemN() : SupplyItem(PacketsItemN::consumerCount) {
        maxPktCount = factoryPacketCount;
        pktsFilled  = 0;
        myId        = idValue++;

        // Allocate array of reHeader structs each containing a single parsed RE header.
        // One of these for each UDP packet read in.
        headers = new reHeader[maxPktCount];

        // Allocate array of mmsghdr structs each containing a single UDP packet
        // (spread over 2 buffers, 1 for hdr, 1 for data).
        packets = new struct mmsghdr[maxPktCount];
        memset(packets, 0, sizeof(*packets));

        for (int i = 0; i < maxPktCount; i++) {
            packets[i].msg_hdr.msg_name = nullptr;
            packets[i].msg_hdr.msg_namelen = 0;

            packets[i].msg_hdr.msg_iov = new struct iovec[1];
            packets[i].msg_hdr.msg_iovlen = 1;

            // Where RE header + data both go
            packets[i].msg_hdr.msg_iov[0].iov_base = new uint8_t[9000];
            packets[i].msg_hdr.msg_iov[0].iov_len = 9000;
        }
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    PacketsItemN::PacketsItemN(const PacketsItemN & item) : SupplyItem(item) {

        // Avoid self copy ...
        if (this != &item) {
            maxPktCount = item.maxPktCount;
            pktsFilled = item.pktsFilled;

            // Allocate mem for parsed hdrs
            headers = new reHeader[maxPktCount];
            // Copy over valid headers
            memcpy(headers, item.headers, maxPktCount * sizeof(reHeader));

            // Allocate mem for packets
            packets = new mmsghdr[maxPktCount];
            memset(packets, 0, sizeof(*packets));

            for (int i = 0; i < maxPktCount; i++) {
                struct msghdr *msg_hdr = &packets[i].msg_hdr;

                msg_hdr->msg_name = nullptr;
                msg_hdr->msg_namelen = 0;

                msg_hdr->msg_iov = new struct iovec[1];
                msg_hdr->msg_iovlen = 1;

                // Where RE header + data both go
                msg_hdr->msg_iov[0].iov_base = new uint8_t[9100];
                msg_hdr->msg_iov[0].iov_len = 9100;
            }

            // Copy over packet data
            for (int i = 0; i < pktsFilled; i++) {
                packets[i].msg_hdr.msg_flags = item.packets[i].msg_hdr.msg_flags;

                memcpy(packets[i].msg_hdr.msg_iov[0].iov_base,
                       item.packets[i].msg_hdr.msg_iov[0].iov_base, HEADER_BYTES);

                memcpy(packets[i].msg_hdr.msg_iov[1].iov_base,
                       item.packets[i].msg_hdr.msg_iov[1].iov_base, 9100);
            }
        }
    }


    /** Destructor. */
    PacketsItemN::~PacketsItemN() {
        delete headers;

        for (int i = 0; i < maxPktCount; i++) {
            delete reinterpret_cast<uint8_t *>(packets[i].msg_hdr.msg_iov[0].iov_base);
            delete packets[i].msg_hdr.msg_iov;
        }

        delete packets;
    }


    /** Method to reset this item each time it is retrieved from the supply. */
    void PacketsItemN::reset() {
        SupplyItem::reset();
        pktsFilled = 0;
    }


    /**
     * Get pointer to array of structs with all packet data.
     * @return pointer to array of structs with all packet data.
     */
    struct mmsghdr * PacketsItemN::getPackets() {return packets;}


    /**
     * Get pointer to array of header data.
     * @return pointer to array of header data.
     */
    reHeader * PacketsItemN::getHeaders() {return headers;}


    /**
     * Get pointer to struct with packet data in it.
     * @param index index into array of data.
     * @return pointer to struct with packet data in it, or nullptr if none.
     */
    struct mmsghdr * PacketsItemN::getPacket(uint32_t index) {
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
    reHeader * PacketsItemN::getHeader(uint32_t index) {
        if (index >= pktsFilled) {
            return nullptr;
        }
        return &(headers[index]);
    }


    /**
     * Get the data id of RE packet header at index.
     * @param index index into array of info.
     * @return data id of RE packet header at index, or -1 if index out of bounds.
     */
    int PacketsItemN::getSource(uint32_t index) {
        if (index >= pktsFilled) {
            return -1;
        }
        return headers[index].dataId;
    }


    /**
     * <p>Get the flag of received message.</p>
     *
     * <ul>
     * <li>MSG_EOR indicates end-of-record; the data returned completed a record.
     * <li>MSG_TRUNC indicates that the trailing portion of a datagram was discarded because the datagram was
     * larger than the buffer supplied.
     * <li>MSG_CTRUNC indicates that some control data were discarded due to lack of space in the
     * buffer for ancillary data.
     * <li>MSG_OOB is returned to indicate that expedited or out-of-band data were received.
     * </ul>
     *
     * @param index index into array of info.
     * @return flag of received message, or -1 if index out of bounds.
     */
    int PacketsItemN::getRecvFlag(uint32_t index) {
        if (index >= pktsFilled) {
            return -1;
        }
        return packets[index].msg_hdr.msg_flags;
    }


    /**
     * Gets whether there was any receive error which truncated data
     * for any of the packets in this item.
     * @return true if data discarded from any of the packets received, else false.
     */
    bool PacketsItemN::dataDiscarded() {
        bool discarded = false;
        for (int i = 0; i < pktsFilled; i++) {
            discarded = discarded || (packets[i].msg_hdr.msg_flags == MSG_TRUNC);
        }
        return discarded;
    }


    /**
     * Get the max number of packets that can be stored in this item.
     * @return max number of packets that can be stored in this item.
     */
    size_t PacketsItemN::getMaxPacketCount() {return maxPktCount;}


    /**
     * Get the current number of valid packets that are stored in this item.
     * @return current number of valid packets that are stored in this item.
     */
    size_t PacketsItemN::getPacketsFilled() {return pktsFilled;}


    /**
     * set the current number of valid packets that are stored in this item.
     * @param count current number of valid packets that are stored in this item.
     */
    void PacketsItemN::setPacketsFilled(size_t count) {pktsFilled = count;}

}
