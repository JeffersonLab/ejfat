//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "ByteBuffer.h"


namespace ejfat {

    // Define a deleter that does not delete memory for a shared pointer that's
    // used with shared memory.
    void null_deleter(uint8_t *) {};


    /** Default constructor, size of 4096 bytes.  */
    ByteBuffer::ByteBuffer() : ByteBuffer(4096) {}


    /**
     * Constructor. Reserves specified size in internal array.
     * @param size size (in bytes) of space to preallocate internally.
     */
    ByteBuffer::ByteBuffer(size_t size) {

        buf = std::shared_ptr<uint8_t>(new uint8_t[size], std::default_delete<uint8_t[]>());
        totalSize = cap = size;
        clear();

        isLittleEndian = byteOrder.isLittleEndian();
        isHostEndian = true;
    }


    /**
     * Copy constructor. Not available in Java, but useful in C++.
     * @param srcBuf ByteBuffer to copy.
     */
    ByteBuffer::ByteBuffer(const ByteBuffer & srcBuf) {
        copy(srcBuf);
    }


    /**
     * Move constructor.
     * @param srcBuf ByteBuffer to move.
     */
    ByteBuffer::ByteBuffer(ByteBuffer && srcBuf) noexcept {
        if (this != &srcBuf) {
            *this = std::move(srcBuf);
        }
    }


    /**
     * This constructor is equivalent to the ByteBuffer.wrap() method in Java.<p>
     *
     * The byteArray arg needs to be created as a result to calling "new" as it's
     * made part of a shared pointer!
     * There is also some risk here if caller continues to use byteArray pointer
     * independently of this object. <b>Don't do it!</b>
     *
     * @param byteArray pointer to array which this object will wrap.
     * @param len length of array in bytes.
     * @param isMappedMem is the byteArray arg a pointer obtained through mmap (file memory mapping)?.
     */
    ByteBuffer::ByteBuffer(char* byteArray, size_t len, bool isMappedMem) :
                    ByteBuffer(reinterpret_cast<uint8_t*>(byteArray), len, isMappedMem) {
    }


    /**
     * This constructor is equivalent to the ByteBuffer.wrap() method in Java.<p>
     *
     * The byteArray arg needs to be created as a result to calling "new" as it's
     * made part of a shared pointer!
     * There is also some risk here if caller continues to use byteArray pointer
     * independently of this object. <b>Don't do it!</b>
     *
     * @param byteArray pointer to array which this object will wrap.
     * @param len length of array in bytes.
     * @param isMappedMem is the byteArray arg a pointer obtained through mmap (file memory mapping)?.
     */
    ByteBuffer::ByteBuffer(uint8_t* byteArray, size_t len, bool isMappedMem) {

        if (isMappedMem) {
            // Must remove the deleter since we're using shared memory and it must be unmapped, not deleted
            buf = std::shared_ptr<uint8_t>(byteArray,  &null_deleter);
        }
        else {
            buf = std::shared_ptr<uint8_t>(byteArray, std::default_delete<uint8_t[]>());
        }
        totalSize = cap = len;
        clear();

        isLittleEndian = byteOrder.isLittleEndian();
        isHostEndian = true;
        isMappedMemory = isMappedMem;
    }


    /** Destructor. Be sure to unmap any memory mapped file. */
    ByteBuffer::~ByteBuffer() {
        if (isMappedMemory) {
            // unmap any memory map
            ::munmap(buf.get(), cap);
        }
    }


    /**
     * Move assignment operator.
     * @param other right side object.
     * @return left side object.
     */
    ByteBuffer & ByteBuffer::operator=(ByteBuffer && other) noexcept {

        // Avoid self assignment ...
        if (this != &other) {
            pos = other.pos;
            cap = other.cap;
            lim = other.lim;
            mrk = other.mrk;
            off = other.off;
            totalSize = other.totalSize;
            byteOrder = other.byteOrder;
            isHostEndian = other.isHostEndian;
            isLittleEndian = other.isLittleEndian;

            // Data is in shared pointer. The following is cheaper than doing
            // buf = other.buf which works but requires atomic counter inc/decrement.
            // This also resets other.buf to nullptr which is the correct behavior since
            // this operation is probably only going to be used internal to this class
            // in the context of creating a temporary ByteBuffer in a method which gets
            // "passed" outside its scope.
            // If this turns out to not work, can always do: buf = other.buf;
            buf = std::move(other.buf);
        }
        return *this;
    }


    /**
     * Assignment operator.
     * @param other right side object.
     * @return left side object.
     */
    ByteBuffer & ByteBuffer::operator=(const ByteBuffer& other) {

        // Avoid self assignment ...
        if (this != &other) {
            pos = other.pos;
            cap = other.cap;
            lim = other.lim;
            mrk = other.mrk;
            off = other.off;
            totalSize = other.totalSize;
            byteOrder = other.byteOrder;
            isHostEndian = other.isHostEndian;
            isLittleEndian = other.isLittleEndian;

            // Share data with rhs object
            buf = other.buf;
        }
        return *this;
    }


    /**
     * Equality operator.
     * Way of checking if the 2 buffers are pointing to the same data.
     * @param rhs right side object.
     * @return true if equal.
     */
    bool ByteBuffer::operator== (const ByteBuffer & rhs) noexcept {
        return (buf == rhs.buf);
    }


    /**
     * Inequality operator.
     * Way of checking if the 2 buffers are not pointing to the same data.
     * @param rhs right side object.
     * @return false if equal.
     */
    bool ByteBuffer::operator!= (const ByteBuffer & rhs) noexcept {
        return (buf != rhs.buf);
    }


    /**
     * Subscript operator for absolute access to data bytes without limit check.
     * This has no Java counterpart.
     * @param index index into internal data vector.
     * @return reference to internal byte in data vector at the given index.
     */
    uint8_t & ByteBuffer::operator[] (size_t index) {
        return (buf.get())[index + off];
    }


    /**
     * Subscript operator for absolute access to data bytes of const ByteBuffers without limit check.
     * This has no Java counterpart.
     * @param index index into internal data vector.
     * @return reference to internal byte in data vector at the given index.
     */
    uint8_t ByteBuffer::operator[] (size_t index) const {
        return (buf.get())[index + off];
    }


    /**
     * Copy data and everything else from arg.
     * @param srcBuf ByteBuffer to copy.
     */
    void ByteBuffer::copy(const ByteBuffer & srcBuf) {

        // Avoid self copy ...
        if (this != &srcBuf) {
            // A copy should not use the same shared pointer, copy data over
            buf = std::shared_ptr<uint8_t>(new uint8_t[srcBuf.totalSize], std::default_delete<uint8_t[]>());

            pos = srcBuf.pos;
            cap = srcBuf.cap;
            lim = srcBuf.lim;
            mrk = srcBuf.mrk;
            off = srcBuf.off;
            totalSize = srcBuf.totalSize;
            byteOrder = srcBuf.byteOrder;
            isHostEndian = srcBuf.isHostEndian;
            isLittleEndian = srcBuf.isLittleEndian;
            memcpy((void *)(buf.get()), (const void *)(srcBuf.buf.get()), totalSize);
        }
    }


    /**
     * Copy data and everything else from arg.
     * @param srcBuf ByteBuffer to copy.
     */
    void ByteBuffer::copy(const std::shared_ptr<const ByteBuffer> & srcBuf) {
        return copy(*(srcBuf.get()));
    }


    /**
     * Copy the given buffer into a new buffer which is accessed thru shared pointer.
     * @param srcBuf ByteBuffer to copy.
     * @return new ByteBuffer.
     */
    std::shared_ptr<ByteBuffer> ByteBuffer::copyBuffer(const std::shared_ptr<const ByteBuffer> & srcBuf) {

        auto newBuf = std::make_shared<ByteBuffer>(srcBuf->totalSize);
        newBuf->pos = srcBuf->pos;
        newBuf->cap = srcBuf->cap;
        newBuf->lim = srcBuf->lim;
        newBuf->mrk = srcBuf->mrk;
        newBuf->off = srcBuf->off;
        newBuf->totalSize = srcBuf->totalSize;
        newBuf->byteOrder = srcBuf->byteOrder;
        newBuf->isHostEndian = srcBuf->isHostEndian;
        newBuf->isLittleEndian = srcBuf->isLittleEndian;
        memcpy((void *)(newBuf->buf.get()), (const void *)(srcBuf->buf.get()), srcBuf->totalSize);
        return newBuf;
    }


    /**
     * Copy the source buffer's data (position to limit) into this buffer starting at local position 0.
     * Limit and capacity are set to the "limit - position" bytes of srcBuf.
     * This can reallocate memory if more is needed and can therefore resize this buffer.
     * Existing data in this buffer will be overwritten.
     * @param srcBuf ByteBuffer to copy data from.
     * @param position position in srcByf to start from.
     * @param limit position in srcByf to end at.
     */
    void ByteBuffer::copyData(const std::shared_ptr<const ByteBuffer> & srcBuf, size_t position, size_t limit) {
        // How many bytes do we copy?
        size_t newSize = limit - position;

        if (newSize < 1) return;
        if (newSize > cap) {
//std::cout << "copyData:  REALLOCATING MEM!!!\n";
            buf = std::shared_ptr<uint8_t>(new uint8_t[newSize], std::default_delete<uint8_t[]>());
            cap = newSize;
            totalSize = newSize;
        }

        pos = 0;
        lim = newSize;
        mrk = -1;
        off = 0;
        byteOrder = srcBuf->byteOrder;
        isHostEndian = srcBuf->isHostEndian;
        isLittleEndian = srcBuf->isLittleEndian;

        memcpy((void *)(buf.get()),
               (const void *)(srcBuf->buf.get() + srcBuf->arrayOffset() + position),
                newSize);
    }


    /**
     * This method writes zeroes into the buffer memory (from pos = 0 to capacity).
     * If this ByteBuffer was obtained through calling {@link #slice}, the underlying
     * data array may not be fully zeroed as position = 0 may occur at a non-zero offset
     * and the capacity may be less than the array's total size.
     * Although this method does not exist in the original Java ByteBuffer class,
     * in Java, all objects have their memory zeroed so this may be useful.
     *
     * @return reference to this ByteBuffer.
     */
    ByteBuffer & ByteBuffer::zero() {
        std::memset(buf.get() + off, 0, cap);
        return *this;
    }


   /**
    * This method compacts this buffer.
    * The bytes between the buffer's position and its limit, if any, are copied
    * to the beginning of the buffer. That is, the byte at index p = position() is copied
    * to index zero, the byte at index p + 1 is copied to index one, and so forth until
    * the byte at index limit() - 1 is copied to index n = limit() - 1 - p. The buffer's
    * position is then set to n+1 and its limit is set to its capacity. The mark, if defined,
    * is discarded.
    * The buffer's position is set to the number of bytes copied, rather than to zero,
    * so that an invocation of this method can be followed immediately by an invocation
    * of another relative put method.
    *
    * @return reference to this ByteBuffer.
    */
    ByteBuffer & ByteBuffer::compact() {
        size_t bytesRemaining = remaining();

        // If no data has to be moved
        if (pos == 0) {
            pos = bytesRemaining;
            lim = cap;
            mrk = -1;
            return *this;
        }

        uint8_t *bf = buf.get() + off;

        // Can a bulk copy be done? In other words, does the data to be transferred
        // fit into the space before the original position?
        if (bytesRemaining <= pos) {
            std::memcpy((void *)(bf), (const void *)(bf+pos), bytesRemaining);
        }
        else {
            // Unfortunately we gotta copy this byte-by-byte cause of the overlap.
            // However, the last pos number of bytes can be done in bulk.
            size_t firstChunk = bytesRemaining - pos;
            for (size_t i=0; i < firstChunk; i++) {
                bf[i] = bf[i+pos];
            }
            // second chunk = pos
            std::memcpy((void *)(bf+firstChunk), (const void *)(bf+firstChunk+pos), pos);
        }

        pos = bytesRemaining;
        lim = cap;
        mrk = -1;

        return *this;
    }


    /**
     * This method expands the size of this buffer if it's less than the given size.
     * Note that all ByteBuffers that currently share memory with this object, either through
     * being assigned, by calling {@link #duplicate}, or by calling {@link #slice},
     * will no longer share memory. New memory is allocated.
     * Useful in C++, but has no counterpart in Java.
     * Bytes up to limit are copied. New memory past limit is not zeroed.
     * All other internals are kept the same, including all data within the limit.
     * @param size new size (in bytes) of space to allocate internally.
     */
    void ByteBuffer::expand(size_t size) {
        if (size <= cap) return;

        // If there's data copy it over
        if (lim > 0) {
            auto tempBuf = std::shared_ptr<uint8_t>(new uint8_t[size], std::default_delete<uint8_t[]>());
            std::memcpy((void *)(tempBuf.get()), (const void *)(buf.get() + off), lim);
            buf = tempBuf;
        }
        else {
            buf = std::shared_ptr<uint8_t>(new uint8_t[size], std::default_delete<uint8_t[]>());
        }
        totalSize = cap = size;
        off = 0;
    }


    /**
     * This method is identical to the {@link #expand(size_t)} method with the exception
     * that it copies over all the original capacity of data. This is sometimes necessary
     * as data operations can be performed directly in the array and the limit is not maintained.
     * @param size new size (in bytes) of space to allocate internally.
     */
    void ByteBuffer::expandAndCopyAll(size_t size) {
        if (size <= cap) return;

        auto tempBuf = std::shared_ptr<uint8_t>(new uint8_t[size], std::default_delete<uint8_t[]>());
        std::memcpy((void *)(tempBuf.get()), (const void *)(buf.get() + off), cap);
        buf = tempBuf;
        totalSize = cap = size;
        off = 0;
    }



    /**
     * This method tests for data equivilancy. Two byte buffers are equal if, and only if,
     * they have the same number of remaining elements, and the two sequences of remaining
     * elements, considered independently of their starting positions, are pointwise equal.
     *
     * @param other A reference to a ByteBuffer to compare to this one
     * @return True if the internal buffers match, otherwise False.
     */
    bool ByteBuffer::equals(const ByteBuffer & other) {
        // If the remaining bytes aren't equal, they ain't equal
        size_t remainingBytes = remaining();
        if (other.remaining() != remainingBytes) {
            return false;
        }

        // Compare byte by byte
        uint8_t *bf = buf.get() + off;
        uint8_t *obf = other.buf.get() + other.off;
        for (size_t i = 0; i < remainingBytes; i++) {
            if (bf[pos + i] != obf[other.pos + i]) {
                return false;
            }
        }

        return true;
    }


    /**
     * This method has no relevance to this C++ library.
     * This always returns false since it is backed by an array.
     * @return false.
     */
    bool ByteBuffer::isDirect() const {return false;}


    /**
     * Tells whether or not this buffer is backed by an array.
     * This always returns true since it is backed by an array.
     * @return true.
     */
    bool ByteBuffer::hasArray() const {return true;}


    /**
     * Returns whether there are bytes remaining between the position and limit,
     * i.e. {@link #position()} &lt; {@link #limit()}.
     * @return position < limit.
     */
    bool ByteBuffer::hasRemaining() const {return pos < lim;}


    /**
     * Returns whether this buffer is read only. Always returns false;
     * @return false.
     */
    bool ByteBuffer::isReadOnly() const {return false;}


    /**
     * Get the byte order of the data.
     * @return byte order of the data.
     */
    const ByteOrder & ByteBuffer::order() const {return byteOrder;}


    /**
     * Get a pointer to this buffer's backing array which contains the data.
     * Note, there may be a non-zero offset which can be found by calling {@link #arrayOffset}.
     * Do not free or delete this array!
     * @return pointer to the data array.
     */
    uint8_t * ByteBuffer::array() const {return buf.get();}


    /**
     * Get a shared pointer to this buffer's backing array which contains the data.
     * Note, there may be a non-zero offset which can be found by calling {@link #arrayOffset}.
     * @return shared pointer to the data array.
     */
    std::shared_ptr<uint8_t> ByteBuffer::getData() const {return buf;}


    /**
     * Get the offset within this buffer's backing array of the first element of the buffer.
     * @return offset within this buffer's backing array of the first element of the buffer.
     */
    size_t ByteBuffer::arrayOffset() const {return off;}


    /**
     * Returns the number of bytes from the current position to the end of the data.
     * @return Number of bytes from position({@link #position()}) to the end ({@link #limit()}).
     */
    size_t ByteBuffer::remaining() const {return lim - pos;}


    /**
     * Returns the total available bytes in this buffer.
     * @return the total available bytes in this buffer.
     */
    size_t ByteBuffer::capacity() const {return cap;}


    /**
     * Returns the limit, the position of the last valid data byte.
     * @return the position of the last valid data byte.
     */
    size_t ByteBuffer::limit() const {return lim;}


    /**
     * Returns the position of the buffer.
     * @return the position of the buffer.
     */
    size_t ByteBuffer::position() const {return pos;}


    /**
     * Sets the buffer's mark at its position.
     * @return this buffer.
     */
    ByteBuffer & ByteBuffer::mark() {
        mrk = pos;
        return *this;
    }


    /**
     * Clears this buffer. The position is set to 0, the limit is set to the capacity,
     * and the mark is discarded. Data remains untouched.
     * @return this buffer.
     */
    ByteBuffer & ByteBuffer::clear() {
        pos = 0;
        lim = cap;
        mrk = -1;
        return *this;
    }


    /**
     * Flips this buffer. The limit is set to the current position and then the
     * position is set to 0. The mark is discarded. Gets buffer ready for reading.
     * @return this buffer.
     */
    ByteBuffer & ByteBuffer::flip() {
        lim = pos;
        pos = 0;
        mrk = -1;
        return *this;
    }


    /**
     * Resets this buffer's position to the previously-marked position.
     * @return this buffer.
     * @throws std::runtime_error if mark &lt; 0.
     */
    ByteBuffer & ByteBuffer::reset() {
        ssize_t m = mrk;
        if (m < 0) {
            throw std::runtime_error("invalid mark");
        }
        pos = m;
        return *this;
    }


    /**
     * Rewinds this buffer. The position is set to 0 and the mark is discarded.
     * @return this buffer.
     */
    ByteBuffer & ByteBuffer::rewind() {
        pos = 0;
        mrk = -1;
        return *this;
    }


    /**
     * Sets the position of the buffer.
     * @param p position of the buffer.
     * @return this buffer.
     * @throws std::runtime_error if p &gt; capacity.
     */
    ByteBuffer & ByteBuffer::position(size_t p) {
        if (p > lim) {
            throw std::runtime_error("buffer pos of " + std::to_string(p) + " will exceed limit of " + std::to_string(lim));
        }
        pos = p;
        if (mrk > pos) {
            mrk = -1;
        }
        return *this;
    };


    /**
     * Sets the limit of the buffer.
     * @param l limit of the buffer.
     * @return this buffer.
     * @throws std::runtime_error if l &gt; capacity.
     */
    ByteBuffer & ByteBuffer::limit(size_t l) {
        if (l > cap) {
            throw std::runtime_error("buffer lim of " + std::to_string(l) + " will exceed cap of " + std::to_string(cap));
        }
        lim = l;

        if (pos > lim) {
            pos = lim;
        }

        if (mrk > lim) {
            mrk = -1;
        }
        return *this;
    };


    /**
     * Sets the byte order of the data. It does <b>NOT</b> do any byte swapping.
     * @param order actual byte order of the data.
     * @return this buffer.
     */
    ByteBuffer & ByteBuffer::order(ByteOrder const & order) {
        byteOrder = order;
        isHostEndian = (order == ByteOrder::ENDIAN_LOCAL);
        isLittleEndian = (order == ByteOrder::ENDIAN_LITTLE);
        return *this;
    }


    /**
     * Returns a byte buffer that shares this buffer's content.
     *
     * <p> The content of the returned buffer will be that of this buffer.  Changes
     * to this buffer's content will be visible in the returned buffer, and vice
     * versa; the two buffers' position, limit, and mark values will be
     * independent.
     *
     * <p> The returned buffer's capacity, limit, position, and mark values will
     * initially be identical to those of this buffer.  </p>
     *
     * <p> The C++ version of this method departs from the Java which has no argument.
     * To implement this method, one could return a unique pointer to a locally created
     * ByteBuffer object which would eliminate the need for an argument. However, handling
     * the new buffer would be different from handling those created by the constructor
     * since the caller would have a unique pointer instead of a ByteBuffer reference.
     *
     * <p> A cleaner way to do this is for the caller to create their own, new ByteBuffer
     * object and pass it in as an argument. This method then makes it identical to this
     * buffer and the data (implemented as a shared pointer) is shared between
     * the objects. The buffer passed in as an argument is also the one returned.
     *
     * @param destBuf byte buffer to be made a duplicate of this one.
     * @return  the same byte buffer as passed in as the argument.
     */
    ByteBuffer & ByteBuffer::duplicate(ByteBuffer & destBuf) {
        destBuf.cap = cap;
        destBuf.lim = lim;
        destBuf.pos = pos;
        destBuf.mrk = mrk;
        destBuf.off = off;

        destBuf.totalSize = totalSize;
        destBuf.byteOrder = byteOrder;
        destBuf.isHostEndian = isHostEndian;
        destBuf.isLittleEndian = isLittleEndian;

        // shared pointers
        destBuf.buf = buf;
        return destBuf;
    }


    /**
     * Returns a byte buffer that shares this buffer's content.
     *
     * <p> The content of the returned buffer will be that of this buffer.  Changes
     * to this buffer's content will be visible in the returned buffer, and vice
     * versa; the two buffers' position, limit, and mark values will be
     * independent.
     *
     * <p> The returned buffer's capacity, limit, position, and mark values will
     * initially be identical to those of this buffer.  </p>
     *
     * <p> The C++ version of this method departs from the Java which has no argument.
     * To implement this method, one could return a unique pointer to a locally created
     * ByteBuffer object which would eliminate the need for an argument. However, handling
     * the new buffer would be different from handling those created by the constructor
     * since the caller would have a unique pointer instead of a ByteBuffer reference.
     *
     * <p> A cleaner way to do this is for the caller to create their own, new ByteBuffer
     * object and pass it in as an argument. This method then makes it identical to this
     * buffer and the data (implemented as a shared pointer) is shared between
     * the objects. The buffer passed in as an argument is also the one returned.
     *
     * @param destBuf byte buffer to be made a duplicate of this one.
     * @return  the same byte buffer as passed in as the argument.
     */
    std::shared_ptr<ByteBuffer> & ByteBuffer::duplicate(std::shared_ptr<ByteBuffer> & destBuf) {
        auto & buff = *(destBuf.get());
        duplicate(buff);
        return destBuf;
    }


    /**
     * Returns a byte buffer that shares this buffer's content.
     *
     * <p> The content of the returned buffer will be that of this buffer.  Changes
     * to this buffer's content will be visible in the returned buffer, and vice
     * versa; the two buffers' position, limit, and mark values will be
     * independent.
     *
     * <p> The returned buffer's capacity, limit, position, and mark values will
     * initially be identical to those of this buffer.  </p>
     *
     * <p> The C++ version of this method departs from the Java which has no argument.
     * To implement this method, one could return a unique pointer to a locally created
     * ByteBuffer object which would eliminate the need for an argument. However, handling
     * the new buffer would be different from handling those created by the constructor
     * since the caller would have a unique pointer instead of a ByteBuffer reference.
     *
     * <p> A cleaner way to do this is for the caller to create their own, new ByteBuffer
     * object and pass it in as an argument. This method then makes it identical to this
     * buffer and the data (implemented as a shared pointer) is shared between
     * the objects. The buffer passed in as an argument is also the one returned.
     *
     * @param destBuf byte buffer to be made a duplicate of this one.
     * @return  the same byte buffer as passed in as the argument.
     */
    std::shared_ptr<ByteBuffer> ByteBuffer::duplicate() {
        auto destBuf = std::make_shared<ByteBuffer>(cap);
        destBuf->lim = lim;
        destBuf->pos = pos;
        destBuf->mrk = mrk;
        destBuf->off = off;

        destBuf->totalSize = totalSize;
        destBuf->byteOrder = byteOrder;
        destBuf->isHostEndian = isHostEndian;
        destBuf->isLittleEndian = isLittleEndian;

        // shared pointers
        destBuf->buf = buf;
        return destBuf;
    }


    /**
      * Creates a new byte buffer whose content is a shared subsequence of
      * this buffer's content.
      *
      * <p> The content of the new buffer will start at this buffer's current
      * position.  Changes to this buffer's content will be visible in the new
      * buffer, and vice versa; the two buffers' position, limit, and mark
      * values will be independent.
      *
      * <p> The new buffer's position will be zero, its capacity and its limit
      * will be the number of bytes remaining in this buffer, and its mark
      * will be undefined.
      *
      * @return  The new byte buffer.
      */
    std::shared_ptr<ByteBuffer> ByteBuffer::slice() {

        size_t remaining = (pos <= lim ? lim - pos : 0);

        auto sliceBuf = std::make_shared<ByteBuffer>(remaining);
        sliceBuf->cap = remaining;
        sliceBuf->lim = remaining;
        sliceBuf->pos = 0;
        sliceBuf->mrk = -1;
        sliceBuf->off = pos + off;

        sliceBuf->totalSize = totalSize;
        sliceBuf->byteOrder = byteOrder;
        sliceBuf->isHostEndian = isHostEndian;
        sliceBuf->isLittleEndian = isLittleEndian;

        // shared pointers
        sliceBuf->buf = buf;
        return sliceBuf;
    }


    /**
    * The given (and returned) byte buffer is one whose content is a shared subsequence of
    * this buffer's content.
    *
    * <p> The content of the given buffer will start at this buffer's current
    * position.  Changes to this buffer's content will be visible in the given
    * buffer, and vice versa; the two buffers' position, limit, and mark
    * values will be independent.
    *
    * <p> The given buffer's position will be zero, its capacity and its limit
    * will be the number of bytes remaining in this buffer, and its mark
    * will be undefined.
    *
     * @param destBuf byte buffer to be made a slice of this one.
     * @return  the same byte buffer as passed in as the argument.
    */
    ByteBuffer & ByteBuffer::slice(ByteBuffer & destBuf) {

        size_t remaining = (pos <= lim ? lim - pos : 0);

        destBuf.cap = remaining;
        destBuf.lim = remaining;
        destBuf.pos = 0;
        destBuf.mrk = -1;
        destBuf.off = pos + off;

        destBuf.totalSize = totalSize;
        destBuf.byteOrder = byteOrder;
        destBuf.isHostEndian = isHostEndian;
        destBuf.isLittleEndian = isLittleEndian;

        // shared pointers
        destBuf.buf = buf;
        return destBuf;
    }


    /**
    * The given (and returned) byte buffer is one whose content is a shared subsequence of
    * this buffer's content.
    *
    * <p> The content of the given buffer will start at this buffer's current
    * position.  Changes to this buffer's content will be visible in the given
    * buffer, and vice versa; the two buffers' position, limit, and mark
    * values will be independent.
    *
    * <p> The given buffer's position will be zero, its capacity and its limit
    * will be the number of bytes remaining in this buffer, and its mark
    * will be undefined.
    *
     * @param destBuf byte buffer to be made a slice of this one.
     * @return  the same byte buffer as passed in as the argument.
    */
    std::shared_ptr<ByteBuffer> & ByteBuffer::slice(std::shared_ptr<ByteBuffer> & destBuf) {
        auto & buff = *(destBuf.get());
        slice(buff);
        return destBuf;
    }


    //----------------
    // Read Functions
    //----------------

    // NOTE: the method get(uint8_t & dst) cannot be defined in C++ since arrays don't have lengths!

    /**
     * Relative bulk <i>get</i> method.
     *
     * This method transfers bytes from this buffer into the given
     * destination array. If there are fewer bytes remaining in the
     * buffer than are required to satisfy the request, then no
     * bytes are transferred and an underflow_error is thrown.<p>
     *
     * Otherwise, this method copies <tt>length</tt> bytes from this
     * buffer into the given array, starting at the current position of this
     * buffer and at the given pointer. The position of this
     * buffer is then incremented by <tt>length</tt>.
     *
     * @param  dst array into which bytes are to be written.
     * @param  length number of bytes to be written to the given
     *         array; must be no larger than dst size - offset.
     * @return  this buffer.
     * @throws  underflow_error if fewer than <tt>length</tt> bytes remaining in buffer.
     */
    const ByteBuffer & ByteBuffer::getBytes(uint8_t *dst, size_t length) const {
        // check args
        if (length > remaining()) {
            throw std::underflow_error("buffer underflow");
        }
        memcpy((void *)dst, (void *)(buf.get() + off + pos), length);
        pos += length;
        return *this;
    }


    /**
     * Relative bulk <i>get</i> method.
     *
     * This method transfers bytes from this buffer into the given
     * destination vector. If there are fewer bytes remaining in the
     * buffer than are required to satisfy the request, then no
     * bytes are transferred and an underflow_error is thrown.<p>
     *
     * Otherwise, this method copies <tt>length</tt> bytes from this
     * buffer into the given array, starting at the current position of this
     * buffer and at the given offset in the array. The position of this
     * buffer is then incremented by <tt>length</tt>.
     *
     * @param  dst    vector into which bytes are to be written.
     * @param  offset offset (bytes) within the vector of the first byte to be written.
     * @param  length number of bytes to be written to the given vector.
     * @return  this buffer.
     * @throws  underflow_error if fewer than <tt>length</tt> bytes remaining in buffer.
     */
    const ByteBuffer & ByteBuffer::getBytes(std::vector<uint8_t> & dst, size_t offset, size_t length) const {
        // check args
        if (length > remaining()) {
            throw std::underflow_error("buffer underflow");
        }
        dst.resize(offset + length, 0);
        memcpy((void *)(dst.data() + offset), (const void *)(buf.get() + off + pos), length);
        pos += length;
        return *this;
    }


    /**
     * Relative <i>get</i> method. Reads the byte at this buffer's
     * current position, but does not increments the position.
     *
     * @return  byte at buffer's current position.
     * @throws  underflow_error if buffer's current position is not smaller than its limit.
     */
    uint8_t ByteBuffer::peek() const {return read<uint8_t>(pos);}


    /**
     * Relative <i>get</i> method. Reads the byte at this buffer's
     * current position, and then increments the position.
     *
     * @return  byte at buffer's current position.
     * @throws  underflow_error if buffer's current position is not smaller than its limit.
     */
    uint8_t ByteBuffer::getByte()  const {return read<uint8_t>();}


    /**
     * Absolute <i>get</i> method. Reads the byte at the given index.
     *
     * @param index index from which the byte will be read
     * @return  byte at the given index
     * @throws  underflow_error if index is not smaller than buffer's limit.
     */
    uint8_t ByteBuffer::getByte(size_t index) const {return read<uint8_t>(index);}


    /**
     * Relative <i>get</i> method for reading a 16-bit unicode character value
     * which is by nature unsigned.
     * Keeping the name as "getChar" makes it compatible with Java, but be aware
     * it does NOT get a 8-bit "char" type.
     * This method reads the next two bytes at this buffer's current position,
     * composing them into a wchar_t value according to the current byte order,
     * and then increments the position by two.</p>
     *
     * @return  wchar_t value at buffer's current position
     * @throws  underflow_error if fewer than two bytes remaining in buffer.
     */
    wchar_t ByteBuffer::getChar() const {return read<wchar_t>();}


    /**
     * Absolute <i>get</i> method for reading a 16-bit unicode character value
     * which is by nature unsigned. Reads at the given index.
     * Keeping the name as "getChar" makes it compatible with Java, but be aware
     * it does NOT get a 8-bit "char" type.
     * This method reads the next two bytes at the given index,
     * composing them into a wchar_t value according to the current byte order,
     * and then increments the position by two.</p>
     *
     * @param index index from which the bytes will be read.
     * @return  wchar_t value at the given index.
     * @throws  underflow_error if fewer than two bytes remaining in buffer.
     */
    wchar_t ByteBuffer::getChar(size_t index) const {return read<wchar_t>(index);}


    /**
     * Relative <i>get</i> method for reading a short value.
     * Reads the next two bytes at this buffer's current position,
     * composing them into a short value according to the current
     * byte order, and then increments the position by two.</p>
     *
     * @return  short value at buffer's current position.
     * @throws  underflow_error if fewer than two bytes remaining in buffer.
     */
    int16_t ByteBuffer::getShort() const {
        // If data & host are the same endian, no swap necessary
        if (isHostEndian) {
            return read<int16_t>();
        }
        else {
            auto s = read<int16_t>();
            return SWAP_16(s);
        }
    }


    /**
     * Absolute <i>get</i> method for reading a short value.
     * Reads two bytes at the given index, composing them into a
     * short value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  short value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus one.
     */
    int16_t ByteBuffer::getShort(size_t index) const {
        if (isHostEndian) {
            return read<int16_t>(index);
        }
        else {
            auto s = read<int16_t>(index);
            return SWAP_16(s);
        }
    }


    /**
     * Relative <i>get</i> method for reading an unsigned short value.
     * Reads the next two bytes at this buffer's current position,
     * composing them into an unsigned short value according to the current
     * byte order, and then increments the position by two.</p>
     * This method is not defined in the Java version.
     *
     * @return  unsigned short value at buffer's current position.
     * @throws  underflow_error if fewer than two bytes remaining in buffer.
     */
    uint16_t ByteBuffer::getUShort() const {
        if (isHostEndian) {
            return read<uint16_t>();
        }
        else {
            auto s = read<uint16_t>();
            return SWAP_16(s);
        }
    }


    /**
     * Absolute <i>get</i> method for reading an unsigned short value.
     * Reads two bytes at the given index, composing them into an
     * unsigned short value according to the current byte order.</p>
     * This method is not defined in the Java version.
     *
     * @param  index  index from which the bytes will be read.
     * @return  unsigned short value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus one.
     */
    uint16_t ByteBuffer::getUShort(size_t index) const {
        if (isHostEndian) {
            return read<uint16_t>(index);
        }
        else {
            auto s = read<uint16_t>(index);
            return SWAP_16(s);
        }
    }


    /**
     * Relative <i>get</i> method for reading an int value.
     * Reads the next four bytes at this buffer's current position,
     * composing them into an int value according to the current
     * byte order, and then increments the position by four.</p>
     *
     * @return  int value at buffer's current position.
     * @throws  underflow_error if fewer than four bytes remaining in buffer.
     */
    int32_t ByteBuffer::getInt() const {
        if (isHostEndian) {
            return read<int32_t>();
        }
        else {
            auto s = read<int32_t>();
            return SWAP_32(s);
        }
    }


    /**
     * Absolute <i>get</i> method for reading an int value.
     * Reads four bytes at the given index, composing them into an
     * int value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  int value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus three.
     */
    int32_t ByteBuffer::getInt(size_t index) const {
        if (isHostEndian) {
            return read<int32_t>(index);
        }
        else {
            auto s = read<int32_t>(index);
            return SWAP_32(s);
        }
    }


    /**
     * Relative <i>get</i> method for reading an unsigned int value.
     * Reads the next four bytes at this buffer's current position,
     * composing them into an unsigned int value according to the current
     * byte order, and then increments the position by four.</p>
     *
     * @return  unsigned int value at buffer's current position.
     * @throws  underflow_error if fewer than four bytes remaining in buffer.
     */
    uint32_t ByteBuffer::getUInt() const {
        if (isHostEndian) {
            return read<uint32_t>();
        }
        else {
            auto s = read<uint32_t>();
            return SWAP_32(s);
        }
    }


    /**
     * Absolute <i>get</i> method for reading an unsigned int value.
     * Reads four bytes at the given index, composing them into an
     * unsigned int value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  unsigned int value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus three.
     */
    uint32_t ByteBuffer::getUInt(size_t index) const {
        if (isHostEndian) {
            return read<uint32_t>(index);
        }
        else {
            auto s = read<uint32_t>(index);
            return SWAP_32(s);
        }
    }


    /**
     * Relative <i>get</i> method for reading a long long value.
     * Reads the next eight bytes at this buffer's current position,
     * composing them into a long long value according to the current
     * byte order, and then increments the position by eight.</p>
     *
     * @return  long long value at buffer's current position.
     * @throws  underflow_error if fewer than eight bytes remaining in buffer.
     */
    int64_t ByteBuffer::getLong() const {
        if (isHostEndian) {
            return read<int64_t>();
        }
        else {
            auto s = read<int64_t>();
            return SWAP_64(s);
        }
    }


    /**
     * Absolute <i>get</i> method for reading a long long value.
     * Reads eight bytes at the given index, composing them into a
     * long long value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  long long value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus seven.
     */
    int64_t ByteBuffer::getLong(size_t index) const {
        if (isHostEndian) {
            return read<int64_t>(index);
        }
        else {
            auto s = read<int64_t>(index);
            return SWAP_64(s);
        }
    }


    /**
     * Relative <i>get</i> method for reading an unsigned long long value.
     * Reads the next eight bytes at this buffer's current position,
     * composing them into an unsigned long long value according to the current
     * byte order, and then increments the position by eight.</p>
     *
     * @return  unsigned long long value at buffer's current position.
     * @throws  underflow_error if fewer than eight bytes remaining in buffer.
     */
    uint64_t ByteBuffer::getULong() const {
        if (isHostEndian) {
            return read<uint64_t>();
        }
        else {
            auto s = read<uint64_t>();
            return SWAP_64(s);
        }
    }


    /**
     * Absolute <i>get</i> method for reading an unsigned long long value.
     * Reads eight bytes at the given index, composing them into an
     * unsigned long long value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  unsigned long long value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus seven.
     */
    uint64_t ByteBuffer::getULong(size_t index) const {
        if (isHostEndian) {
            return read<uint64_t>(index);
        }
        else {
            auto s = read<uint64_t>(index);
            return SWAP_64(s);
        }
    }


    /**
     * Relative <i>get</i> method for reading a float value.
     * Reads the next four bytes at this buffer's current position,
     * composing them into a float value according to the current
     * byte order, and then increments the position by four.</p>
     *
     * @return  float value at buffer's current position.
     * @throws  underflow_error if fewer than four bytes remaining in buffer.
     */
    float ByteBuffer::getFloat() const {
        if (isHostEndian) {
            return read<float>();
        }
        return ByteOrder::byteSwap(read<float>());
    }


    /**
     * Absolute <i>get</i> method for reading a float value.
     * Reads four bytes at the given index, composing them into a
     * float value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  float value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus three.
     */
    float ByteBuffer::getFloat(size_t index) const {
        if (isHostEndian) {
            return read<float>(index);
        }
        return ByteOrder::byteSwap(read<float>(index));
    }


    /**
     * Relative <i>get</i> method for reading a double value.
     * Reads the next eight bytes at this buffer's current position,
     * composing them into a double value according to the current
     * byte order, and then increments the position by eight.</p>
     *
     * @return  double value at buffer's current position.
     * @throws  underflow_error if fewer than eight bytes remaining in buffer.
     */
    double ByteBuffer::getDouble() const {
        if (isHostEndian) {
            return read<double>();
        }
        return ByteOrder::byteSwap(read<double>());
    }


    /**
     * Absolute <i>get</i> method for reading a double value.
     * Reads eight bytes at the given index, composing them into a
     * double value according to the current byte order.</p>
     *
     * @param  index  index from which the bytes will be read.
     * @return  double value at the given index.
     * @throws  underflow_error if <tt>index</tt> is not smaller than buffer's limit,
     *          minus seven.
     */
    double ByteBuffer::getDouble(size_t index) const {
        if (isHostEndian) {
            return read<double>(index);
        }
        return ByteOrder::byteSwap(read<double>(index));
    }


//----------------
// Write Methods
//----------------


    /**
     * Relative bulk <i>put</i> method.<p>
     *
     * This method transfers the bytes remaining in the given source
     * buffer into this buffer. If there are more bytes remaining in the
     * source buffer than in this buffer, that is, if
     * <tt>src.remaining()</tt>&nbsp;<tt>&gt;</tt>&nbsp;<tt>remaining()</tt>,
     * then no bytes are transferred and a {@link std::overflow_error} is thrown.<p>
     *
     * Otherwise, this method copies
     * <i>n</i>&nbsp;=&nbsp;<tt>src.remaining()</tt> bytes from the given
     * buffer into this buffer, starting at each buffer's current position.
     * The positions of both buffers are then incremented by <i>n</i>.
     *
     * @param  src source buffer from which bytes are to be read;
     *             must not be this buffer.
     * @return  this buffer.
     * @throws  std::runtime_error if source buffer is this buffer.
     * @throws  overflow_error if insufficient space in this buffer
     *          for the remaining bytes in the source buffer.
     */
    ByteBuffer & ByteBuffer::put(const ByteBuffer & src) {
        if (&src == this) {
            throw std::runtime_error("may not copy data to self");
        }

        size_t srcBytes = src.remaining();
        if (srcBytes > remaining()) {
            throw std::overflow_error("buffer overflow");
        }

        std::memcpy((void *)(buf.get() + off + pos),
                    (const void *)(src.buf.get() + src.off + src.pos), srcBytes);
        pos += srcBytes;
        src.pos += srcBytes;
        return *this;
    }


    /**
     * Relative bulk <i>put</i> method.<p>
     *
     * This method transfers the bytes remaining in the given source
     * buffer into this buffer. If there are more bytes remaining in the
     * source buffer than in this buffer, that is, if
     * <tt>src.remaining()</tt>&nbsp;<tt>&gt;</tt>&nbsp;<tt>remaining()</tt>,
     * then no bytes are transferred and a {@link std::overflow_error} is thrown.<p>
     *
     * Otherwise, this method copies
     * <i>n</i>&nbsp;=&nbsp;<tt>src.remaining()</tt> bytes from the given
     * buffer into this buffer, starting at each buffer's current position.
     * The positions of both buffers are then incremented by <i>n</i>.
     *
     * @param  src source buffer from which bytes are to be read;
     *             must not be this buffer.
     * @return  this buffer.
     * @throws  std::runtime_error if source buffer is this buffer.
     * @throws  overflow_error if insufficient space in this buffer
     *          for the remaining bytes in the source buffer.
     */
    ByteBuffer & ByteBuffer::put(const std::shared_ptr<ByteBuffer> & src) {
        return (put(*(src.get())));
    }


// NOTE: the method put(uint8_t* src) cannot be defined in C++ since arrays don't have lengths


    /**
     * Relative bulk <i>put</i> method.<p>
     *
     * This method transfers bytes into this buffer from the given
     * source array.  If there are more bytes to be copied from the array
     * than remain in this buffer, that is, if
     * <tt>length</tt>&nbsp;<tt>&gt;</tt>&nbsp;<tt>remaining()</tt>, then no
     * bytes are transferred and a {@link std::overflow_error} is
     * thrown.<p>
     *
     * Otherwise, this method copies <tt>length</tt> bytes from the
     * given array into this buffer, starting at the given pointer
     * and at the current position of this buffer.  The position of this buffer
     * is then incremented by <tt>length</tt>.
     *
     * @param  src array from which bytes are to be read
     * @param  length number of bytes to be read from the given array;
     *         must be no larger than src array's size - offset.
     * @return  this buffer.
     *
     * @throws  overflow_error if insufficient space in this buffer
     */
    ByteBuffer & ByteBuffer::put(const uint8_t* src, size_t length) {
        // check args
        if (length > remaining()) {
            throw std::overflow_error("buffer overflow");
        }
        std::memcpy((void *)(buf.get() + off + pos), (const void *)src, length);
        pos += length;
        return *this;
    }

    /**
     * Relative bulk <i>put</i> method.<p>
     *
     * This method transfers bytes into this buffer from the given
     * vector.  If there are more bytes to be copied from the vector
     * than remain in this buffer, that is, if
     * <tt>length</tt>&nbsp;<tt>&gt;</tt>&nbsp;<tt>remaining()</tt>, then no
     * bytes are transferred and a {@link std::overflow_error} is
     * thrown.<p>
     *
     * Otherwise, this method copies <tt>length</tt> bytes from the
     * given array into this buffer, starting at the given offset in the array
     * and at the current position of this buffer.  The position of this buffer
     * is then incremented by <tt>length</tt>.
     *
     * @param  src array from which bytes are to be read
     * @param  offset offset (bytes) within the array of the first byte to be read;
     *         must be no larger than src array's size.
     * @param  length number of bytes to be read from the given array;
     *         must be no larger than src array's size - offset.
     * @return  this buffer.
     *
     * @throws  std::overflow_error if insufficient space in this buffer
     */
    ByteBuffer & ByteBuffer::put(const std::vector<uint8_t> & src, size_t offset, size_t length) {
        // check args
        if (length > (src.size() - offset)) {
            throw std::overflow_error("buffer overflow");
        }
        std::memcpy((void *)(buf.get() + off + pos), (const void *)(src.data() + offset), length);
        pos += length;
        return *this;
    }


    /**
     * Relative <i>put</i> method.
     * Writes the given byte into this buffer at the current
     * position, and then increments the position by one.</p>
     *
     * @param  val byte value to be written.
     * @return  this buffer.
     * @throws  overflow_error if buffer's current position is not smaller than its limit
     */
    ByteBuffer & ByteBuffer::put(uint8_t val) {
        write(val);
        return *this;
    }


    /**
     * Absolute <i>put</i> method.
     * Writes the given byte into this buffer at the given index.
     *
     * @param  index index at which the byte will be written.
     * @param  val byte value to be written.
     * @return  this buffer.
     * @throws  overflow_error if buffer's current position is not smaller than its limit
     */
    ByteBuffer & ByteBuffer::put(size_t index, uint8_t val) {
        write(val, index);
        return *this;
    }


    /**
     * Relative <i>put</i> method for writing a wide char value.
     * Writes two bytes containing the given char value, in the
     * current byte order, into this buffer at the current position, and then
     * increments the position by two.
     *
     * @param  val char value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than two bytes remaining in this buffer,
     *                        i.e. not smaller than the buffer's limit, minus one.
     */
    ByteBuffer & ByteBuffer::putChar(wchar_t val) {
        if (isHostEndian) {
            write(val);
        }
        else {
            uint16_t s = SWAP_16(val);
            write(s);
        }
        return *this;
    }


    /**
     * Absolute <i>put</i> method for writing a wide char value.
     * Writes two bytes containing the given char value, in the
     * current byte order, into this buffer at the given index.
     *
     * @param  index index at which the bytes will be written.
     * @param  val char value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than two bytes remaining in this buffer,
     *                        i.e. not smaller than the buffer's limit, minus one.
     */
    ByteBuffer & ByteBuffer::putChar(size_t index, wchar_t val) {
        if (isHostEndian) {
            write(val, index);
        }
        else {
            uint16_t s = SWAP_16(val);
            write(s, index);
        }
        return *this;
    }


    /**
     * Relative <i>put</i> method for writing a short value.
     * Writes two bytes containing the given short value, in the
     * current byte order, into this buffer at the current position, and then
     * increments the position by two.
     *
     * @param  val short value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than two bytes remaining in this buffer.
     */
    ByteBuffer & ByteBuffer::putShort(uint16_t val) {
        if (isHostEndian) {
            write(val);
        }
        else {
            uint16_t s = SWAP_16(val);
            write(s);
        }
        return *this;
    }


    /**
     * Absolute <i>put</i> method for writing a short value.
     * Writes two bytes containing the given short value, in the
     * current byte order, into this buffer at the given index.
     *
     * @param  index index at which the bytes will be written.
     * @param  val short value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than two bytes remaining in this buffer,
     *          i.e. not smaller than the buffer's limit, minus one.
     */
    ByteBuffer & ByteBuffer::putShort(size_t index, uint16_t val) {
        if (isHostEndian) {
            write(val, index);
        }
        else {
            uint16_t s = SWAP_16(val);
            write(s, index);
        }
        return *this;
    }


    /**
     * Relative <i>put</i> method for writing an int value.
     * Writes four bytes containing the given int value, in the
     * current byte order, into this buffer at the current position, and then
     * increments the position by four.
     *
     * @param  val int value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than four bytes remaining in this buffer.
     */
    ByteBuffer & ByteBuffer::putInt(uint32_t val) {
        if (isHostEndian) {
            write(val);
        }
        else {
            uint32_t i = SWAP_32(val);
            write(i);
        }
        return *this;
    }


    /**
     * Absolute <i>put</i> method for writing an int value.
     * Writes four bytes containing the given int value, in the
     * current byte order, into this buffer at the given index.
     *
     * @param  index index at which the bytes will be written.
     * @param  val int value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than four bytes remaining in this buffer,
     *          i.e. not smaller than the buffer's limit, minus three.
     */
    ByteBuffer & ByteBuffer::putInt(size_t index, uint32_t val) {
        if (isHostEndian) {
            write(val, index);
        }
        else {
            uint32_t i = SWAP_32(val);
            write(i, index);
        }
        return *this;
    }


    /**
     * Relative <i>put</i> method for writing a long long value.
     * Writes eight bytes containing the given long long value, in the
     * current byte order, into this buffer at the current position, and then
     * increments the position by eight.
     *
     * @param  val long long value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than eight bytes remaining in this buffer.
     */
    ByteBuffer & ByteBuffer::putLong(uint64_t val) {
        if (isHostEndian) {
            write(val);
        }
        else {
            uint64_t l = SWAP_64(val);
            write(l);
        }
        return *this;
    }


    /**
     * Absolute <i>put</i> method for writing a long long value.
     * Writes eight bytes containing the given long long value, in the
     * current byte order, into this buffer at the given index.
     *
     * @param  index index at which the bytes will be written.
     * @param  val long long value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than eight bytes remaining in this buffer,
     *          i.e. not smaller than the buffer's limit, minus seven.
     */
    ByteBuffer & ByteBuffer::putLong(size_t index, uint64_t val) {
        if (isHostEndian) {
            write(val, index);
        }
        else {
            uint64_t l = SWAP_64(val);
            write(l, index);
        }
        return *this;
    }


    /**
     * Relative <i>put</i> method for writing a float value.
     * Writes four bytes containing the given float value, in the
     * current byte order, into this buffer at the current position, and then
     * increments the position by four.
     *
     * @param  val float value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than four bytes remaining in this buffer.
     */
    ByteBuffer & ByteBuffer::putFloat(float val) {
        if (isHostEndian) {
            write(val);
        }
        else {
            float f = ByteOrder::byteSwap(val);
            write(f);
        }
        return *this;
    }


    /**
     * Absolute <i>put</i> method for writing a float value.
     * Writes four bytes containing the given float value, in the
     * current byte order, into this buffer at the given index.
     *
     * @param  index index at which the bytes will be written.
     * @param  val float value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than four bytes remaining in this buffer,
     *          i.e. not smaller than the buffer's limit, minus three.
     */
    ByteBuffer & ByteBuffer::putFloat(size_t index, float val) {
        if (isHostEndian) {
            write(val, index);
        }
        else {
            float f = ByteOrder::byteSwap(val);
            write(f, index);
        }
        return *this;
    }


    /**
     * Relative <i>put</i> method for writing a double value.
     * Writes eight bytes containing the given double value, in the
     * current byte order, into this buffer at the current position, and then
     * increments the position by eight.
     *
     * @param  val double value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than eight bytes remaining in this buffer.
     */
    ByteBuffer & ByteBuffer::putDouble(double val) {
        if (isHostEndian) {
            write(val);
        }
        else {
            double d = ByteOrder::byteSwap(val);
            write(d);
        }
        return *this;
    }


    /**
     * Absolute <i>put</i> method for writing a double value.
     * Writes eight bytes containing the given double value, in the
     * current byte order, into this buffer at the given index.
     *
     * @param  index index at which the bytes will be written.
     * @param  val double value to be written.
     * @return  this buffer.
     * @throws  overflow_error if there are fewer than eight bytes remaining in this buffer,
     *          i.e. not smaller than the buffer's limit, minus seven.
     */
    ByteBuffer & ByteBuffer::putDouble(size_t index, double val) {
        if (isHostEndian) {
            write(val, index);
        }
        else {
            double d = ByteOrder::byteSwap(val);
            write(d, index);
        }
        return *this;
    }


//----------------
// Utility Methods
//----------------


    /**
     * This method prints out the desired number of data bytes starting from the given index
     * without regard to the limit.
     *
     * @param offset  offset into data
     * @param bytes   number of bytes to print in hex
     * @param label   a label to print as header
     */
    void ByteBuffer::printBytes(size_t offset, size_t bytes, std::string const & label) {

        size_t limit = (bytes + offset) > cap ? cap : (bytes + offset);

        std::cout << label <<  ":" << std::endl;

        for (size_t i = offset; i < limit; i++) {
            if (i%20 == 0) {
                std::cout << "\n  array[" << (i + 1) << "-" << (i + 20) << "] =  ";
            }
            else if (i%4 == 0) {
                std::cout << "  ";
            }

            printf("%02x ", buf.get()[i + off]);
        }
        std::cout << std::endl << std::endl;
    }


    /**
     * Obtain a string representation of the buffer.
     * @return a string representation of the buffer.
     */
    std::string ByteBuffer::toString() const {
        std::stringstream ss;

        ss << "buffer capacity: " << cap << std::endl;
        ss << "          limit: " << lim << std::endl;
        ss << "       position: " << pos << std::endl;
        ss << "           mark: " << mrk << std::endl;
        ss << "            off: " << off << std::endl;
        ss << "      remaining: " << (lim-pos) << std::endl;

        return ss.str();
    }



} // namespace evio
