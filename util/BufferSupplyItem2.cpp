//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "BufferSupplyItem2.h"


namespace ejfat {


    //--------------------------------
    // STATIC INITIALIZATION
    //--------------------------------

    uint32_t  BufferSupplyItem2::factoryBufferSize {0};
    ByteOrder BufferSupplyItem2::factoryByteOrder {ByteOrder::ENDIAN_LOCAL};

    uint64_t  SupplyItem::idValue {0};
    bool      SupplyItem::factoryOrderedRelease;


    /**
     * Method to set BufferSupplyItem2 parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each BufferSupplyItem2, must be passed in as global parameters.
     *
     * @param order   byte order.
     * @param bufSize max number of uncompressed data bytes each record can hold.
     * @param release does the caller promise to release things in exact order as received?
     */
    void BufferSupplyItem2::setEventFactorySettings(const ByteOrder & order, uint32_t bufSize, bool release) {
        BufferSupplyItem2::factoryByteOrder = order;
        BufferSupplyItem2::factoryBufferSize = bufSize;
        SupplyItem::factoryOrderedRelease = release;
    }


    /** Function to create BufferSupplyItems by RingBuffer. */
    const std::function< std::shared_ptr<BufferSupplyItem2> () >& BufferSupplyItem2::eventFactory() {
        static std::function< std::shared_ptr<BufferSupplyItem2> () > result([]  {
            return std::move(std::make_shared<BufferSupplyItem2>());
        });
        return result;
    }


    /**
     * Default constructor which uses values set by {@link #setEventFactorySetting()}.
     */
    BufferSupplyItem2::BufferSupplyItem2() {
        order          = BufferSupplyItem2::factoryByteOrder;
        bufferSize     = BufferSupplyItem2::factoryBufferSize;
        orderedRelease = SupplyItem::factoryOrderedRelease;
        buffer = std::make_shared<ByteBuffer>(bufferSize);
        myId = idValue++;
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    BufferSupplyItem2::BufferSupplyItem2(const BufferSupplyItem2 & item) : SupplyItem(item) {

        // Avoid self copy ...
        if (this != &item) {
            buffer->copy(item.getBuffer());
            bufferSize       = item.bufferSize;
            order            = item.order;
            force            = item.force;
            for (int i=0; i < getUserIntCount(); i++) {
                userInt[i] = item.userInt[i];
            }
            userLong         = item.userLong;
            userBoolean      = item.userBoolean;
        }
    }


    /**
     * Method to reset this item each time it is retrieved from the supply.
     */
    void BufferSupplyItem2::reset() {
        SupplyItem::reset();

        buffer->clear();
        std::memset(userInt, 0, getUserIntCount()*sizeof(int32_t));
        userLong = 0L;
        force = false;
        userBoolean = false;
    }


    /**
     * Get the byte order used to build record.
     * @return byte order used to build record.
     */
    ByteOrder BufferSupplyItem2::getOrder() const {return order;}


    /**
     * Get the flag used to suggest a forced write to a consumer.
     * @return flag used to suggest a forced write to a consumer.
     */
    bool BufferSupplyItem2::getForce() const {return force;}


    /**
     * Set the flag used to suggest a forced write to a consumer.
     * @param frc flag used to suggest a forced write to a consumer.
     */
    void BufferSupplyItem2::setForce(bool frc) {this->force = frc;}


    /**
     * Get the user long.
     * User long gets reset to 0 each time supply.get() is called.
     * @return user long.
     */
    int64_t BufferSupplyItem2::getUserLong() const {return userLong;}


    /**
     * Set the user long.
     * @param i user long.
     */
    void BufferSupplyItem2::setUserLong(int64_t i) {userLong = i;}


    /**
    * Get the user integer array.
    * Each int in array gets reset to 0 each time supply.get() is called.
    * @return user integer array.
    */
    int32_t* BufferSupplyItem2::getUserInts() {return userInt;}


    /**
     * Get the number of elements in user int array.
     * @return number of elements in user int array.
     */
    int BufferSupplyItem2::getUserIntCount() {return 60;}


    /**
     * Get the user boolean.
     * User boolean gets reset to false each time supply.get() is called.
     * @return user boolean.
     */
    bool BufferSupplyItem2::getUserBoolean() const {return userBoolean;}


    /**
     * Set user boolean.
     * @param usrBool user boolean.
     */
    void BufferSupplyItem2::setUserBoolean(bool usrBool) {userBoolean = usrBool;}


    /**
     * Get the size in bytes of the contained ByteBuffer.
     * @return size in bytes of the contained ByteBuffer.
     */
    uint32_t BufferSupplyItem2::getBufferSize() const {return bufferSize;}


    /**
     * Set the contained ByteBuffer.
     * This method is dangerous -- definitely not thread safe!
     * @param buf contained ByteBuffer.
     */
    void BufferSupplyItem2::setBuffer(std::shared_ptr<ByteBuffer> buf) {
        bufferSize = buf->capacity();
        buffer = buf;
    }


    /**
     * Get the contained ByteBuffer. To be used by a data producer.
     * The contents are "cleared" such that position is set to 0, limit to capacity.
     * @return contained ByteBuffer.
     */
    std::shared_ptr<ByteBuffer> BufferSupplyItem2::getClearedBuffer() {
        buffer->clear();
        return buffer;
    }


    /**
     * Get the contained ByteBuffer without any modifications.
     * To be used by a data consumer.
     * @return contained ByteBuffer without any modifications.
     */
    std::shared_ptr<ByteBuffer> BufferSupplyItem2::getBuffer() const {
        return buffer;
    }


    /**
     * Make sure the buffer is the size needed.
     * This method is dangerous -- definitely not thread safe!
     * Use this method immediately upon getting this item from the supply
     * and before the buffer is used.
     * If expanded, all data is <b>LOST</b>, so call this before writing data.
     * @param capacity minimum necessary size of buffer in bytes.
     * @return internal buffer, new object if capacity expanded, else current buffer as is.
     */
    std::shared_ptr<ByteBuffer> BufferSupplyItem2::ensureCapacity(uint32_t capacity) {
        if (bufferSize < capacity) {
            buffer = std::make_shared<ByteBuffer>(capacity);
            buffer->order(order);
            bufferSize = capacity;
        }
        return buffer;
    }


    /**
     * Make sure the buffer is the size needed.
     * If expanded, all data up to the limit is copied.
     * Position, limit, and mark are unchanged.
     * @param capacity new, larger, desired capacity buffer in bytes.
     * @return current buffer with new capacity.
     */
    std::shared_ptr<ByteBuffer> BufferSupplyItem2::expandBuffer(uint32_t capacity) {
        if (bufferSize < capacity) {
            buffer->expand(capacity);
            bufferSize = capacity;
        }
        return buffer;
    }



}
