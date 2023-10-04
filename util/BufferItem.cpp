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

    uint32_t  BufferItem::consumerCount {1};
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
     * @param consumers number consumers whose related info will be stored in this item (8 max, 1 min).
     */
    void BufferItem::setEventFactorySettings(const ByteOrder & order, uint32_t bufSize, uint32_t consumers) {
        BufferItem::factoryByteOrder = order;
        BufferItem::factoryBufferSize = bufSize;
        consumers = consumers > 8 ? 8 : consumers;
        consumers = consumers < 1 ? 1 : consumers;
        BufferItem::consumerCount = consumers;
    }


    /** Function to create BufferSupplyItems by RingBuffer. */
    const std::function< std::shared_ptr<BufferItem> () >& BufferItem::eventFactory() {
        static std::function< std::shared_ptr<BufferItem> () > result([]  {
            return std::make_shared<BufferItem>();
        });
        return result;
    }


    /**
     * Default constructor which uses values set by {@link #setEventFactorySetting()}.
     */
    BufferItem::BufferItem() : SupplyItem(BufferItem::consumerCount) {
        order          = BufferItem::factoryByteOrder;
        bufferSize     = BufferItem::factoryBufferSize;
        orderedRelease = SupplyItem::factoryOrderedRelease;

        buffer = std::make_shared<ByteBuffer>(bufferSize);
        myId = idValue++;
    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    BufferItem::BufferItem(const BufferItem & item) : SupplyItem(item) {
        copy(item);
    }

    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    BufferItem::BufferItem(const std::shared_ptr<BufferItem> & item) : SupplyItem(*(item.get())) {
        copy(item);
    }


    /**
     * Copy given ring item.
     * @param item ring item to copy.
     */
    void BufferItem::copy(const BufferItem & item) {
        // Avoid self copy ...
        if (this != &item) {
            buffer->copy(item.getBuffer());
            bufferSize       = item.bufferSize;
            order            = item.order;
            force            = item.force;
            isValidData      = item.isValidData;
            userInt          = item.userInt;
            userLong         = item.userLong;
            userBoolean      = item.userBoolean;

            eventNum         = item.eventNum;
            dataLen          = item.dataLen;
            offsets          = item.offsets;
        }
    }


    /**
     * Copy given ring item.
     * @param item ring item to copy.
     */
    void BufferItem::copy(const std::shared_ptr<BufferItem> & item) {
        // Avoid self copy ...
        if (this != item.get()) {
            buffer->copy(item->getBuffer());
            bufferSize       = item->bufferSize;
            order            = item->order;
            force            = item->force;
            isValidData      = item->isValidData;
            userInt          = item->userInt;
            userLong         = item->userLong;
            userBoolean      = item->userBoolean;

            eventNum         = item->eventNum;
            dataLen          = item->dataLen;
            offsets          = item->offsets;
        }
    }


    /**
     * Method to reset this item each time it is retrieved from the supply.
     */
    void BufferItem::reset() {
        SupplyItem::reset();

        buffer->clear();
        isValidData = true;
        userInt = 0;
        userLong = 0L;
        force = false;
        userBoolean = false;

        eventNum = 0;
        dataLen = 0;
        offsets.clear();
    }


    /**
     * Get the byte order used to build record.
     * @return byte order used to build record.
     */
    ByteOrder BufferItem::getOrder() const {return order;}

    /**
      * Get a reference to the set storing the offsets in reassembly headers of UDP packets
      * used to contruct the data in this buffer, if any.
      * @return reference to the set storing the offsets in reassembly headers of UDP packets
      * used to contruct the data in this buffer.
      */
    std::unordered_set<uint32_t> & BufferItem::getOffsets() {return offsets;}


    /**
     * Get the event or tick number.
     * @return the event or tick number.
     */
    uint64_t BufferItem::getEventNum() {return eventNum;}

    /**
     * Set the event or tick number.
     * @param num event or tick number.
     */
    void BufferItem::setEventNum(uint64_t num) {eventNum = num;}


     /**
      * Get the length of valid data in bytes.
      * @return length of valid data in bytes.
      */
    uint32_t BufferItem::getDataLen() {return dataLen;}

     /**
      * Set the length of valid data in bytes.
      * @param len length of valid data in bytes.
      */
    void BufferItem::setDataLen(uint32_t len) {dataLen = len;}


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
     * Get whether buffer contains valid data or not.
     * @return true if buffer contains valid data, else false.
     */
    bool BufferItem::validData() {return isValidData;}


    /**
     * Set whether buffer contains valid data or not.
     * @param valid true if buffer contains valid data, else false.
     */
    void BufferItem::setValidData(bool valid) {isValidData = valid;}


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
    * Get the user integer.
    * Gets reset to 0 each time supply.get() is called.
    * @return user integer.
    */
    int32_t BufferItem::getUserInt() {return userInt;}


    /**
     * Set the user int.
     * @param i user int.
     */
    void BufferItem::setUserInt(int32_t i) {userInt = i;}


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
     * If expanded, all data up to the original capacity is copied.
     * Position, limit, and mark are unchanged.
     * @param capacity new, larger, desired capacity buffer in bytes.
     * @return current buffer with new capacity.
     */
    std::shared_ptr<ByteBuffer> BufferItem::expandBuffer(uint32_t capacity) {
        if (bufferSize < capacity) {
            buffer->expandAndCopyAll(capacity);
            bufferSize = capacity;
        }
        return buffer;
    }



}
