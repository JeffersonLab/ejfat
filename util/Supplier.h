//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_SUPPLIER_H
#define UTIL_SUPPLIER_H


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
     * This class was originally written in Java and is translated to C++ here.
     * It is used to provide a very fast supply of ByteBuffer objects
     * (actually ByteBufferItem objects each of which wraps a ByteBuffer)
     * for reuse in 2 different modes (uses Disruptor software package).<p>
     *
     * 1) It can be used as a simple supply of ByteBuffer(Item)s.
     * In this mode, only get() and release() are called. A user does a {@link #get()},
     * uses that buffer, then calls {@link #release()} when done with it. If there are
     * multiple users of a single buffer (say 5), then call bufferItem.setUsers(5)
     * before it is used and the buffer is only released when all 5 users have
     * called release().<p>
     *
     * 2) It can be used as a supply of ByteBuffers in which a single
     * producer provides data for a single consumer which is waiting for that data.
     * The producer does a {@link #get()}, fills the buffer with data, and finally does a {@link #publish()}
     * to let the consumer know the data is ready. Simultaneously, a consumer does a
     * {@link #consumerGet()} to access the data buffer once it is ready. To preserve all the data
     * as the producer wrote it, use the {@link BufferSupplyItem#getBuffer()} when getting
     * the data buffer. The consumer then calls {@link #release()} when finished
     * which allows the producer to reuse the now unused buffer.<p>
     *
     *
     * <pre><code>
     *
     *   This is a graphical representation of how our ring buffer is set up in mode 2.
     *
     *   (1) The producer who calls get() will get a ring item allowing a buffer to be
     *       filled. That same user does a publish() when done filling buffer.
     *
     *   (2) The consumer who calls consumerGet() will get that ring item and will
     *       use its data. That same user does a release() when done with the buffer.
     *
     *   (3) When the consumer calls release() it frees the ring item to be used by the producer again.
     *
     *                         ||
     *                         ||
     *                         ||
     *                       ________
     *                     /    |    \
     *                    / 1 _ | _ 2 \  <---- Thread using buffers
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
    template <class T> class Supplier {

        // Ensure that SupplyItem is the base class for the T class
        static_assert(std::is_base_of<SupplyItem, T>::value, "template T must derive from SupplyItem");

    protected:

        /** Mutex for thread safety when releasing resources that were out-of-order. */
        std::mutex supplyMutex;

        /** Number of records held in this supply. */
        uint32_t ringSize = 0;

        /** Ring buffer. Variable ringSize needs to be defined first. */
        std::shared_ptr<Disruptor::RingBuffer<std::shared_ptr<T>>> ringBuffer = nullptr;


        /** Barrier to prevent buffers from being used again, before being released. */
        std::shared_ptr<Disruptor::ISequenceBarrier> barrier;
        /** Which buffer is this one? */
        std::shared_ptr<Disruptor::ISequence> sequence;
        /** All sequences for barrier. */
        std::vector<std::shared_ptr<Disruptor::ISequence>> allSeqs;
        /** Which buffer is next for the consumer? */
        int64_t nextConsumerSequence = 0L;
        /** Up to which buffer is available for the consumer? */
        int64_t availableConsumerSequence = 0L;

    
        // For thread safety

        /** True if user releases ByteBufferItems in same order as acquired. */
        bool orderedRelease;
        /** When releasing in sequence, the last sequence to have been released. */
        int64_t lastSequenceReleased = -1L;
        /** When releasing in sequence, the highest sequence to have asked for release. */
        int64_t maxSequence = -1L;
        /** When releasing in sequence, the number of sequences between maxSequence &
         * lastSequenceReleased which have called release(), but not been released yet. */
        uint32_t between = 0;

        // For item id
        int itemCounter;


    public:


        Supplier(const Supplier & supply) = delete;

        ~Supplier() {ringBuffer.reset();}


        /**
         * Default Constructor. Ring has 16 bufs, each 4096 bytes.
         * Each buf is labeled as having data with same as local endian.
         */
        Supplier() :
                Supplier(16, false) {
        }



        /**
         * Constructor. Used when wanting to avoid locks for speed purposes. Say a BufferSupplyItem
         * is used by several users. This is true in ET or emu input channels in which many evio
         * events all contain a reference to the same buffer. If the user can guarantee that all
         * the users of one buffer release it before any of the users of the next, then synchronization
         * is not necessary. If that isn't the case, then locks take care of preventing a later
         * acquired buffer from being released first and consequently everything that came before
         * it in the ring.
         *
         * @param ringSize        number of BufferSupplyItem objects in ring buffer.
         * @param bufferSize      initial size (bytes) of ByteBuffer in each BufferSupplyItem object.
         * @param order           byte order of ByteBuffer in each BufferSupplyItem object.
         * @param orderedRelease  if true, the user promises to release the BufferSupplyItems
         *                        in the same order as acquired. This avoids using
         *                        synchronized code (no locks).
         * @throws IllegalArgumentException if args &lt; 1 or ringSize not power of 2.
         */
        Supplier(int ringSize, bool orderedRelease) :
                orderedRelease(orderedRelease) {

            if (ringSize < 1) {
                throw std::runtime_error("positive args only");
            }

            if (!Disruptor::Util::isPowerOf2(ringSize)) {
                throw std::runtime_error("ringSize must be a power of 2");
            }

            // Spin first then block
            auto blockingStrategy = std::make_shared< Disruptor::BlockingWaitStrategy >();
            auto waitStrategy = std::make_shared< Disruptor::SpinCountBackoffWaitStrategy >(10000, blockingStrategy);

            // Set BufferSupplyItem static values to be used when eventFactory is creating SupplyItem objects
            // Doing things in this roundabout manner is necessary because the disruptor's
            // createSingleProducer method takes a function for created items which has no args! Thus these args,
            // needed for construction of each BufferSupplyItem, must be passed in as global parameters.
            //        T::setEventFactorySettings(order, bufferSize, orderedRelease);
            T::setEventFactorySettings(orderedRelease);

            // Create ring buffer with "ringSize" # of elements
            ringBuffer = Disruptor::RingBuffer<std::shared_ptr<T>>::createSingleProducer(
                    T::eventFactory(), ringSize, waitStrategy);

            // Barrier to keep unreleased buffers from being reused
            barrier  = ringBuffer->newBarrier();
            sequence = std::make_shared<Disruptor::Sequence>(Disruptor::Sequence::InitialCursorValue);
            allSeqs.push_back(sequence);
            ringBuffer->addGatingSequences(allSeqs);
            availableConsumerSequence = -1L;
            nextConsumerSequence = sequence->value() + 1;
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


        /**
         * Get the max number of data bytes the items in this supply can hold all together.
         * @return max number of data bytes the items in this supply can hold all together.
         */
        uint32_t getMaxRingBytes() const {
            uint32_t totalBytes = 0;
            for (int i=0; i < ringSize; i++) {
                // How does one iterate over contents??
                totalBytes += 1; // TODO: fix
            }
            return totalBytes; //(int) (ringSize*1.1*bufferSize);
        }


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
            std::shared_ptr<T> & bufItem = (*ringBuffer.get())[getSequence];

            // Get item ready for use
            bufItem->reset();

            // Store sequence for later releasing of the buffer
            bufItem->setProducerSequence(getSequence);

            return bufItem;
        }


        /**
         * Get the next "n" available item in ring buffer for writing/reading data.
         * This may only be used in conjunction with:
         * {@link #publish()} or preferably {@link #publish(std::shared_ptr<BufferSupplyItem>[]}.
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
                std::shared_ptr<T> &bufItem = (*ringBuffer.get())[seq];
                bufItem->reset();
                items[seq - lo] = bufItem;
                bufItem->setProducerSequence(seq);
            }
        }


        /**
         * Get the next available item in ring buffer for writing/reading data.
         * Does not set the ByteBuffer to position = 0 and limit = capacity.
         * In other words, it facilitates reading existing data from the buffer.
         * When finished with this item, it's up to the user to set position and
         * limit to the correct value for the next user.
         * Not sure if this method is thread-safe.
         *
         * @return next available item in ring buffer.
         * @throws InterruptedException if thread interrupted.
         */
        std::shared_ptr<T> getAsIs() {
            // Next available item claimed by data producer
            long getSequence = ringBuffer->next();

            // Get object in that position (sequence) of ring buffer
            std::shared_ptr<T> & bufItem = (*ringBuffer.get())[getSequence];
            bufItem->setFromConsumerGet(false);

            // Store sequence for later releasing of the buffer
            bufItem->setProducerSequence(getSequence);

            return bufItem;
        }


        /**
         * Get the next available item in ring buffer for getting data already written into.
         * Not sure if this method is thread-safe.
         * @return next available item in ring buffer for getting data already written into.
         * @throws InterruptedException if thread interrupted.
         */
        std::shared_ptr<T> consumerGet() {

            std::shared_ptr<T> item = nullptr;

            try  {
                // Only wait for read-volatile-memory if necessary ...
                if (availableConsumerSequence < nextConsumerSequence) {
                    availableConsumerSequence = barrier->waitFor(nextConsumerSequence);
                }

                item = (*ringBuffer.get())[nextConsumerSequence];
                item->setConsumerSequence(nextConsumerSequence++);
                item->setFromConsumerGet(true);
            }
            catch (Disruptor::AlertException & ex) {
                std::cout << ex.message() << std::endl;
            }

            return item;
        }


        /**
         * Consumer releases claim on the given ring buffer item so it becomes available for reuse.
         * This method <b>ensures</b> that sequences are released in order and is thread-safe.
         * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
         * @param item item in ring buffer to release for reuse.
         */
        void release(std::shared_ptr<T> & item) {
            if (item == nullptr) return;

            // Each item may be used by several objects/threads. It will
            // only be released for reuse if everyone releases their claim.

            int64_t seq;
            bool isConsumerGet = item->isFromConsumerGet();
            if (isConsumerGet) {
                seq = item->getConsumerSequence();
                //System.out.println(" S" + seq + "P" + item.auxIndex);
            }
            else {
                seq = item->getProducerSequence();
                //System.out.print(" P" + seq);
            }

            if (item->decrementCounter()) {
                if (orderedRelease) {
                    //System.out.println(" <" + maxSequence + ">" );
                    sequence->setValue(seq);
                    return;
                }

                supplyMutex.lock();
                {
                    // If we got a new max ...
                    if (seq > maxSequence) {
                        // If the old max was > the last released ...
                        if (maxSequence > lastSequenceReleased) {
                            // we now have a sequence between last released & new max
                            between++;
                        }

                        // Set the new max
                        maxSequence = seq;
                    }
                        // If we're < max and > last, then we're in between
                    else if (seq > lastSequenceReleased) {
                        between++;
                    }

                    // If we now have everything between last & max, release it all.
                    // This way higher sequences are never released before lower.
                    if ((maxSequence - lastSequenceReleased - 1L) == between) {
                        //System.out.println("\n**" + maxSequence + "**" );
                        sequence->setValue(maxSequence);
                        lastSequenceReleased = maxSequence;
                        between = 0;
                    }
                }
                supplyMutex.unlock();

            }

        }


        /**
         * Used to tell that the consumer that the ring buffer item is ready for consumption.
         * Not sure if this method is thread-safe.
         * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
         * @param bufferSupplyItem item available for consumer's use.
         */
        void publish(std::shared_ptr<T> & bufferSupplyItem) {
            if (bufferSupplyItem == nullptr) return;
            ringBuffer->publish(bufferSupplyItem->getProducerSequence());
        }


        /**
         * Used to tell that the consumer that the ring buffer items are ready for consumption.
         * This may only be used in conjunction with {@link #get(int32_t, std::shared_ptr<BufferSupplyItem>[])}.
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


#endif // UTIL_SUPPLIER_H
