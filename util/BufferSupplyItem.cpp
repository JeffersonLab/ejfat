//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "BufferSupplyItem.h"


namespace ejfat {


    //--------------------------------
    // STATIC INITIALIZATION
    //--------------------------------

    // Set default values for BufferSupplyItems
    uint64_t  BufferSupplyItem::idValue {0ULL};
    bool      BufferSupplyItem::factoryOrderedRelease {false};
    uint32_t  BufferSupplyItem::factoryBufferSize {0};
    ByteOrder BufferSupplyItem::factoryByteOrder {ByteOrder::ENDIAN_LOCAL};


    /**
     * Method to set BufferSupplyItem parameters for objects created by eventFactory.
     * Doing things in this roundabout manor is necessary because the disruptor's
     * createSingleProducer method takes a function for created items which has no args! Thus these args,
     * needed for construction of each BufferSupplyItem, must be passed in as global parameters.
     *
     * @param order   byte order.
     * @param bufSize max number of uncompressed data bytes each record can hold.
     * @param release does the caller promise to release things in exact order as received?
     */
    void BufferSupplyItem::setEventFactorySettings(const ByteOrder & order, uint32_t bufSize, bool release) {
        BufferSupplyItem::factoryByteOrder = order;
        BufferSupplyItem::factoryOrderedRelease = release;
        BufferSupplyItem::factoryBufferSize = bufSize;
    }


    /** Function to create BufferSupplyItems by RingBuffer. */
    const std::function< std::shared_ptr<BufferSupplyItem> () >& BufferSupplyItem::eventFactory() {
        static std::function< std::shared_ptr<BufferSupplyItem> () > result([]  {
            return std::move(std::make_shared<BufferSupplyItem>());
        });
        return result;
    }


//    /** Function to create BufferSupplyItems by RingBuffer. */
//    const std::function< std::shared_ptr<BufferSupplyItem> (int, const ByteOrder &, bool) >
//    & BufferSupplyItem::eventFactory(int size, const ByteOrder &order, bool release) {
//        static std::function< std::shared_ptr<BufferSupplyItem> (int, const ByteOrder &, bool) >
//                result([size, order, release] (int, const ByteOrder &, bool) {
//            return std::move(std::make_shared<BufferSupplyItem>(size, order, release, idValue++));
//        });
//        return result;
//    }



    /**
     * Default constructor which uses values set by {@link #setEventFactorySetting()}.
     */
    BufferSupplyItem::BufferSupplyItem() {
        order = BufferSupplyItem::factoryByteOrder;
        orderedRelease = BufferSupplyItem::factoryOrderedRelease;
        buffer = std::make_shared<ByteBuffer>(BufferSupplyItem::bufferSize);
        myId = idValue++;
    }


//    /**
//     * Constructor.
//     *
//     * @param bufferSize size in bytes of ByteBuffer to construct.
//     * @param order byte order of ByteBuffer to construct.
//     * @param direct is the buffer direct (in memory not managed by JVM) or not.
//     * @param orderedRelease if true, release BufferSupplyItems in same order as acquired.
//     * @param myId unique id of this object.
//     */
//    BufferSupplyItem::BufferSupplyItem(int bufferSize, const ByteOrder & order,
//                                       bool orderedRelease, int myId) :
//            bufferSize(bufferSize),
//            order(order),
//            orderedRelease(orderedRelease),
//            myId(myId)
//    {
//        buffer = std::make_shared<ByteBuffer>(bufferSize);
//        buffer->order(order);
//    }


    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    BufferSupplyItem::BufferSupplyItem(const BufferSupplyItem & item) : order(item.order) {

        // Avoid self copy ...
        if (this != &item) {

            buffer->copy(item.getBufferAsIs());

            bufferSize       = item.bufferSize;
            order            = item.order;
            orderedRelease   = item.orderedRelease;
            producerSequence = item.producerSequence;
            consumerSequence = item.consumerSequence;
            atomicCounter    = item.atomicCounter.load();
            volatileCounter  = item.volatileCounter;
            multipleUsers    = item.multipleUsers;
            force            = item.force;
            fromConsumerGet  = item.fromConsumerGet;
            userInt          = item.userInt;
            userBoolean      = item.userBoolean;
            myId             = item.myId;
        }
    }


    /**
     * Method to reset this item each time it is retrieved from the supply.
     */
    void BufferSupplyItem::reset() {
        buffer->clear();
        userInt = 0;
        force = false;
        userBoolean = false;
        multipleUsers = false;
        fromConsumerGet = false;
        producerSequence = consumerSequence = 0L;
    }


    /**
     * Get the byte order used to build record.
     * @return byte order used to build record.
     */
    ByteOrder BufferSupplyItem::getOrder() const {return order;}


    /**
     * Get the unique id of this object.
     * @return unique id of this object.
     */
    uint32_t BufferSupplyItem::getMyId() const {return myId;}


    /**
     * Get the flag used to suggest a forced write to a consumer.
     * @return flag used to suggest a forced write to a consumer.
     */
    bool BufferSupplyItem::getForce() const {return force;}


    /**
     * Set the flag used to suggest a forced write to a consumer.
     * @param force flag used to suggest a forced write to a consumer.
     */
    void BufferSupplyItem::setForce(bool force) {this->force = force;}


    /**
     * Was this item obtained through a call to consumerGet()?
     * @return {@code true} only if item obtained through a call to consumerGet().
     */
    bool BufferSupplyItem::isFromConsumerGet() const {return fromConsumerGet;}


    /**
     * Set whether this item was obtained through a call to consumerGet().
     * @param fromConsumerGet {@code true} only if item obtained through
     *                        a call to consumerGet().
     */
    void BufferSupplyItem::setFromConsumerGet(bool fromConsumerGet) {
        this->fromConsumerGet = fromConsumerGet;
    }


    /**
     * Get the user integer.
     * User int gets reset to 0 each time supply.get() is called.
     * @return user integer.
     */
    int BufferSupplyItem::getUserInt() const {return userInt;}


    /**
     * Set the user integer.
     * @param i user integer.
     */
    void BufferSupplyItem::setUserInt(int i) {userInt = i;}


    /**
     * Get the user boolean.
     * User boolean gets reset to false each time supply.get() is called.
     * @return user boolean.
     */
    bool BufferSupplyItem::getUserBoolean() const {return userBoolean;}


    /**
     * Set user boolean.
     * @param usrBool user boolean.
     */
    void BufferSupplyItem::setUserBoolean(bool usrBool) {userBoolean = usrBool;}


    /**
     * Get the sequence of this item for producer.
     * @return sequence of this item for producer.
     */
    int64_t BufferSupplyItem::getProducerSequence() const {return producerSequence;}


    /**
     * Set the sequence of this item for producer.
     * @param sequence sequence of this item for producer.
     */
    void BufferSupplyItem::setProducerSequence(int64_t sequence) {this->producerSequence = sequence;}


    /**
     * Get the sequence of this item for consumer.
     * @return sequence of this item for consumer.
     */
    int64_t BufferSupplyItem::getConsumerSequence() const {return consumerSequence;}


    /**
     * Set the sequence of this item for consumer.
     * @param sequence sequence of this item for consumer.
     */
    void BufferSupplyItem::setConsumerSequence(int64_t sequence) {this->consumerSequence = sequence;}


    /**
     * Get the size in bytes of the contained ByteBuffer.
     * @return size in bytes of the contained ByteBuffer.
     */
    uint32_t BufferSupplyItem::getBufferSize() const {return bufferSize;}


    /**
     * Set the contained ByteBuffer.
     * This method is dangerous -- definitely not thread safe!
     * @param buf contained ByteBuffer.
     */
    void BufferSupplyItem::setBuffer(std::shared_ptr<ByteBuffer> buf) {
        bufferSize = buf->capacity();
        buffer = buf;
    }


    /**
     * Get the contained ByteBuffer.
     * Position is set to 0.
     * @return contained ByteBuffer.
     */
    std::shared_ptr<ByteBuffer> BufferSupplyItem::getBuffer() {
        buffer->position(0);
        return buffer;
    }


    /**
     * Get the contained ByteBuffer without any modifications.
     * @return contained ByteBuffer without any modifications.
     */
    std::shared_ptr<ByteBuffer> BufferSupplyItem::getBufferAsIs() const {
        return buffer;
    }


    /**
     * Make sure the buffer is the size needed.
     * @param capacity minimum necessary size of buffer in bytes.
     */
    void BufferSupplyItem::ensureCapacity(uint32_t capacity) {
        if (bufferSize < capacity) {
            buffer = std::make_shared<ByteBuffer>(capacity);
            buffer->order(order);
            bufferSize = capacity;
        }
    }


    /**
     * Set the number of users of this buffer.
     * If multiple users of the buffer exist,
     * keep track of all until last one is finished.
     *
     * @param users number of buffer users
     */
    void BufferSupplyItem::setUsers(int users) {
        if (users > 1) {
            multipleUsers = true;

            if (orderedRelease) {
                volatileCounter = users;
            }
            else {
                atomicCounter = users;
            }
        }
    }


    /**
     * Get the number of users of this buffer.
     * @return number of users of this buffer.
     */
    int BufferSupplyItem::getUsers() const {
        if (multipleUsers) {
            if (orderedRelease) {
                return volatileCounter;
            }
            else {
                return atomicCounter;
            }
        }
        return 1;
    }


    /**
     * Called by buffer user by way of {@link ByteBufferSupply#release(BufferSupplyItem)}
     * if no longer using it so it may be reused later.
     * @return {@code true} if no one using buffer now, else {@code false}.
     */
    bool BufferSupplyItem::decrementCounter() {
        if (!multipleUsers) return true;
        if (orderedRelease) return (--volatileCounter < 1);
        int result = atomicCounter.fetch_sub(1) - 1;
        return (result < 1);
    }


    /**
     * If a reference to this BufferSupplyItem is copied, then it is necessary to increase
     * the number of users. Although this method is not safe to call in general,
     * it is safe, for example, if a RingItem is copied in the ER <b>BEFORE</b>
     * it is copied again onto multiple output channels' rings and then released.
     * Currently this is only used in just such a situation - in the ER when a ring
     * item must be copied and placed on all extra output channels. In this case,
     * there is always at least one existing user.
     *
     * @param additionalUsers number of users to add
     */
    void BufferSupplyItem::addUsers(int additionalUsers) {
        if (additionalUsers < 1) return;

        // If there was only 1 original user of the ByteBuffer ...
        if (!multipleUsers) {
            // The original user's BB is now in the process of being copied
            // so it still exists (decrementCounter not called yet).
            // Total users now = 1 + additionalUsers.
            if (orderedRelease) {
                volatileCounter = additionalUsers + 1;
            }
            else {
                atomicCounter = (additionalUsers + 1);
            }
        }
        else {
            if (orderedRelease) {
                // Warning, this is not an atomic operation!
                volatileCounter += additionalUsers;
            }
            else {
                atomicCounter += additionalUsers;
            }
        }
    }


}
