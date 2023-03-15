//
// Copyright 2023, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_SUPPLYITEM_H
#define UTIL_SUPPLYITEM_H


#include <memory>
#include <atomic>
#include <functional>
#include <cstring>


namespace ejfat {

    /**
     * This class provides the base class for items which are supplied by the Supplier class.
     * Use this to inherit from and provide basic supply logic.
     *
     * @date 3/13/2023
     * @author timmer
     */
    class SupplyItem {

    public:

        /** Assign each record a unique id for debugging purposes. */
        static uint64_t idValue;
        /** True if user releases SupplyItems in same order as acquired (set when constructing Supplier). */
        static bool factoryOrderedRelease;

    protected:

        /** True if user releases SupplyItems in same order as acquired. */
        bool orderedRelease = false;

        /** Sequence in which this object was taken from ring for use by a producer with get(). */
        int64_t producerSequence = 0UL;

        /** Sequence in which this object was taken from ring for use by a consumer with consumerGet(). */
        int64_t consumerSequence = 0UL;

        /** Track more than one user so this object can be released for reuse. */
        std::atomic<int> atomicCounter {0};

        /** Track more than one user so this object can be released for reuse. */
        volatile int volatileCounter {0};

        /** If true, we're tracking more than one user. */
        bool multipleUsers = false;

        /**
         * Need to track whether this item was obtained through consumerGet() or
         * through either get() / getAsIs() since they must be released differently.
         */
        bool fromConsumerGet = false;


        // For testing purposes

        /** Unique id for each object of this class. */
        uint32_t myId;


        /**
         * Default constructor which uses value set in {@link Supplier#Supplier(int, bool)}.
         */
        SupplyItem() {
            orderedRelease = factoryOrderedRelease;
            myId = idValue++;
        }

        /**
         * Copy constructor.
         * @param item ring item to copy.
         */
        SupplyItem(const SupplyItem & item) {
            // Avoid self copy ...
            if (this != &item) {
                orderedRelease   = item.orderedRelease;
                producerSequence = item.producerSequence;
                consumerSequence = item.consumerSequence;
                atomicCounter    = item.atomicCounter.load();
                volatileCounter  = item.volatileCounter;
                multipleUsers    = item.multipleUsers;
                fromConsumerGet  = item.fromConsumerGet;
                myId             = item.myId;
            }
        }

        ~SupplyItem() = default;

        SupplyItem & operator=(const SupplyItem & other) = delete;

    public:

        /**
          * Method to reset this item each time it is retrieved from the supply.
          */
        void reset() {
            multipleUsers = false;
            fromConsumerGet = false;
            producerSequence = consumerSequence = 0L;
        }


        /**
         * Get the unique id of this object.
         * @return unique id of this object.
         */
        uint32_t getMyId() const {return myId;}


        /**
         * Was this item obtained through a call to consumerGet()?
         * @return {@code true} only if item obtained through a call to consumerGet().
         */
        bool isFromConsumerGet() const {return fromConsumerGet;}


        /**
         * Set whether this item was obtained through a call to consumerGet().
         * @param fromConsumer {@code true} only if item obtained through
         *                     a call to consumerGet().
         */
        void setFromConsumerGet(bool fromConsumer) {
            this->fromConsumerGet = fromConsumer;
        }


        // User should not call these, only called by Supplier ...........

        /**
         * Called internally by {@link Supplier#release(SupplyItem)}
         * if no longer using item so it may be reused later.
         * <b>User should NEVER call this.</b>
         * @return {@code true} if no one using buffer now, else {@code false}.
         */
        bool decrementCounter() {
            if (!multipleUsers) return true;
            if (orderedRelease) return (--volatileCounter < 1);
            int result = atomicCounter.fetch_sub(1) - 1;
            return (result < 1);
        }


        /**
         * Get the sequence of this item for producer.
         * <b>User will NOT need to call this.</b>
         * @return sequence of this item for producer.
         */
        int64_t getProducerSequence() const {return producerSequence;}


        /**
         * Set the sequence of this item for producer.
         * <b>User will NOT need to and should NOT call this.</b>
         * @param sequence sequence of this item for producer.
         */
        void setProducerSequence(int64_t sequence) {this->producerSequence = sequence;}


        /**
         * Get the sequence of this item for consumer.
         * <b>User will NOT need to call this.</b>
         * @return sequence of this item for consumer.
         */
        int64_t getConsumerSequence() const {return consumerSequence;}


        /**
         * Set the sequence of this item for consumer.
         * <b>User will NOT need to and should NOT call this.</b>
         * @param sequence sequence of this item for consumer.
         */
        void setConsumerSequence(int64_t sequence) {this->consumerSequence = sequence;}


        /**
         * Set the number of users of this buffer.
         * If multiple users of the buffer exist,
         * keep track of all until last one is finished.
         *
         * @param users number of buffer users
         */
        void setUsers(int users) {
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
         * Get the number of users of this item.
         * @return number of users of this item.
         */
        int getUsers() const {
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
         * If a reference to this SupplyItem is copied, then it is necessary to increase
         * the number of users. Although this method is not safe to call in general,
         * it is safe, for example, if a RingItem is copied in the ER <b>BEFORE</b>
         * it is copied again onto multiple output channels' rings and then released.
         * Currently this is only used in just such a situation - in the ER when a ring
         * item must be copied and placed on all extra output channels. In this case,
         * there is always at least one existing user.
         *
         * @param additionalUsers number of users to add
         */
        void addUsers(int additionalUsers) {
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

    };

    uint64_t  SupplyItem::idValue = 0;
    bool      SupplyItem::factoryOrderedRelease = false;

}




#endif // UTIL_SUPPLYITEM_H
