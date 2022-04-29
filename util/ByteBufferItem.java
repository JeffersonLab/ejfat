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

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * This class is used in conjunction with the {@link ByteBufferSupply} class
 * to provide a very fast supply of ByteBuffer objects for reuse. Objects of
 * this class are used to populate the ring buffer in ByteBufferSupply.
 * Uses the Disruptor software package.
 * @author timmer (4/14/14)
 */
public class ByteBufferItem {

    /** Size of ByteBuffer in bytes. */
    private int bufferSize;

    /** ByteBuffer object. */
    private ByteBuffer buffer;

    /** Byte order of buffer. */
    private final ByteOrder order;

    /** Is this byte buffer direct? */
    private final boolean direct;

    /** True if user releases ByteBufferItems in same order as acquired. */
    private final boolean orderedRelease;

    /** Sequence in which this object was taken from ring for use by a producer with get(). */
    private long producerSequence;

    /** Sequence in which this object was taken from ring for use by a consumer with consumerGet(). */
    private long consumerSequence;

    /** Track more than one user so this object can be released for reuse. */
    private AtomicInteger atomicCounter;

    /** Track more than one user so this object can be released for reuse. */
    private volatile int volatileCounter;

    /** If true, we're tracking more than one user. */
    private boolean multipleUsers;

    /**
     * If true, and this item comes from a supply used in the sense of
     * single-producer-single-consumer, then this flag can relay to the
     * consumer the need to force any write.
     */
    private boolean force;

    /**
     * Need to track whether this item was obtained through consumerGet() or
     * through either get() / getAsIs() since they must be released differently.
     */
    private boolean fromConsumerGet;

    /** Extra integer for user's convenience.
     *  Gets reset to 0 each time supply.get() is called. */
    private int userInt;

    /** Extra boolean for user's convenience.
     *  Gets reset to false each time supply.get() is called. */
    private boolean userBoolean;

    /** Auxiliary object storage. Used to store EvioNodePool. */
    private Object myObject;

    /** Auxiliary integer storage. Used to store pool ID. */
    public int myIndex;

    // For testing purposes

    /** Unique id for each object of this class. */
    private final int myId;



    /**
     * Constructor.
     *
     * @param bufferSize size in bytes of ByteBuffer to construct.
     * @param order byte order of ByteBuffer to construct.
     * @param direct is the buffer direct (in memory not managed by JVM) or not.
     * @param orderedRelease if true, release ByteBufferItems in same order as acquired.
     * @param myId unique id of this object.
     */
    ByteBufferItem(int bufferSize, ByteOrder order,
                          boolean direct, boolean orderedRelease, int myId) {
        this(bufferSize, order, direct, orderedRelease, myId, null, 0);
    }


    /**
     * Constructor.
     *
     * @param bufferSize size in bytes of ByteBuffer to construct.
     * @param order byte order of ByteBuffer to construct.
     * @param direct is the buffer direct (in memory not managed by JVM) or not.
     * @param orderedRelease if true, release ByteBufferItems in same order as acquired.
     * @param myId      unique id of this object.
     * @param auxObject auxiliary object to store.
     * @param auxIndex  auxiliary integer to store.
     */
    ByteBufferItem(int bufferSize, ByteOrder order,
                   boolean direct, boolean orderedRelease, int myId,
                   Object auxObject, int auxIndex) {

        this.myId = myId;
        this.order = order;
        this.direct = direct;
        this.myIndex = auxIndex;
        this.myObject = auxObject;
        this.bufferSize = bufferSize;
        this.orderedRelease = orderedRelease;

        if (direct) {
            buffer = ByteBuffer.allocateDirect(bufferSize).order(order);
        }
        else {
            buffer = ByteBuffer.allocate(bufferSize).order(order);
        }
    }


    /**
     * Constructor used to initially fill each ByteBufferItem of a ByteBufferSupply
     * with a copy of a template buffer.
     *
     * @param templateBuf    this item's buffer is a copy this of template ByteBuffer.
     * @param orderedRelease if true, release ByteBufferItems in same order as acquired.
     * @param myId unique id of this object.
     */
    ByteBufferItem(ByteBuffer templateBuf, boolean orderedRelease, int myId) {

        this.myId = myId;
        this.order = templateBuf.order();
        this.direct = templateBuf.isDirect();
        this.bufferSize = templateBuf.capacity();
        this.orderedRelease = orderedRelease;

        if (direct) {
            buffer = ByteBuffer.allocateDirect(bufferSize).order(order);
            for (int i=0; i < bufferSize; i++) {
                buffer.put(i, templateBuf.get(i));
            }
        }
        else {
            buffer = ByteBuffer.allocate(bufferSize).order(order);
            System.arraycopy(templateBuf.array(), 0, buffer.array(), 0, bufferSize);
        }
        buffer.position(templateBuf.position());
        buffer.limit(templateBuf.limit());
    }


    /**
     * Method to reset this item each time it is retrieved from the supply.
     */
    public void reset() {
        buffer.clear();
        userInt = 0;
        force = false;
        userBoolean = false;
        multipleUsers = false;
        fromConsumerGet = false;
        producerSequence = consumerSequence = 0L;
    }

    public boolean equals(ByteBuffer buf) {
        return buffer == buf;
    }

    /**
     * Get the unique id of this object.
     * @return unique id of this object.
     */
    public int getMyId() {return myId;}

    /**
     * Get the auxiliary object.
     * @return auxiliary object.
     */
    public Object getMyObject() {return myObject;}

    /**
     * Set the auxiliary object.
     * @param obj auxiliary object.
     */
    public void setMyObject(Object obj) {myObject = obj;}

    /**
     * Is this a direct buffer or not?
     * @return {@code true} if direct buffer, else {@code false}.
     */
    public boolean isDirect() {return direct;}

    /**
     * Get the flag used to suggest a forced write to a consumer.
     * @return flag used to suggest a forced write to a consumer.
     */
    public boolean getForce() {
        return force;
    }

    /**
     * Set the flag used to suggest a forced write to a consumer.
     * @param force flag used to suggest a forced write to a consumer.
     */
    public void setForce(boolean force) {
        this.force = force;
    }

    /**
     * Was this item obtained through a call to consumerGet()?
     * @return {@code true} only if item obtained through a call to consumerGet().
     */
    public boolean isFromConsumerGet() {
        return fromConsumerGet;
    }

    /**
     * Set whether this item was obtained through a call to consumerGet().
     * @param fromConsumerGet {@code true} only if item obtained through
     *                        a call to consumerGet().
     */
    public void setFromConsumerGet(boolean fromConsumerGet) {
        this.fromConsumerGet = fromConsumerGet;
    }

    /**
     * Get the user integer.
     * User int gets reset to 0 each time supply.get() is called.
     * @return user integer.
     */
    public int getUserInt() {return userInt;}

    /**
     * Set the user integer.
     * @param i user integer.
     */
    public void setUserInt(int i) {userInt = i;}

    /**
     * Get the user boolean.
     * User boolean gets reset to false each time supply.get() is called.
     * @return user boolean.
     */
    public boolean getUserBoolean() {return userBoolean;}

    /**
     * Set user boolean.
     * @param bool user boolean.
     */
    public void setUserBoolean(boolean bool) {userBoolean = bool;}

    //--------------------------------

    /**
     * Get the sequence of this item for producer.
     * @return sequence of this item for producer.
     */
    public long getProducerSequence() {return producerSequence;}


    /**
     * Set the sequence of this item for producer.
     * @param sequence sequence of this item for producer.
     */
    public void setProducerSequence(long sequence) {this.producerSequence = sequence;}


    /**
     * Get the sequence of this item for consumer.
     * @return sequence of this item for consumer.
     */
    public long getConsumerSequence() {return consumerSequence;}


    /**
     * Set the sequence of this item for consumer.
     * @param sequence sequence of this item for consumer.
     */
    public void setConsumerSequence(long sequence) {this.consumerSequence = sequence;}


    /**
     * Get the size in bytes of the contained ByteBuffer.
     * @return size in bytes of the contained ByteBuffer.
     */
    public int getBufferSize() {return bufferSize;}


    /**
     * Set the contained ByteBuffer.
     * This method is dangerous -- definitely not thread safe!
     * @param buf contained ByteBuffer.
     */
    public void setBuffer(ByteBuffer buf) {
        bufferSize = buf.capacity();
        buffer = buf;
    }

    
    /**
     * Get the contained ByteBuffer.
     * Position is set to 0.
     * @return contained ByteBuffer.
     */
    public ByteBuffer getBuffer() {
        buffer.position(0);
        return buffer;
    }


    /**
     * Get the contained ByteBuffer without any modifications.
     * @return contained ByteBuffer without any modifications.
     */
    public ByteBuffer getBufferAsIs() {
        return buffer;
    }


    /**
     * Make sure the buffer is the size needed.
     * @param capacity minimum necessary size of buffer in bytes.
     */
    public void ensureCapacity(int capacity) {
        if (bufferSize < capacity) {
            if (direct) {
                buffer = ByteBuffer.allocateDirect(capacity).order(order);
            }
            else {
                buffer = ByteBuffer.allocate(capacity).order(order);
            }
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
    public void setUsers(int users) {
        if (users > 1) {
            multipleUsers = true;

            if (orderedRelease) {
                volatileCounter = users;
            }
            else {
                atomicCounter = new AtomicInteger(users);
            }
        }
    }


    /**
     * Get the number of users of this buffer.
     * @return number of users of this buffer.
     */
    public int getUsers() {
        if (multipleUsers) {
            if (orderedRelease) {
                return volatileCounter;
            }
            else {
                return atomicCounter.get();
            }
        }
        return 1;
    }


    /**
     * Called by buffer user by way of {@link ByteBufferSupply#release(ByteBufferItem)}
     * if no longer using it so it may be reused later.
     * @return {@code true} if no one using buffer now, else {@code false}.
     */
    boolean decrementCounter() {
        if (!multipleUsers) return true;
        if (orderedRelease) return (--volatileCounter < 1);
        return (atomicCounter.decrementAndGet() < 1);
    }


    /**
     * If a reference to this ByteBufferItem is copied, then it is necessary to increase
     * the number of users. Although this method is not safe to call in general,
     * it is safe, for example, if a RingItem is copied in the ER <b>BEFORE</b>
     * it is copied again onto multiple output channels' rings and then released.
     * Currently this is only used in just such a situation - in the ER when a ring
     * item must be copied and placed on all extra output channels. In this case,
     * there is always at least one existing user.
     *
     * @param additionalUsers number of users to add
     */
    public void addUsers(int additionalUsers) {
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
                atomicCounter = new AtomicInteger(additionalUsers + 1);
            }
        }
        else {
            if (orderedRelease) {
                // Warning, this is not an atomic operation!
                volatileCounter += additionalUsers;
            }
            else {
                atomicCounter.addAndGet(additionalUsers);
            }
        }
    }

}
