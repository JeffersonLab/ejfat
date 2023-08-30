//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_SUPPLIERN_H
#define UTIL_SUPPLIERN_H


#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <iostream>
#include <type_traits>


#include "SupplyItem.h"
#include "Disruptor/Disruptor.h"
#include "Disruptor/SpinCountBackoffWaitStrategy.h"


namespace ejfat {


    /**
     * <p>
     * This class is written in templated form so the fast Disruptor ring buffer
     * can be used to supply different types of items/objects.</p>
     *
     * It is to be used as a supply of items in which a single
     * producer provides data/content for N consumers which are waiting for that data.
     * The producer does a {@link #get()}, fills the item with data, and finally does a
     * {@link #publish(std::shared_ptr<T>)}
     * to let the consumer know the data is ready. Simultaneously, each consumer does a
     * {@link #consumerGet()} to access the item and its data once it is ready.
     * The consumer then calls {@link #release()} when finished
     * which allows the producer to reuse the now unused item.
     * <p>
     * The number of clients must be set in the constructor. It is completely up to
     * the N consumers as to any orchestration needed over who does what to which
     * ring entries. For example, with 2 consumers, one may operate on all "odd" items
     * and the other on "even" items. This, of course, depends on the type of items
     * being supplied.
     *
     *
     * <pre><code>
     *
     *   This is a graphical representation of how our ring buffer is set up in mode 2.
     *
     *   (1) The producer who calls get() will get a ring item allowing a it to be
     *       filled. That same user does a publish() when done filling item.
     *
     *   (2) The consumer who calls consumerGet() will get that ring item and will
     *       use its data. That same user does a release() when done with the item.
     *
     *   (3) When the consumer calls release() it frees the ring item to be used by the producer again.
     *
     *                         ||
     *                         ||
     *                         ||
     *                       ________
     *                     /    |    \
     *                    / 1 _ | _ 2 \  <---- Thread using items
     *                   | __ /   \ __ |               |
     *                   |  6 |    | 3 |               V
     *             ^     | __ | __ | __| ==========================
     *             |      \   5 |   4 /       Barrier
     *         Producer->  \ __ | __ /
     *
     *
     * </code></pre>
     *
     * @version 6.0
     * @since 6.0 4/28/22
     * @author timmer
     */
    template <class T> class SupplierN {

        // Ensure that SupplyItem is the base class for the T class
        static_assert(std::is_base_of<SupplyItem, T>::value, "template T must derive from SupplyItem");

    protected:

        /** Mutex for thread safety when releasing resources that were out-of-order. One per consumer. */
        std::mutex *supplyMutex;

        /** Number of records held in this supply. */
        uint32_t ringSize = 0;

        /** Number of consumers to be consuming ring items. */
        uint32_t consumerCount = 1;

        /** Ring buffer. Variable ringSize needs to be defined first. */
        std::shared_ptr<Disruptor::RingBuffer<std::shared_ptr<T>>> ringBuffer = nullptr;

        /** One barrier to prevent items from being used again, before being released. */
        std::shared_ptr<Disruptor::ISequenceBarrier> barrier;

        /** One sequence for each consumer (array)
         * since each is consuming a different item at ony one time. */
        std::shared_ptr<Disruptor::ISequence> *sequence;

        /** All sequences for barrier. */
        std::vector<std::shared_ptr<Disruptor::ISequence>> allSeqs;

        /** Which item is next for a consumer (array)? */
        int64_t *nextConsumerSequence;

        /** Up to which item is available for a consumer (array)? */
        int64_t *availableConsumerSequence;

        // For thread safety

        /** True if user releases items in same order as acquired. */
        bool orderedRelease;
        /** When releasing in sequence, the last sequence to have been released. One per consumer. */
        int64_t *lastSequenceReleased;
        /** When releasing in sequence, the highest sequence to have asked for release. One per consumer. */
        int64_t *maxSequence;
        /** When releasing in sequence, the number of sequences between maxSequence &
         * lastSequenceReleased which have called release(), but not been released yet.
         * One per consumer. */
        uint32_t *between;

        // For item id
        int itemCounter;


    public:


        SupplierN(const SupplierN & supply) = delete;

        ~SupplierN() {
            delete(sequence);
            delete(availableConsumerSequence);
            delete(nextConsumerSequence);
            delete(lastSequenceReleased);
            delete(maxSequence);
            delete(between);
            delete(supplyMutex);

            ringBuffer.reset();
        }


        /**
         * Default Constructor. Ring has 16 bufs, no ordered release, only 1 consumer.
         */
        SupplierN() :
                SupplierN(16, false, 1) {
        }

        
        /**
         * Constructor. Used when wanting to avoid locks for speed purposes. Say a T item
         * is used by several users. This is true in ET or emu input channels in which many evio
         * events all contain a reference to the same buffer. If the user can guarantee that all
         * the users of one item release it before any of the users of the next, then synchronization
         * is not necessary. If that isn't the case, then locks take care of preventing a later
         * acquired item from being released first and consequently everything that came before
         * it in the ring.
         *
         * @param ringSize        number of T item in ring buffer.
         * @param orderedRelease  if true, the user promises to release the T items
         *                        in the same order as acquired. This avoids using
         *                        synchronized code (no locks).
         * @param consumersCount  number of consumers who will be operating on each ring item.
         * @throws IllegalArgumentException if ringSize arg &lt; 1 or not power of 2.
         */
        SupplierN(uint32_t ringSize, bool orderedRelease, uint32_t consumerCount) :
                orderedRelease(orderedRelease) {

            if (consumerCount > 8) {
                throw std::runtime_error("too many consumers, 8 max");
            }

            if (ringSize < 1 || consumerCount < 1) {
                throw std::runtime_error("positive args only");
            }

            if (!Disruptor::Util::isPowerOf2(ringSize)) {
                throw std::runtime_error("ringSize must be a power of 2");
            }

            this->ringSize = ringSize;
            this->consumerCount = consumerCount;

            // All the supply items need to know if the release is ordered
            SupplyItem::factoryOrderedRelease = orderedRelease;

            // Spin first then block
            auto blockingStrategy = std::make_shared< Disruptor::BlockingWaitStrategy >();
            auto waitStrategy = std::make_shared< Disruptor::SpinCountBackoffWaitStrategy >(10000, blockingStrategy);

            // Any specs on the "T" items need to be set with the T class' static functions
            // BEFORE this constructor is called. That way they can be constructed below using
            // their no-arg constructor.

            // Create ring buffer with "ringSize" # of elements
            ringBuffer = Disruptor::RingBuffer<std::shared_ptr<T>>::createSingleProducer(
                    T::eventFactory(), ringSize, waitStrategy);

            // All csonsumers share 1 barrier to keep unreleased buffers from being reused
            barrier = ringBuffer->newBarrier();

            // One sequence for each consumer
            sequence = new std::shared_ptr<Disruptor::ISequence>[consumerCount];
            availableConsumerSequence = new int64_t[consumerCount];
            nextConsumerSequence = new int64_t[consumerCount];
            lastSequenceReleased = new int64_t[consumerCount];
            maxSequence = new int64_t[consumerCount];
            between = new uint32_t[consumerCount];
            supplyMutex = new std::mutex[consumerCount];

            for (int i=0; i < consumerCount; i++) {
                sequence[i] = std::make_shared<Disruptor::Sequence>(Disruptor::Sequence::InitialCursorValue);
                allSeqs.push_back(sequence[i]);

                availableConsumerSequence[i] = -1L;
                nextConsumerSequence[i] = sequence[i]->value() + 1;
                lastSequenceReleased[i] = -1L;
                maxSequence[i] = -1L;
                between[i] = 0;
            }

            // All consumers must release a ring item before it becomes available for reuse
            ringBuffer->addGatingSequences(allSeqs);
        }


        /**
         * Method to have sequence barriers throw a Disruptor's AlertException.
         * In this case, we can use it to warn write and compress threads which
         * are waiting on barrier.waitFor() in {@link #getToCompress(uint32_t)} and
         * {@link #getToWrite()}. Do this in case of a write, compress, or some other error.
         * This allows any threads waiting on these 2 methods to wake up, clean up,
         * and exit.
         */
        void errorAlert() const {
            barrier->alert();
        }


//        /**
//         * Get the max number of data bytes the items in this supply can hold all together.
//         * @return max number of data bytes the items in this supply can hold all together.
//         */
//        uint32_t getMaxRingBytes() const {
//            uint32_t totalBytes = 0;
//            for (int i=0; i < ringSize; i++) {
//                // How does one iterate over contents??
//                totalBytes += 1; // TODO: fix
//            }
//            return totalBytes; //(int) (ringSize*1.1*bufferSize);
//        }


        /**
         * Get the number of records in this supply.
         * @return number of records in this supply.
         */
        uint32_t getRingSize() const {return ringSize;}


        /**
         * Get the percentage of data-filled but unwritten records in ring.
         * Value of 0 means everything's been written. Value of 100 means
         * that all records in the ring are filled with data (perhaps in
         * various stages of being compressed) and have not been written yet.
         *
         * @return percentage of used records in ring.
         */
        uint64_t getFillLevel() const {
            return 100*(ringBuffer->cursor() - ringBuffer->getMinimumGatingSequence())/ringBuffer->bufferSize();
        }


        /**
         * Get the sequence of last ring buffer item published (seq starts at 0).
         * @return sequence of last ring buffer item published (seq starts at 0).
         */
        int64_t getLastSequence() const {
            return ringBuffer->cursor();
        }


        /**
         * Get the next available item in ring buffer for writing/reading data.
         * Not sure if this method is thread-safe.
         *
         * @return next available item in ring buffer.
         * @throws InterruptedException if thread interrupted.
         */
        std::shared_ptr<T> get() {
            // Next available item claimed by data producer
            long getSequence = ringBuffer->next();

            // Get object in that position (sequence) of ring buffer
            std::shared_ptr<T> bufItem = (*ringBuffer.get())[getSequence];

            // Get item ready for use
            bufItem->reset();

            // Store sequence for later releasing of the buffer
            bufItem->setProducerSequence(getSequence);

            return bufItem;
        }


        /**
         * Get the next "n" available item in ring buffer for writing/reading data.
         * This may only be used in conjunction with:
         * {@link #publish(std::shared_ptr<T>)} or preferably {@link #publish(std::shared_ptr<T>[]}.
         * Not sure if this method is thread-safe.
         *
         * @param n number of ring buffer items to get.
         * @param items array big enough to hold an array of n items.
         * @throws InterruptedException if thread interrupted.
         */
        void get(int32_t n, std::shared_ptr<T> items[]) {
            // Next available n items claimed by data producer
            long hi = ringBuffer->next(n);
            long lo = hi - (n - 1);

            for (long seq = lo; seq <= hi; seq++) {
                // Get object in that position (sequence) of ring buffer
                std::shared_ptr<T> bufItem = (*ringBuffer.get())[seq];
                bufItem->reset();
                items[seq - lo] = bufItem;
                bufItem->setProducerSequence(seq);
            }
        }


        /**
         * Get the next available item in ring buffer for writing/reading data.
         * Does NOT reset the item.
         * In other words, it facilitates reading existing data from the item.
         * When finished with this item, it's up to the user to set its state
         * for the next user.
         * Not sure if this method is thread-safe.
         *
         * @return next available item in ring buffer.
         * @throws InterruptedException if thread interrupted.
         */
        std::shared_ptr<T> getAsIs() {
            // Next available item claimed by data producer
            long getSequence = ringBuffer->next();

            // Get object in that position (sequence) of ring buffer
            std::shared_ptr<T> bufItem = (*ringBuffer.get())[getSequence];
            bufItem->setFromConsumerGet(false);

            // Store sequence for later releasing of the buffer
            bufItem->setProducerSequence(getSequence);

            return bufItem;
        }


        /**
         * Get the next available item in ring buffer for getting data already written into.
         * Not sure if this method is thread-safe.
         * @param id which consumer is this (0 to N-1).
         * @return next available item in ring buffer for getting data already written into.
         * @throws InterruptedException if thread interrupted.
         */
        std::shared_ptr<T> consumerGet(uint32_t id = 0) {
            if (id > consumerCount - 1) {
                throw std::runtime_error("too many consumers, id = " + std::to_string(consumerCount - 1) + " max");
            }

            std::shared_ptr<T> item = nullptr;

            try  {
                // Only wait for read-volatile-memory if necessary ...
                if (availableConsumerSequence[id] < nextConsumerSequence[id]) {
                    availableConsumerSequence[id] = barrier->waitFor(nextConsumerSequence[id]);
                }

                item = (*ringBuffer.get())[nextConsumerSequence[id]];
                item->setConsumerSequence(nextConsumerSequence[id]++, id);
                item->setFromConsumerGet(true);
            }
            catch (Disruptor::AlertException & ex) {
                std::cout << ex.message() << std::endl;
            }

            return item;
        }


        /**
         * Consumer releases claim on the given item so it becomes available for reuse.
         * This method <b>ensures</b> that sequences are released in order and is thread-safe.
         * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
         * @param item item in ring buffer to release for reuse.
         */
        void release(std::shared_ptr<T> & item, uint32_t id = 0) {
            if (id > consumerCount - 1) {
                throw std::runtime_error("too many consumers, id = " + std::to_string(consumerCount - 1) + " max");
            }

            if (item == nullptr) return;

            // For a single consumer, each item may be used by several objects/threads.
            // It will only be released, by a consumer, for reuse if everyone releases their claim.
            // Note also that, in addition, all consumers must release an item for it to become
            // available for useuse.

            int64_t seq;
            bool isConsumerGet = item->isFromConsumerGet();
            if (isConsumerGet) {
                seq = item->getConsumerSequence(id);
                //System.out.println(" S" + seq + "P" + item.auxIndex);
            }
            else {
                seq = item->getProducerSequence();
                //System.out.print(" P" + seq);
            }

            if (item->decrementCounter(id)) {
                if (orderedRelease) {
                    //System.out.println(" <" + maxSequence + ">" );
                    sequence[id]->setValue(seq);
                    return;
                }

                supplyMutex[id].lock();
                {
                    // If we got a new max ...
                    if (seq > maxSequence[id]) {
                        // If the old max was > the last released ...
                        if (maxSequence[id] > lastSequenceReleased[id]) {
                            // we now have a sequence between last released & new max
                            between[id]++;
                        }

                        // Set the new max
                        maxSequence[id] = seq;
                    }
                        // If we're < max and > last, then we're in between
                    else if (seq > lastSequenceReleased[id]) {
                        between[id]++;
                    }

                    // If we now have everything between last & max, release it all.
                    // This way higher sequences are never released before lower.
                    if ((maxSequence[id] - lastSequenceReleased[id] - 1L) == between[id]) {
                        //System.out.println("\n**" + maxSequence[id] + "**" );
                        sequence[id]->setValue(maxSequence[id]);
                        lastSequenceReleased[id] = maxSequence[id];
                        between[id] = 0;
                    }
                }
                supplyMutex[id].unlock();
            }
        }


        /**
         * Used to tell that the consumer that the item is ready for consumption.
         * Not sure if this method is thread-safe.
         * To be used in conjunction with {@link #get()}.
         * @param item item available for consumer's use.
         */
        void publish(std::shared_ptr<T> & item) {
            if (item == nullptr) return;
            ringBuffer->publish(item->getProducerSequence());
        }


        /**
         * Used to tell that the consumer that the items are ready for consumption.
         * This may only be used in conjunction with {@link #get(int32_t, std::shared_ptr<T>[])}.
         * Not sure if this method is thread-safe.
         * @param n number of array items available for consumer's use.
         * @param items array of items available for consumer's use.
         */
        void publish(int32_t n, std::shared_ptr<T> items[]) {
            if (n < 1 || items == nullptr) return;
            ringBuffer->publish(items[0]->getProducerSequence(), items[n-1]->getProducerSequence());
        }

    };

}


#endif // UTIL_SUPPLIERN_H
