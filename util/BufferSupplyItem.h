//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_BUFFERSUPPLYITEM_H
#define UTIL_BUFFERSUPPLYITEM_H


#include <memory>
#include <atomic>
#include <functional>
#include <cstring>


#include "Disruptor/Disruptor.h"
#include "ByteOrder.h"
#include "ByteBuffer.h"


namespace ejfat {

    /**
     * This class provides the items which are supplied by the RecordSupply class.
     *
     * @date 11/05/2019
     * @author timmer
     */
    class BufferSupplyItem {

    public:

        /** Assign each record a unique id for debugging purposes. */
        static uint64_t  idValue;
        static bool      factoryOrderedRelease;
        static uint32_t  factoryBufferSize;
        static ByteOrder factoryByteOrder;

    private:


        /** Size of ByteBuffer in bytes. */
        uint32_t bufferSize;

        /** ByteBuffer object. */
        std::shared_ptr<ByteBuffer> buffer;

        /** Byte order of buffer. */
        ByteOrder order {ByteOrder::ENDIAN_LOCAL};

        /** Is this byte buffer direct? */
        bool direct;

        /** True if user releases ByteBufferItems in same order as acquired. */
        bool orderedRelease = false;

        /** Sequence in which this object was taken from ring for use by a producer with get(). */
        int64_t producerSequence = 0UL;

        /** Sequence in which this object was taken from ring for use by a consumer with consumerGet(). */
        int64_t consumerSequence = 0UL;

        /** Track more than one user so this object can be released for reuse. */
        std::atomic<int> atomicCounter {0};

        /** Track more than one user so this object can be released for reuse. */
        volatile int volatileCounter;

        /** If true, we're tracking more than one user. */
        bool multipleUsers = false;

        /**
         * If true, and this item comes from a supply used in the sense of
         * single-producer-single-consumer, then this flag can relay to the
         * consumer the need to force any write.
         */
        bool force = false;

        /**
         * Need to track whether this item was obtained through consumerGet() or
         * through either get() / getAsIs() since they must be released differently.
         */
        bool fromConsumerGet = false;

        /** Extra integer array for user's convenience. Array has 60 members.
         *  Each int gets reset to 0 each time supply.get() is called. */
        int32_t userInt[60];

        /** Extra long for user's convenience.
         *  Gets reset to 0 each time supply.get() is called. */
        int64_t userLong = 0L;

        /** Extra boolean for user's convenience.
         *  Gets reset to false each time supply.get() is called. */
        bool userBoolean = false;


        // For testing purposes

        /** Unique id for each object of this class. */
        uint32_t myId;


    public:


        static void setEventFactorySettings(const ByteOrder & order, uint32_t bufSize, bool release);
        static const std::function< std::shared_ptr<BufferSupplyItem> () >& eventFactory();
        static const std::function< std::shared_ptr<BufferSupplyItem> (int, const ByteOrder&, bool) >&
                        eventFactory(int, const ByteOrder&, bool);

        BufferSupplyItem();
//        BufferSupplyItem(int bufferSize, const ByteOrder & order = ByteOrder::ENDIAN_LOCAL, bool release = false, int myId = 0);
        BufferSupplyItem(const BufferSupplyItem & item);
        ~BufferSupplyItem() = default;

        BufferSupplyItem & operator=(const BufferSupplyItem & other) = delete;

        void reset();
        ByteOrder getOrder() const;
        uint32_t  getMyId() const;

        bool getForce() const;
        void setForce(bool force);

        bool isFromConsumerGet() const;
        void setFromConsumerGet(bool fromConsumerGet);

        int32_t* getUserInts();
        int      getUserIntCount();
        int64_t  getUserLong() const;
        void     setUserLong(int64_t i);
        bool     getUserBoolean() const;
        void     setUserBoolean(bool usrBool);

        uint32_t  getBufferSize() const;
        void setBuffer(std::shared_ptr<ByteBuffer> buf);
        std::shared_ptr<ByteBuffer> getClearedBuffer();
        std::shared_ptr<ByteBuffer> getBuffer() const;
        std::shared_ptr<ByteBuffer> ensureCapacity(uint32_t capacity);
        std::shared_ptr<ByteBuffer> expandBuffer(uint32_t capacity);

        void setUsers(int users);
        int  getUsers() const;
        void addUsers(int additionalUsers);

        // User should not call these, only called by BufferSupply ...........

        bool decrementCounter();

        int64_t getProducerSequence() const;
        void setProducerSequence(int64_t sequence);

        int64_t getConsumerSequence() const;
        void setConsumerSequence(int64_t sequence);

    };

}


#endif // UTIL_BUFFERSUPPLYITEM_H
