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
#include <string>


namespace ejfat {

    /**
     * This class provides the base class for items which are supplied by the Supplier class.
     * Use this to inherit from and provide basic supply logic.
     * This allows for up to 8 consumers to have access to this item at any one time
     * (ie they share the same ring buffer's barrier).
     *
     * @date 3/13/2023
     * @author timmer
     */
    class SupplyItem {

        template<typename T> friend class Supplier;
        template<typename T> friend class SupplierN;

    public:

        /** Assign each record a unique id for debugging purposes. */
        static uint64_t idValue;
        /** Max number of consumers whose associated data can be stored here. */
        static uint32_t maxConsumers;
        /** True if user releases SupplyItems in same order as acquired (set when constructing Supplier). */
        static bool factoryOrderedRelease;

    protected:
        /** How many consumers have access to this item (share same ring buffer barrier)? */
        int consumerCount = 1;

        /** True if user releases SupplyItems in same order as acquired. */
        bool orderedRelease = false;

        /** Sequence in which this object was taken from ring for use by a producer with get(). */
        int64_t producerSequence = 0UL;

        /** Sequences for multiple consumers in which this object was taken from ring for use with consumerGet(). */
        int64_t consumerSequences[8];

        /** Track more than one user so this object can be released for reuse. */
        std::atomic<int> atomicCounters[8];

        /** Track more than one user so this object can be released for reuse. */
        volatile int volatileCounters[8];

        /** If true, we're tracking more than one user (for each consumer). */
        bool multipleUsers[8];

        /**
         * Need to track whether this item was obtained through consumerGet() or
         * through either get() / getAsIs() since they must be released differently.
         */
        bool fromConsumerGet = false;


        // For testing purposes

        /** Unique id for each object of this class. */
        uint32_t myId;


        SupplyItem(uint32_t consumerCount = 1);
        SupplyItem(const SupplyItem & item);
        ~SupplyItem() = default;


    public:

        SupplyItem & operator=(const SupplyItem & other) = delete;


        void reset();
        uint32_t getMyId() const;

        bool isFromConsumerGet() const;
        void setFromConsumerGet(bool fromConsumer);

        void setUsers(int users, uint32_t id = 0);
        int  getUsers(uint32_t id = 0) const;
        void addUsers(int additionalUsers, uint32_t id = 0);

    protected:

        bool decrementCounter(uint32_t id = 0);

        int64_t getProducerSequence() const;
        void setProducerSequence(int64_t sequence);

        int64_t getConsumerSequence(uint32_t id = 0) const;
        void setConsumerSequence(int64_t sequence, uint32_t id = 0);
    };

}




#endif // UTIL_SUPPLYITEM_H
