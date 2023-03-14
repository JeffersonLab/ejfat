//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_BUFFERITEM_H
#define UTIL_BUFFERITEM_H


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
     * This class defines the ByteBufferItems which are supplied by the Supplier class.
     *
     * @date 03/13/2023
     * @author timmer
     */
    class BufferItem : public SupplyItem {

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


        static void setEventFactorySettings(const ByteOrder & order, uint32_t bufSize);
        static const std::function< std::shared_ptr<BufferItem> () >& eventFactory();
        static const std::function< std::shared_ptr<BufferItem> (int, const ByteOrder&, bool) >&
                        eventFactory(int, const ByteOrder&, bool);

        BufferItem();
//        BufferItem(int bufferSize, const ByteOrder & order = ByteOrder::ENDIAN_LOCAL, bool release = false, int myId = 0);
        BufferItem(const BufferItem & item);
        ~BufferItem() = default;

        BufferItem & operator=(const BufferItem & other) = delete;

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


#endif // UTIL_BUFFERITEM_H
