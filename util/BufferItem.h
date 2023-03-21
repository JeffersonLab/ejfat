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


#include "ByteOrder.h"
#include "ByteBuffer.h"
#include "SupplyItem.h"
#include "ejfat_assemble_ersap.hpp"


namespace ejfat {

    /**
     * This class defines the ByteBuffer items which are supplied by the Supplier class.
     *
     * @date 03/13/2023
     * @author timmer
     */
    class BufferItem : public SupplyItem {

    public:

        static uint32_t  factoryBufferSize;
        static ByteOrder factoryByteOrder;

    private:


        /** Size of ByteBuffer in bytes. */
        uint32_t bufferSize;

        /** ByteBuffer object. */
        std::shared_ptr<ByteBuffer> buffer;

        /** Byte order of buffer. */
        ByteOrder order {ByteOrder::ENDIAN_LOCAL};

        /** Reassembly header associated with this data, if any. */
        reHeader header;

        /**
         * If true, and this item comes from a supply used in the sense of
         * single-producer-single-consumer, then this flag can relay to the
         * consumer the need to force any write.
         */
        bool force = false;

        /** If true, the data contained in buffer is valid, else it isn't. */
        bool isValidData = true;

        /** Extra integer for user's convenience.
         *  Gets reset to 0 each time supply.get() is called. */
        int32_t userInt = 0;

        /** Extra long for user's convenience.
         *  Gets reset to 0 each time supply.get() is called. */
        int64_t userLong = 0L;

        /** Extra boolean for user's convenience.
         *  Gets reset to false each time supply.get() is called. */
        bool userBoolean = false;


    public:

        static void setEventFactorySettings(const ByteOrder & order, uint32_t bufSize);
        static const std::function< std::shared_ptr<BufferItem> () >& eventFactory();


        BufferItem();
        BufferItem(const BufferItem & item);
//        ~BufferItem();

        BufferItem & operator=(const BufferItem & other) = delete;

        void reset();

        ByteOrder getOrder() const;

        reHeader & getHeader();
        void setHeader(reHeader *hdr);

        bool getForce() const;
        void setForce(bool force);

        int  getSourceId();
        void setSourceId(int id);

        bool validData();
        void setValidData(bool valid);

        int32_t  getUserInt();
        void     setUserInt(int32_t i);

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
