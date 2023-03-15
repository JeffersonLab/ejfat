//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "BufferItem.h"


namespace ejfat {


    //--------------------------------
    // STATIC INITIALIZATION
    //--------------------------------

    uint32_t  BufferItem::factoryBufferSize {0};
    ByteOrder BufferItem::factoryByteOrder {ByteOrder::ENDIAN_LOCAL};


    /**
     * Method to set BufferItem parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each BufferItem, must be passed in as global parameters.
     *
     * @param order   byte order.
     * @param bufSize max number of uncompressed data bytes each record can hold.
     */
    void BufferItem::setEventFactorySettings(const ByteOrder & order, uint32_t bufSize) {
        BufferItem::factoryByteOrder = order;
        BufferItem::factoryBufferSize = bufSize;
    }


    /** Function to create BufferSupplyItems by RingBuffer. */
    const std::function< std::shared_ptr<BufferItem> () >& BufferItem::eventFactory() {
        static std::function< std::shared_ptr<BufferItem> () > result([]  {
            return std::move(std::make_shared<BufferItem>());
        });
        return result;
    }


    /**
     * Default constructor which uses values set by {@link #setEventFactorySetting()}.
     */
    BufferItem::BufferItem() : SupplyItem() {
        order          = BufferItem::factoryByteOrder;
        bufferSize     = BufferItem::factoryBufferSize;
        orderedRelease = SupplyItem::factoryOrderedRelease;

        buffer = std::make_shared<ByteBuffer>(bufferSize);
        userInt = new int32_t[60];
        myId = idValue++;
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    BufferItem::BufferItem(const BufferItem & item) : SupplyItem(item) {

        // Avoid self copy ...
        if (this != &item) {
            buffer->copy(item.getBuffer());
            bufferSize       = item.bufferSize;
            order            = item.order;
            force            = item.force;
            userInt          = new int32_t[60];
            for (int i=0; i < getUserIntCount(); i++) {
                userInt[i] = item.userInt[i];
            }
            userLong         = item.userLong;
            userBoolean      = item.userBoolean;
        }
    }


    BufferItem::~BufferItem() {
        delete(userInt);
    };


    /**
     * Method to reset this item each time it is retrieved from the supply.
     */
    void BufferItem::reset() {
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
    ByteOrder BufferItem::getOrder() const {return order;}


    /**
     * Get the flag used to suggest a forced write to a consumer.
     * @return flag used to suggest a forced write to a consumer.
     */
    bool BufferItem::getForce() const {return force;}


    /**
     * Set the flag used to suggest a forced write to a consumer.
     * @param frc flag used to suggest a forced write to a consumer.
     */
    void BufferItem::setForce(bool frc) {this->force = frc;}


    /**
     * Get the user long.
     * User long gets reset to 0 each time supply.get() is called.
     * @return user long.
     */
    int64_t BufferItem::getUserLong() const {return userLong;}


    /**
     * Set the user long.
     * @param i user long.
     */
    void BufferItem::setUserLong(int64_t i) {userLong = i;}


    /**
    * Get the user integer array.
    * Each int in array gets reset to 0 each time supply.get() is called.
    * @return user integer array.
    */
    int32_t* BufferItem::getUserInts() {return userInt;}


    /**
     * Get the number of elements in user int array.
     * @return number of elements in user int array.
     */
    int BufferItem::getUserIntCount() {return 60;}


    /**
     * Get the user boolean.
     * User boolean gets reset to false each time supply.get() is called.
     * @return user boolean.
     */
    bool BufferItem::getUserBoolean() const {return userBoolean;}


    /**
     * Set user boolean.
     * @param usrBool user boolean.
     */
    void BufferItem::setUserBoolean(bool usrBool) {userBoolean = usrBool;}


    /**
     * Get the size in bytes of the contained ByteBuffer.
     * @return size in bytes of the contained ByteBuffer.
     */
    uint32_t BufferItem::getBufferSize() const {return bufferSize;}


    /**
     * Set the contained ByteBuffer.
     * This method is dangerous -- definitely not thread safe!
     * @param buf contained ByteBuffer.
     */
    void BufferItem::setBuffer(std::shared_ptr<ByteBuffer> buf) {
        bufferSize = buf->capacity();
        buffer = buf;
    }


    /**
     * Get the contained ByteBuffer. To be used by a data producer.
     * The contents are "cleared" such that position is set to 0, limit to capacity.
     * @return contained ByteBuffer.
     */
    std::shared_ptr<ByteBuffer> BufferItem::getClearedBuffer() {
        buffer->clear();
        return buffer;
    }


    /**
     * Get the contained ByteBuffer without any modifications.
     * To be used by a data consumer.
     * @return contained ByteBuffer without any modifications.
     */
    std::shared_ptr<ByteBuffer> BufferItem::getBuffer() const {
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
    std::shared_ptr<ByteBuffer> BufferItem::ensureCapacity(uint32_t capacity) {
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
    std::shared_ptr<ByteBuffer> BufferItem::expandBuffer(uint32_t capacity) {
        if (bufferSize < capacity) {
            buffer->expand(capacity);
            bufferSize = capacity;
        }
        return buffer;
    }



}
