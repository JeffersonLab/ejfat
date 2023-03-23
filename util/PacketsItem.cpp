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

    size_t PacketsItem::factoryPacketCount {200};


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
     * Print a couple things from the given packet in a PacketItem.
     * @param index index of specific packet in PacketItem.
     */
    void PacketsItem::printPacketItem(std::shared_ptr<PacketsItem> item, int index) {
        if (item == nullptr || item->getPacket(index) == nullptr) {
            fprintf(stderr, "no PacketItem data\n" );
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
    PacketsItem::PacketsItem() : SupplyItem() {
        maxPktCount = factoryPacketCount;
//        maxPktCount = 200;
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
//            packets[i].msg_hdr.msg_name = nullptr;
//            packets[i].msg_hdr.msg_namelen = 0;

            packets[i].msg_hdr.msg_iov = new struct iovec[2];
            packets[i].msg_hdr.msg_iovlen = 2;
            memset(packets[i].msg_hdr.msg_iov, 0, sizeof(struct iovec[2]));

            // Where RE header goes
            packets[i].msg_hdr.msg_iov[0].iov_base = new uint8_t[HEADER_BYTES];
            packets[i].msg_hdr.msg_iov[0].iov_len = HEADER_BYTES;

            // Where data goes (can hold jumbo frame)
            packets[i].msg_hdr.msg_iov[1].iov_base = new uint8_t[9000];
            packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
        }

//        // Allocate array of reHeader structs each containing a single parsed RE header.
//        // One of these for each UDP packet read in.
//        memset(hdrs, 0, sizeof(hdrs));
//        memset(iovecs, 0, sizeof(iovecs));
//
//        // Allocate array of mmsghdr structs each containing a single UDP packet
//        // (spread over 2 buffers, 1 for hdr, 1 for data).
//        packets = msgs;
//        memset(msgs, 0, sizeof(msgs));
//
//        for (int i = 0; i < 200; i++) {
//            msgs[i].msg_hdr.msg_name = nullptr;
//            msgs[i].msg_hdr.msg_namelen = 0;
//
//            packets[i].msg_hdr.msg_iov = &iovecs[i*2];
//            packets[i].msg_hdr.msg_iovlen = 2;
//
//            // Where RE header goes
//            packets[i].msg_hdr.msg_iov[0].iov_base = rcvHdr[i];
//            packets[i].msg_hdr.msg_iov[0].iov_len = 20;
//
//            // Where data goes (can hold jumbo frame)
//            packets[i].msg_hdr.msg_iov[1].iov_base = rcvBuf[i];
//            packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
//        }

    }

//    const int PACKETS_TO_READ = 1024;
//    static char receiveBuffer[PACKETS_TO_READ][USHRT_MAX];
//    static struct iovec iovecs[PACKETS_TO_READ];
//    static struct mmsghdr msgs[PACKETS_TO_READ];

//    for (int i = 0; i < PACKETS_TO_READ; i++) {
//    iovecs[i].iov_base         = receiveBuffer[i];
//    iovecs[i].iov_len          = USHRT_MAX;
//    msgs[i].msg_hdr.msg_iov    = &iovecs[i];
//    msgs[i].msg_hdr.msg_iovlen = 1;
//    msgs[i].msg_hdr.msg_name = &sockFrom[i];
//    msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in );
//    }


//            #define VLEN 10
//            #define BUFSIZE 200
//            #define TIMEOUT 1
//                int i;
//                struct mmsghdr msgs[VLEN];
//                struct iovec iovecs[VLEN];
//                char bufs[VLEN][BUFSIZE+1];
//
//
//            memset(msgs, 0, sizeof(msgs));
//            for (i = 0; i < VLEN; i++) {
//            iovecs[i].iov_base         = bufs[i];
//            iovecs[i].iov_len          = BUFSIZE;
//            msgs[i].msg_hdr.msg_iov    = &iovecs[i];
//            msgs[i].msg_hdr.msg_iovlen = 1;
//            }
//

//
//            retval = recvmmsg(sockfd, msgs, VLEN, 0, &timeout);


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
            memcpy(headers, item.headers, maxPktCount * sizeof(reHeader));

            // Allocate mem for packets
            packets = new mmsghdr[maxPktCount];
            memset(packets, 0, sizeof(*packets));

            for (int i = 0; i < maxPktCount; i++) {
                packets[i].msg_hdr.msg_name = nullptr;
                packets[i].msg_hdr.msg_namelen = 0;

                packets[i].msg_hdr.msg_iov = new struct iovec[2];
                packets[i].msg_hdr.msg_iovlen = 2;
                memset(packets[i].msg_hdr.msg_iov, 0, sizeof(struct iovec[2]));

                // Where RE header goes
                packets[i].msg_hdr.msg_iov[0].iov_base = new uint8_t[HEADER_BYTES];
                packets[i].msg_hdr.msg_iov[0].iov_len = HEADER_BYTES;

                // Where data goes
                packets[i].msg_hdr.msg_iov[1].iov_base = new uint8_t[9000];
                packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
            }

//            // Allocate array of reHeader structs each containing a single parsed RE header.
//            // One of these for each UDP packet read in.
//            memset(hdrs, 0, sizeof(hdrs));
//            memset(iovecs, 0, sizeof(iovecs));
//
//            // Allocate array of mmsghdr structs each containing a single UDP packet
//            // (spread over 2 buffers, 1 for hdr, 1 for data).
//            packets = msgs;
//            memset(msgs, 0, sizeof(msgs));
//
//            for (int i = 0; i < 200; i++) {
//                msgs[i].msg_hdr.msg_name = nullptr;
//                msgs[i].msg_hdr.msg_namelen = 0;
//
//                packets[i].msg_hdr.msg_iov = &iovecs[i*2];
//                packets[i].msg_hdr.msg_iovlen = 2;
//
//                // Where RE header goes
//                packets[i].msg_hdr.msg_iov[0].iov_base = rcvHdr[i];
//                packets[i].msg_hdr.msg_iov[0].iov_len = 20;
//
//                // Where data goes (can hold jumbo frame)
//                packets[i].msg_hdr.msg_iov[1].iov_base = rcvBuf[i];
//                packets[i].msg_hdr.msg_iov[1].iov_len = 9000;
//            }


            // Copy over packet data
            for (int i = 0; i < pktsFilled; i++) {
                memcpy(packets[i].msg_hdr.msg_iov[0].iov_base,
                       item.packets[i].msg_hdr.msg_iov[0].iov_base, HEADER_BYTES);

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
     * Get pointer to array of structs with all packet data.
     * @return pointer to array of structs with all packet data.
     */
    struct mmsghdr * PacketsItem::getPackets() {return packets;}


    /**
     * Get pointer to array of header data.
     * @return pointer to array of header data.
     */
    reHeader * PacketsItem::getHeaders() {return headers;}


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
    reHeader * PacketsItem::getHeader(uint32_t index) {
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
    int PacketsItem::getSource(uint32_t index) {
        if (index >= pktsFilled) {
            return -1;
        }
        return headers[index].dataId;
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


    /**
     * set the current number of valid packets that are stored in this item.
     * @param count current number of valid packets that are stored in this item.
     */
    void PacketsItem::setPacketsFilled(size_t count) {pktsFilled = count;}

}
