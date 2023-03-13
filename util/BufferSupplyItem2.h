//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_BUFFERSUPPLYITEM2_H
#define UTIL_BUFFERSUPPLYITEM2_H


#include <memory>
#include <atomic>
#include <functional>
#include <cstring>


#include "Disruptor/Disruptor.h"
#include "ByteOrder.h"
#include "ByteBuffer.h"
#include "SupplyItem.h"


namespace ejfat {

    /**
     * This class provides the items which are supplied by the RecordSupply class.
     *
     * @date 11/05/2019
     * @author timmer
     */
    class BufferSupplyItem2 : public SupplyItem {

    public:

        /** Assign each record a unique id for debugging purposes. */
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

        /**
         * If true, and this item comes from a supply used in the sense of
         * single-producer-single-consumer, then this flag can relay to the
         * consumer the need to force any write.
         */
        bool force = false;

        /** Extra integer array for user's convenience. Array has 60 members.
         *  Each int gets reset to 0 each time supply.get() is called. */
        int32_t userInt[60];

        /** Extra long for user's convenience.
         *  Gets reset to 0 each time supply.get() is called. */
        int64_t userLong = 0L;

        /** Extra boolean for user's convenience.
         *  Gets reset to false each time supply.get() is called. */
        bool userBoolean = false;


    public:


        static void setEventFactorySettings(const ByteOrder & order, uint32_t bufSize, bool release);
        static const std::function< std::shared_ptr<BufferSupplyItem2> () >& eventFactory();
        static const std::function< std::shared_ptr<BufferSupplyItem2> (int, const ByteOrder&, bool) >&
                        eventFactory(int, const ByteOrder&, bool);

        BufferSupplyItem2();
//        BufferSupplyItem2(int bufferSize, const ByteOrder & order = ByteOrder::ENDIAN_LOCAL, bool release = false, int myId = 0);
        BufferSupplyItem2(const BufferSupplyItem2 & item);
        ~BufferSupplyItem2() = default;

        BufferSupplyItem2 & operator=(const BufferSupplyItem2 & other) = delete;

        void reset();

        ByteOrder getOrder() const;
        bool getForce() const;
        void setForce(bool force);

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

    };

}


#endif // UTIL_BUFFERSUPPLYITEM2_H
