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


        SupplyItem();
        SupplyItem(const SupplyItem & item);
        ~SupplyItem() = default;
        SupplyItem & operator=(const SupplyItem & other) = delete;


    public:

        void reset();
        uint32_t getMyId() const;
        bool isFromConsumerGet() const;
        void setFromConsumerGet(bool fromConsumer);


        // User should not call these, only called by Supplier ...........

        bool decrementCounter();
        int64_t getProducerSequence() const;
        void setProducerSequence(int64_t sequence);
        int64_t getConsumerSequence() const;
        void setConsumerSequence(int64_t sequence);

        void setUsers(int users);
        int getUsers() const;
        void addUsers(int additionalUsers);

    };

}




#endif // UTIL_SUPPLYITEM_H
