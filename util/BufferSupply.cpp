//
// Copyright (c) 2020, Jefferson Science Associates
//
// Thomas Jefferson National Accelerator Facility
// EPSCI Group
//
// 12000, Jefferson Ave, Newport News, VA 23606
// Phone : (757)-269-7100
//

#include <iostream>
#include "BufferSupply.h"


namespace ejfat {



    /**
     * Default Constructor. Ring has 16 bufs, each 4096 bytes.
     * Each buf is labeled as having data with same as local endian.
     */
    BufferSupply::BufferSupply() :
            BufferSupply(16, 4096, ByteOrder::ENDIAN_LITTLE, false) {
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
    BufferSupply::BufferSupply(int ringSize, int bufferSize, const ByteOrder & order, bool orderedRelease) :
                bufferSize(bufferSize), order(order), orderedRelease(orderedRelease) {

        if (ringSize < 1 || bufferSize < 1) {
            throw std::runtime_error("positive args only");
        }

        if (!Disruptor::Util::isPowerOf2(ringSize)) {
            throw std::runtime_error("ringSize must be a power of 2");
        }

        // Spin first then block
        auto blockingStrategy = std::make_shared< Disruptor::BlockingWaitStrategy >();
        auto waitStrategy = std::make_shared< Disruptor::SpinCountBackoffWaitStrategy >(10000, blockingStrategy);

        // Set BufferSupplyItem static values to be used when eventFactory is creating BufferSupplyItem objects
        // Doing things in this roundabout manor is necessary because the disruptor's
        // createSingleProducer method takes a function for created items which has no args! Thus these args,
        // needed for construction of each BufferSupplyItem, must be passed in as global parameters.
        BufferSupplyItem::setEventFactorySettings(order, bufferSize, orderedRelease);

        // Create ring buffer with "ringSize" # of elements
        ringBuffer = Disruptor::RingBuffer<std::shared_ptr<BufferSupplyItem>>::createSingleProducer(
                BufferSupplyItem::eventFactory(), ringSize, waitStrategy);

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
    void BufferSupply::errorAlert() const {
        barrier->alert();
    }


    /**
     * Get the max number of bytes the records in this supply can hold all together.
     * @return max number of bytes the records in this supply can hold all together.
     */
    uint32_t BufferSupply::getMaxRingBytes() const {return (int) (ringSize*1.1*bufferSize);}


    /**
     * Get the number of records in this supply.
     * @return number of records in this supply.
     */
    uint32_t BufferSupply::getRingSize() const {return ringSize;}


    /**
     * Get the percentage of data-filled but unwritten records in ring.
     * Value of 0 means everything's been written. Value of 100 means
     * that all records in the ring are filled with data (perhaps in
     * various stages of being compressed) and have not been written yet.
     *
     * @return percentage of used records in ring.
     */
    uint64_t BufferSupply::getFillLevel() const {
        return 100*(ringBuffer->cursor() - ringBuffer->getMinimumGatingSequence())/ringBuffer->bufferSize();
    }


    /**
     * Get the sequence of last ring buffer item published (seq starts at 0).
     * @return sequence of last ring buffer item published (seq starts at 0).
     */
    int64_t BufferSupply::getLastSequence() const {
        return ringBuffer->cursor();
    }


    /**
     * Get the byte order used to build record.
     * @return byte order used to build record.
     */
    ByteOrder BufferSupply::getOrder() const {return order;}


    /**
     * Get the next available item in ring buffer for writing/reading data.
     * Not sure if this method is thread-safe.
     * 
     * @return next available item in ring buffer.
     * @throws InterruptedException if thread interrupted.
     */
    std::shared_ptr<BufferSupplyItem> BufferSupply::get() {
        // Next available item claimed by data producer
        long getSequence = ringBuffer->next();

        // Get object in that position (sequence) of ring buffer
        std::shared_ptr<BufferSupplyItem> & bufItem = (*ringBuffer.get())[getSequence];

        // Get item ready for use
        bufItem->reset();

        // Store sequence for later releasing of the buffer
        bufItem->setProducerSequence(getSequence);

        return bufItem;
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
    std::shared_ptr<BufferSupplyItem> BufferSupply::getAsIs() {
        // Next available item claimed by data producer
        long getSequence = ringBuffer->next();

        // Get object in that position (sequence) of ring buffer
        std::shared_ptr<BufferSupplyItem> & bufItem = (*ringBuffer.get())[getSequence];
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
    std::shared_ptr<BufferSupplyItem> BufferSupply::consumerGet() {

        std::shared_ptr<BufferSupplyItem> item = nullptr;

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
     void BufferSupply::release(std::shared_ptr<BufferSupplyItem> & item) {
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
     void BufferSupply::publish(std::shared_ptr<BufferSupplyItem> & bufferSupplyItem) {
        if (bufferSupplyItem == nullptr) return;
        ringBuffer->publish(bufferSupplyItem->getProducerSequence());
    }


}


