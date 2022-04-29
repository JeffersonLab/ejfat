/*
 * Copyright (c) 2014, Jefferson Science Associates
 *
 * Thomas Jefferson National Accelerator Facility
 * Data Acquisition Group
 *
 * 12000, Jefferson Ave, Newport News, VA 23606
 * Phone : (757)-269-7100
 *
 */

package org.jlab.coda.emu.support.data;


import com.lmax.disruptor.*;
import org.jlab.coda.jevio.EvioNodePool;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;

import static com.lmax.disruptor.RingBuffer.createSingleProducer;

/**
 * This class is used to provide a very fast supply of ByteBuffer objects
 * (actually ByteBufferItem objects each of which wraps a ByteBuffer)
 * for reuse in 3 different modes (uses Disruptor software package).<p>
 *
 * 1) It can be used as a simple supply of ByteBuffer(Item)s.
 * In this mode, only get() and release() are called. A user does a get(),
 * uses that buffer, then calls release() when done with it. If there are
 * multiple users of a single buffer (say 5), then call bufferItem.setUsers(5)
 * before it is used and the buffer is only released when all 5 users have
 * called release().<p>
 *
 * 2) As in the first usage, it can be used as a supply of ByteBuffers,
 * but each buffer can be preset to a specific ByteBuffer object. Thus
 * it can act as a supply of buffers in which each contains specific data.
 * Because of the circular nature of the ring used to implement this code,
 * after all ByteBuffers have been gotten by the user for the first time,
 * it starts back over with the first -- going round and round.<p>
 *
 * To implement this, use the constructor which takes a list of ByteBuffer
 * objects with which to fill this supply. The user does a getAsIs() which
 * does <b>not</b> clear the buffer's position and limit. When finished
 * reading/writing, user calls release(). It's up to the user to maintain
 * proper values for the buffer's position and limit since it will be used again.
 * If there are multiple users of a single buffer (say 5), then call
 * bufferItem.setUsers(5) before it is used and the buffer is only released
 * when all 5 users have called release().<p>
 *
 * 3) It can be used as a supply of ByteBuffers in which a single
 * producer provides data for a single consumer which is waiting for that data.
 * The producer does a get(), fills the buffer with data, and finally does a publish()
 * to let the consumer know the data is ready. Simultaneously, a consumer does a
 * consumerGet() to access the data once it is ready. The consumer then calls
 * release() when finished which allows the producer to reuse the
 * now unused buffer.<p>
 *
 * @author timmer (4/7/14)
 */
public class ByteBufferSupply {

    /** Initial size, in bytes, of ByteBuffers contained in each ByteBufferItem in ring. */
    private int bufferSize;

    /** Byte order of ByteBuffer in each ByteBufferItem. */
    private ByteOrder order;

    /** Are the buffers created, direct? */
    private boolean direct;

    /** Ring buffer. */
    private final RingBuffer<ByteBufferItem> ringBuffer;

    /** Barrier to prevent buffers from being used again, before being released. */
    private final SequenceBarrier barrier;

    /** Which buffer is this one? */
    private final Sequence sequence;

    /** Which buffer is next for the consumer? */
    private long nextConsumerSequence;

    /** Up to which buffer is available for the consumer? */
    private long availableConsumerSequence = -1L;

    /** Object to be associated with each ByteBufferItem. */
    public Object auxObject;

    //------------------------------------------
    // For thread safety
    //------------------------------------------

    /** True if user releases ByteBufferItems in same order as acquired. */
    private final boolean orderedRelease;

    /** When releasing in sequence, the last sequence to have been released. */
    private long lastSequenceReleased = -1L;

    /** When releasing in sequence, the highest sequence to have asked for release. */
    private long maxSequence = -1L;

    /** When releasing in sequence, the number of sequences between maxSequence &
     * lastSequenceReleased which have called release(), but not been released yet. */
    private int between;

    //------------------------------------------
    // For item id
    //------------------------------------------

    private int itemCounter;



    /** Class used to initially create all items in ring buffer. */
    private final class ByteBufferFactory implements EventFactory<ByteBufferItem> {
        public ByteBufferItem newInstance() {
            return new ByteBufferItem(bufferSize, order, direct,
                                      orderedRelease, itemCounter++);
        }
    }


    /** Class used to associate a separate object with each item in ring buffer. */
    private final class AuxByteBufferFactory implements EventFactory<ByteBufferItem> {

        private int index;
        private final EvioNodePool[] pools;

        public AuxByteBufferFactory(EvioNodePool[] pools) {this.pools = pools;}

        public ByteBufferItem newInstance() {
//System.out.println("BBS constructor: pools[" + index + "].id = " + pools[index].getId());
            ByteBufferItem item = new ByteBufferItem(bufferSize, order, direct,
                                                     orderedRelease, itemCounter++,
                                                     pools[index], pools[index].getId());
            index++;
            return item;
        }
    }


    /** Class used to initially create all items in ring buffer. */
    private final class PredefinedByteBufferFactory implements EventFactory<ByteBufferItem> {

        private int index;
        private final List<ByteBuffer> bufList;

        public PredefinedByteBufferFactory(List<ByteBuffer> bufList) {
            this.bufList = bufList;
        }

        public ByteBufferItem newInstance() {
            return new ByteBufferItem(bufList.get(index++), orderedRelease, itemCounter++);
        }
    }


    /**
     * Constructor.
     * Buffers are big endian and not direct.
     * @param ringSize    number of ByteBufferItem objects in ring buffer.
     * @param bufferSize  initial size (bytes) of ByteBuffer in each ByteBufferItem object.
     * @throws IllegalArgumentException if args &lt; 1 or ringSize not power of 2.
     */
    public ByteBufferSupply(int ringSize, int bufferSize)
            throws IllegalArgumentException {

        this(ringSize, bufferSize, ByteOrder.BIG_ENDIAN, false);
    }

    /**
     * Constructor.
     *
     * @param ringSize    number of ByteBufferItem objects in ring buffer.
     * @param bufferSize  initial size (bytes) of ByteBuffer in each ByteBufferItem object.
     * @param order       byte order of ByteBuffer in each ByteBufferItem object.
     * @param direct      if true, make ByteBuffers direct.
     * @throws IllegalArgumentException if args &lt; 1 or ringSize not power of 2.
     */
    public ByteBufferSupply(int ringSize, int bufferSize, ByteOrder order, boolean direct)
            throws IllegalArgumentException {
        this(ringSize, bufferSize, order, direct, false);
    }


    /**
     * Constructor. Used when wanting to avoid locks for speed purposes. Say a ByteBufferItem
     * is used by several users. This is true in ET or emu input channels in which many evio
     * events all contain a reference to the same buffer. If the user can guarantee that all
     * the users of one buffer release it before any of the users of the next, then synchronization
     * is not necessary. If that isn't the case, then locks take care of preventing a later
     * acquired buffer from being released first and consequently everything that came before
     * it in the ring.
     *
     * @param ringSize        number of ByteBufferItem objects in ring buffer.
     * @param bufferSize      initial size (bytes) of ByteBuffer in each ByteBufferItem object.
     * @param order           byte order of ByteBuffer in each ByteBufferItem object.
     * @param direct          if true, make ByteBuffers direct.
     * @param orderedRelease  if true, the user promises to release the ByteBufferItems
     *                        in the same order as acquired. This avoids using
     *                        synchronized code (no locks).
     * @throws IllegalArgumentException if args &lt; 1 or ringSize not power of 2.
     */
    public ByteBufferSupply(int ringSize, int bufferSize, ByteOrder order,
                            boolean direct, boolean orderedRelease)
            throws IllegalArgumentException {

        if (ringSize < 1 || bufferSize < 1) {
            throw new IllegalArgumentException("positive args only");
        }

        if (Integer.bitCount(ringSize) != 1) {
            throw new IllegalArgumentException("ringSize must be a power of 2");
        }

        this.order = order;
        this.direct = direct;
        this.bufferSize = bufferSize;
        this.orderedRelease = orderedRelease;

        // Create ring buffer with "ringSize" # of elements,
        // each with ByteBuffers of size "bufferSize" bytes.
        ringBuffer = createSingleProducer(new ByteBufferFactory(), ringSize,
                                          //new PhasedBackoffWaitStrategy(500L, 1000L, TimeUnit.NANOSECONDS, new LiteBlockingWaitStrategy()));
                                          // new LiteBlockingWaitStrategy());
                                          //new SleepingWaitStrategy(10000, 200000));   // 200 usec
                                          new SpinCountBackoffWaitStrategy(10000, new LiteBlockingWaitStrategy()));
                                          //new BlockingWaitStrategy());
                                          //new YieldingWaitStrategy());

        // Barrier to keep unreleased buffers from being reused
        barrier  = ringBuffer.newBarrier();
        sequence = new Sequence(Sequencer.INITIAL_CURSOR_VALUE);
        ringBuffer.addGatingSequences(sequence);
        nextConsumerSequence = sequence.get() + 1;
    }


    /**
     * Constructor. Used when wanting to avoid locks for speed purposes. Say a ByteBufferItem
     * is used by several users. This is true in ET or emu input channels in which many evio
     * events all contain a reference to the same buffer. If the user can guarantee that all
     * the users of one buffer release it before any of the users of the next, then synchronization
     * is not necessary. If that isn't the case, then locks take care of preventing a later
     * acquired buffer from being released first and consequently everything that came before
     * it in the ring.<p>
     *
     * In addition, each ByteBufferItem in this supply will have an object associated with it
     * which is passed in through the last argument. Used to store an EvioNodePool with each
     * buffer in the supply.
     *
     * @param ringSize        number of ByteBufferItem objects in ring buffer.
     * @param bufferSize      initial size (bytes) of ByteBuffer in each ByteBufferItem object.
     * @param order           byte order of ByteBuffer in each ByteBufferItem object.
     * @param direct          if true, make ByteBuffers direct.
     * @param orderedRelease  if true, the user promises to release the ByteBufferItems
     *                        in the same order as acquired. This avoids using
     *                        synchronized code (no locks).
     * @param pools           one EvioNodePool for each ByteBuffer.
     * @throws IllegalArgumentException if args &lt; 1 or ringSize not power of 2,
     *                                  pools array must have not be null or have less than
     *                                  ringSize number of elements.
     */
    public ByteBufferSupply(int ringSize, int bufferSize, ByteOrder order,
                            boolean direct, boolean orderedRelease, EvioNodePool[] pools)
            throws IllegalArgumentException {

        if (ringSize < 1 || bufferSize < 1) {
            throw new IllegalArgumentException("positive args only");
        }

        if (Integer.bitCount(ringSize) != 1) {
            throw new IllegalArgumentException("ringSize must be a power of 2");
        }
        
        if (pools == null || pools.length < ringSize) {
            throw new IllegalArgumentException("pools array must contain at least ringSize # of elements");
        }

        this.order = order;
        this.direct = direct;
        this.bufferSize = bufferSize;
        this.orderedRelease = orderedRelease;

        // Create ring buffer with "ringSize" # of elements,
        // each with ByteBuffers of size "bufferSize" bytes.
        ringBuffer = createSingleProducer(new AuxByteBufferFactory(pools), ringSize,
                                          //new PhasedBackoffWaitStrategy(500L, 1000L, TimeUnit.NANOSECONDS, new LiteBlockingWaitStrategy()));
                                          // new LiteBlockingWaitStrategy());
                                          //new SleepingWaitStrategy(10000, 200000));   // 200 usec
                                          new SpinCountBackoffWaitStrategy(10000, new LiteBlockingWaitStrategy()));
                                          //new BlockingWaitStrategy());
                                          //new YieldingWaitStrategy());

        // Barrier to keep unreleased buffers from being reused
        barrier  = ringBuffer.newBarrier();
        sequence = new Sequence(Sequencer.INITIAL_CURSOR_VALUE);
        ringBuffer.addGatingSequences(sequence);
        nextConsumerSequence = sequence.get() + 1;
    }



    /**
     * Constructor. Used when wanting a source of ByteBuffers which already
     * contain data. Useful when testing.
     *
     * @param ringSize        number of ByteBufferItem objects in ring buffer.
     * @param bufList         list of ByteBuffers used to populate this supply.
     *                        List must contain ringSize number of buffers.
     * @param orderedRelease  if true, the user promises to release the ByteBufferItems
     *                        in the same order as acquired. This avoids using
     *                        synchronized code (no locks).
     * @throws IllegalArgumentException bad arg or ringSize not power of 2.
     */
    public ByteBufferSupply(int ringSize, List<ByteBuffer> bufList, boolean orderedRelease)
            throws IllegalArgumentException {

        if (ringSize < 1) {
            throw new IllegalArgumentException("positive args only");
        }

        if (Integer.bitCount(ringSize) != 1) {
            throw new IllegalArgumentException("ringSize must be a power of 2");
        }

        if (bufList == null || bufList.size() < ringSize) {
            throw new IllegalArgumentException("bufList is null or size < ringSize");
        }

        this.orderedRelease = orderedRelease;

        // Create ring buffer with "ringSize" # of elements taken from bufList.
        ringBuffer = createSingleProducer(new PredefinedByteBufferFactory(bufList), ringSize,
                                          //new PhasedBackoffWaitStrategy(500L, 1000L, TimeUnit.NANOSECONDS, new LiteBlockingWaitStrategy()));
                                          //new LiteBlockingWaitStrategy());
                                          //new BlockingWaitStrategy());
                                          //new SleepingWaitStrategy(10000, 200000));   // 200 usec
                                          new SpinCountBackoffWaitStrategy(10000, new LiteBlockingWaitStrategy()));
                                          //new YieldingWaitStrategy());

        // Barrier to keep unreleased buffers from being reused
        barrier  = ringBuffer.newBarrier();
        sequence = new Sequence(Sequencer.INITIAL_CURSOR_VALUE);
        ringBuffer.addGatingSequences(sequence);
        nextConsumerSequence = sequence.get() + 1;
    }


    /**
     * What percentage of the byte buffers are being used?
     * If this ring is full (of unused buffers), it corresponds to an empty input or output channel (0%).
     * In this case, (ringSize - (ringSize - (produced - consumed))) = (produced - consumed) = 0.
     * If it's empty then the channel is full (100%).
     * In this case, (produced - consumed) = ringSize.
     * @return fill level, 0% to 100%.
     */
    public int getFillLevel() {
        int ringCount = ringBuffer.getBufferSize();
        return (int) (100*(ringCount - ringBuffer.remainingCapacity())/ringCount);
        //return (int) (100*(ringBuffer.getCursor() - (ringBuffer.getMinimumGatingSequence()))/ringCount);
    }


    /**
     * Get the next available item in ring buffer for writing/reading data.
     * Not sure if this method is thread-safe.
     * 
     * @return next available item in ring buffer.
     * @throws InterruptedException if thread interrupted.
     */
    public ByteBufferItem get() throws InterruptedException {
        // Next available item claimed by data producer
        long getSequence = ringBuffer.nextIntr(1);

        // Get object in that position (sequence) of ring buffer
        ByteBufferItem bufItem = ringBuffer.get(getSequence);

        // Get item ready for use
        bufItem.reset();

        // Store sequence for later releasing of the buffer
        bufItem.setProducerSequence(getSequence);

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
    public ByteBufferItem getAsIs() throws InterruptedException {
        // Next available item claimed by data producer
        long getSequence = ringBuffer.nextIntr(1);

        // Get object in that position (sequence) of ring buffer
        ByteBufferItem bufItem = ringBuffer.get(getSequence);
        bufItem.setFromConsumerGet(false);

        // Store sequence for later releasing of the buffer
        bufItem.setProducerSequence(getSequence);

        return bufItem;
    }


    /**
     * Get the next available item in ring buffer for getting data already written into.
     * Not sure if this method is thread-safe.
     * @return next available item in ring buffer for getting data already written into.
     * @throws InterruptedException if thread interrupted.
     */
    public ByteBufferItem consumerGet() throws InterruptedException {

        ByteBufferItem item = null;

        try  {
            // Only wait for read-volatile-memory if necessary ...
            if (availableConsumerSequence < nextConsumerSequence) {
                availableConsumerSequence = barrier.waitFor(nextConsumerSequence);
            }

            item = ringBuffer.get(nextConsumerSequence);
            item.setConsumerSequence(nextConsumerSequence++);
            item.setFromConsumerGet(true);
        }
        catch (final com.lmax.disruptor.TimeoutException ex) {
            // never happen since we don't use timeout wait strategy
            ex.printStackTrace();
        }
        catch (final AlertException ex) {
            ex.printStackTrace();
        }

        return item;
    }

//    private boolean delta2 = false;
//    private boolean delta3 = false;

    /**
     * Consumer releases claim on the given ring buffer item so it becomes available for reuse.
     * This method <b>ensures</b> that sequences are released in order and is thread-safe.
     * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
     * @param item item in ring buffer to release for reuse.
     */
    public void release(ByteBufferItem item) {
        if (item == null) return;

        // Each item may be used by several objects/threads. It will
        // only be released for reuse if everyone releases their claim.

        long seq;
        boolean isConsumerGet = item.isFromConsumerGet();
        if (isConsumerGet) {
            seq = item.getConsumerSequence();
//System.out.println(" S" + seq + "P" + item.auxIndex);
        }
        else {
            seq = item.getProducerSequence();
//System.out.print(" P" + seq);
        }


        if (item.decrementCounter()) {
            if (orderedRelease) {
//System.out.println(" <" + maxSequence + ">" );
                sequence.set(seq);
                return;
            }

            synchronized (this) {
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

//                delta2 = seq == (lastSequenceReleased + 2);
//                delta3 = seq == (lastSequenceReleased + 3);

                // If we now have everything between last & max, release it all.
                // This way higher sequences are never released before lower.
                if ((maxSequence - lastSequenceReleased - 1L) == between) {
//System.out.println("\n**" + maxSequence + "**" );
                    sequence.set(maxSequence);
                    lastSequenceReleased = maxSequence;
                    between = 0;
//                    delta2 = delta3 = false;
                }
//                // If we have next one up from the last released, we can release it
//                else if (seq == lastSequenceReleased + 1) {
//                    if (delta2) {
//                        seq++;
//                        if (delta3) seq++;
//                    }
//                    sequence.set(seq);
//                    between--;
//                    lastSequenceReleased = seq;
//                    delta2 = false;
//                }
            }
        }

   }


//    /**
//      * Consumer releases claim on the given ring buffer item so it becomes available for reuse.
//      * This method <b>ensures</b> that sequences are released in order and is thread-safe.
//      * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
//      * @param item item in ring buffer to release for reuse.
//      */
//     public void release(ByteBufferItem item) {
//         if (item == null) return;
//
//         // Each item may be used by several objects/threads. It will
//         // only be released for reuse if everyone releases their claim.
//         if (item.decrementCounter()) {
//             // Sequence we want to release
//             long seq;
//             boolean isConsumerGet = item.isFromConsumerGet();
//
//             if (isConsumerGet) {
//                 seq = item.getConsumerSequence();
//             }
//             else {
//                 seq = item.getProducerSequence();
//             }
//
//             if (orderedRelease) {
// //System.out.println("    BBS: Unsync release " + seq);
//                 sequence.set(seq);
//                 return;
//             }
// //System.out.println("    BBS: release go into SYNC code");
//
//             synchronized (this) {
//                 // If we got a new max ...
//                 if (seq > maxSequence) {
//                     // If the old max was > the last released ...
//                     if (maxSequence > lastSequenceReleased) {
//                         // we now have a sequence between last released & new max
//                         between++;
//                     }
//
//                     // Set the new max
//                     maxSequence = seq;
//                 }
//                 // If we're < max and > last, then we're in between
//                 else if (seq > lastSequenceReleased) {
//                     between++;
//                 }
//
//                 // If we now have everything between last & max, release it all.
//                 // This way higher sequences are never released before lower.
//                 if ((maxSequence - lastSequenceReleased - 1L) == between) {
// //System.out.println("    BBS: Sync release " + seq);
//                     sequence.set(maxSequence);
//                     lastSequenceReleased = maxSequence;
//                     between = 0;
//                 }
//             }
//         }
//     }


//    /**
//     * Consumer releases claim on the given ring buffer item so it becomes available for reuse.
//     * This method <b>ensures</b> that sequences are released in order and is thread-safe.
//     * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
//     * @param item item in ring buffer to release for reuse.
//     */
//    void releaseOrig(ByteBufferItem item) {
//        if (item == null) return;
//
//        // Each item may be used by several objects/threads. It will
//        // only be released for reuse if everyone releases their claim.
//        if (item.decrementCounter()) {
//            if (orderedRelease) {
//                if (item.isFromConsumerGet()) {
////System.out.println(" S" + item.getConsumerSequence());
////System.out.println("    BBS: Ord release " + item.getConsumerSequence());
//                    sequence.set(item.getConsumerSequence());
//                }
//                else {
////System.out.println(" S" + item.getProducerSequence());
////System.out.println("    BBS: Ord release " + item.getProducerSequence());
//                    sequence.set(item.getProducerSequence());
//                }
//                return;
//            }
////System.out.println("    BBS: release go into SYNC code");
//
//            synchronized (this) {
//                // Sequence we want to release
//                long seq;
//                if (item.isFromConsumerGet()) {
//                    seq = item.getConsumerSequence();                }
//                else {
//                    seq = item.getProducerSequence();
//                }
//
//                // If we got a new max ...
//                if (seq > maxSequence) {
//                    // If the old max was > the last released ...
//                    if (maxSequence > lastSequenceReleased) {
//                        // we now have a sequence between last released & new max
//                        between++;
//                    }
//
//                    // Set the new max
//                    maxSequence = seq;
//                }
//                // If we're < max and > last, then we're in between
//                else if (seq > lastSequenceReleased) {
//                    between++;
//                }
//
//                // If we now have everything between last & max, release it all.
//                // This way higher sequences are never released before lower.
//                if ((maxSequence - lastSequenceReleased - 1L) == between) {
//                    sequence.set(maxSequence);
//                    lastSequenceReleased = maxSequence;
//                    between = 0;
//                }
//            }
//        }
//    }



    /**
     * Used to tell that the consumer that the ring buffer item is ready for consumption.
     * Not sure if this method is thread-safe.
     * To be used in conjunction with {@link #get()} and {@link #consumerGet()}.
     * @param byteBufferItem item available for consumer's use.
     */
    public void publish(ByteBufferItem byteBufferItem) {
        if (byteBufferItem == null) return;
        ringBuffer.publish(byteBufferItem.getProducerSequence());
    }

}
