//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "SupplyItem.h"


namespace ejfat {

    uint64_t  SupplyItem::idValue {0};
    uint32_t  SupplyItem::maxConsumers {8};
    bool      SupplyItem::factoryOrderedRelease {false};


     /**
     * Default constructor which uses value set in {@link Supplier#Supplier(int, bool)}.
      * @param consumers total number of consumers sharing the same supply items at the
      * same time. Max is 8!
      */
    SupplyItem::SupplyItem(uint32_t consumers) {
        if (consumers > maxConsumers) {
            throw std::runtime_error("too many consumers, " + std::to_string(maxConsumers) + "  max");
        }

        consumerCount  = consumers;
        orderedRelease = factoryOrderedRelease;
        myId = idValue++;
        for (int i=0; i < consumers; i++) {
            consumerSequences[i] = 0UL;
            atomicCounters[i]    = 0;
            volatileCounters[i]  = 0;
            multipleUsers[i]     = false;
        }
    }

    /**
     * Copy constructor.
     * @param item ring item to copy.
     */
    SupplyItem::SupplyItem(const SupplyItem & item) {
        // Avoid self copy ...
        if (this != &item) {
            consumerCount     = item.consumerCount;
            orderedRelease    = item.orderedRelease;
            producerSequence  = item.producerSequence;
            for (int i=0; i < consumerCount; i++) {
                consumerSequences[i] = item.consumerSequences[i];
                atomicCounters[i]    = item.atomicCounters[i].load();
                volatileCounters[i]  = item.volatileCounters[i];
                multipleUsers[i]     = item.multipleUsers[i];
            }
            fromConsumerGet   = item.fromConsumerGet;
            myId              = item.myId;
        }
    }


    /**
      * Method to reset this item each time it is retrieved from the supply.
      */
    void SupplyItem::reset() {
        fromConsumerGet = false;
        producerSequence = 0L;
        for (int i=0; i < 8; i++) {
            consumerSequences[i] = 0UL;
            atomicCounters[i]    = 0;
            volatileCounters[i]  = 0;
            multipleUsers[i]     = false;
        }
    }


    /**
     * Get the unique id of this object.
     * @return unique id of this object.
     */
    uint32_t SupplyItem::getMyId() const {return myId;}


    /**
     * Was this item obtained through a call to consumerGet()?
     * @return {@code true} only if item obtained through a call to consumerGet().
     */
    bool SupplyItem::isFromConsumerGet() const {return fromConsumerGet;}


    /**
     * Set whether this item was obtained through a call to consumerGet().
     * @param fromConsumer {@code true} only if item obtained through
     *                     a call to consumerGet().
     */
    void SupplyItem::setFromConsumerGet(bool fromConsumer) {
        this->fromConsumerGet = fromConsumer;
    }


    /**
     * Called internally by {@link Supplier#release(SupplyItem)}
     * if no longer using item so it may be reused later.
     * @param id id of consumer of this item.
     * @return {@code true} if no one using buffer now, else {@code false}.
     */
    bool SupplyItem::decrementCounter(uint32_t id) {
        if (id > 7) {
            throw std::overflow_error("too many consumers, 8 max");
        }

        if (!multipleUsers[id]) return true;
        if (orderedRelease) return (--volatileCounters[id] < 1);
        int result = atomicCounters[id].fetch_sub(1) - 1;
        return (result < 1);
    }


    /**
     * Get the sequence of this item for producer.
     * @return sequence of this item for producer.
     */
    int64_t SupplyItem::getProducerSequence() const {return producerSequence;}


    /**
     * Set the sequence of this item for producer.
     * @param sequence sequence of this item for producer.
     */
    void SupplyItem::setProducerSequence(int64_t sequence) {this->producerSequence = sequence;}


    /**
     * Get the sequence of this item for this consumer.
     * <b>User will NOT need to call this.</b>
     * @param id id of this consumer (0 to 7).
     * @return sequence of this item for this consumer.
     */
    int64_t SupplyItem::getConsumerSequence(uint32_t id) const {
        if (id > 7) {
            throw std::overflow_error("too many consumers, 8 max");
        }

        return consumerSequences[id];
    }


    /**
     * Set the sequence of this item for this consumer.
     * <b>User will NOT need to and should NOT call this.</b>
     * @param sequence sequence of this item for this consumer.
     * @param id id of this consumer (0 to 7).
     */
    void SupplyItem::setConsumerSequence(int64_t sequence, uint32_t id) {
        if (id > 7) {
            throw std::overflow_error("too many consumers, 8 max");
        }

        this->consumerSequences[id] = sequence;
    }


    /**
     * Set the number of users of this buffer.
     * If multiple users of the buffer exist,
     * keep track of all until last one is finished.
     *
     * @param users number of buffer users
     */
    void SupplyItem::setUsers(int users, uint32_t id) {
        if (id > 7) {
            throw std::overflow_error("too many consumers, 8 max");
        }

        if (users > 1) {
            multipleUsers[id] = true;

            if (orderedRelease) {
                volatileCounters[id] = users;
            }
            else {
                atomicCounters[id] = users;
            }
        }
    }


    /**
     * Get the number of users of this item by given consumer.
     * @param id id of consumer of this item.
     * @return number of users of this item by this consumer.
     */
    int SupplyItem::getUsers(uint32_t id) const {
        if (id > 7) {
            throw std::overflow_error("too many consumers, 8 max");
        }

        if (multipleUsers[id]) {
            if (orderedRelease) {
                return volatileCounters[id];
            }
            else {
                return atomicCounters[id];
            }
        }
        return 1;
    }


    /**
     * If a reference to this SupplyItem is copied, then it is necessary to increase
     * the number of users. Although this method is not safe to call in general,
     * it is safe, for example, if a RingItem is copied in the ER <b>BEFORE</b>
     * it is copied again onto multiple output channels' rings and then released.
     * Currently this is only used in just such a situation - in the ER when a ring
     * item must be copied and placed on all extra output channels. In this case,
     * there is always at least one existing user.
     *
     * @param id id of consumer of this item.
     * @param additionalUsers number of users to add
     */
    void SupplyItem::addUsers(int additionalUsers, uint32_t id) {
        if (id > 7) {
            throw std::overflow_error("too many consumers, 8 max");
        }

        if (additionalUsers < 1) return;

        // If there was only 1 original user of the ByteBuffer ...
        if (!multipleUsers[id]) {
            // The original user's BB is now in the process of being copied
            // so it still exists (decrementCounter not called yet).
            // Total users now = 1 + additionalUsers.
            if (orderedRelease) {
                volatileCounters[id] = additionalUsers + 1;
            }
            else {
                atomicCounters[id] = (additionalUsers + 1);
            }
        }
        else {
            if (orderedRelease) {
                // Warning, this is not an atomic operation!
                volatileCounters[id] += additionalUsers;
            }
            else {
                atomicCounters[id] += additionalUsers;
            }
        }
    }

}
    


