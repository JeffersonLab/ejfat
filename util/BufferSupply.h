//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_BUFFERSUPPLY_H
#define UTIL_BUFFERSUPPLY_H


#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>


#include "ByteOrder.h"
#include "BufferSupplyItem.h"
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
     * uses that buffer, then calls release() when done with it. If there are
     * multiple users of a single buffer (say 5), then call bufferItem.setUsers(5)
     * before it is used and the buffer is only released when all 5 users have
     * called release().<p>
     *
     * 2) It can be used as a supply of ByteBuffers in which a single
     * producer provides data for a single consumer which is waiting for that data.
     * The producer does a get(), fills the buffer with data, and finally does a publish()
     * to let the consumer know the data is ready. Simultaneously, a consumer does a
     * consumerGet() to access the data buffer once it is ready. To preserve all the data
     * as the producer wrote it, use the {@link BufferSupplyItem.getBufferAsIs() when getting
     * the data buffer. The consumer then calls
     * release() when finished which allows the producer to reuse the
     * now unused buffer.<p>
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
    class BufferSupply {

    private:

        /** Mutex for thread safety when releasing resources that were out-of-order. */
        std::mutex supplyMutex;

        /** Max number of data bytes each ByteBuffer can hold. */
        uint32_t bufferSize = 0;

        /** Number of records held in this supply. */
        uint32_t ringSize = 0;

        /** Byte order of ByteBuffer in each ByteBufferItem. */
        ByteOrder order;


        /** Ring buffer. Variable ringSize needs to be defined first. */
        std::shared_ptr<Disruptor::RingBuffer<std::shared_ptr<BufferSupplyItem>>> ringBuffer = nullptr;


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

        BufferSupply();
        // No need to copy these things
        BufferSupply(const BufferSupply & supply) = delete;
        // By default, items are not released in order and data is local endian
        BufferSupply(int ringSize, int bufferSize,
                     const ByteOrder & order = ByteOrder::ENDIAN_LOCAL,
                     bool orderedRelease = false);


        ~BufferSupply() {ringBuffer.reset();}

        void errorAlert() const;

        uint32_t getMaxRingBytes() const;
        uint32_t getRingSize() const;
        ByteOrder getOrder() const;
        uint64_t getFillLevel() const;
        int64_t getLastSequence() const;

        std::shared_ptr<BufferSupplyItem> get();
        std::shared_ptr<BufferSupplyItem> getAsIs();
        std::shared_ptr<BufferSupplyItem> consumerGet();

        void release(std::shared_ptr<BufferSupplyItem> & item);
        void publish(std::shared_ptr<BufferSupplyItem> & item);

    };

}


#endif // UTIL_BUFFERSUPPLY_H
