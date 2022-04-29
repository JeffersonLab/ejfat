//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#include "ByteOrder.h"


namespace ejfat {


    template <typename T>
    void ByteOrder::byteSwapInPlace(T& var) {
        char* varArray = reinterpret_cast<char*>(&var);
        for (size_t i = 0;  i < sizeof(T)/2;  i++)
            std::swap(varArray[sizeof(var) - 1 - i],varArray[i]);
    }

    /**
     * Convenience method to return swapped float.
     * @param var float to swap
     * @return swapped float.
     */
    float ByteOrder::byteSwap(float var) {
        char* varArray = reinterpret_cast<char*>(&var);
        for (size_t i = 0;  i < 2;  i++)
            std::swap(varArray[3 - i],varArray[i]);
        return var;
    }

    /**
     * Convenience method to return swapped double.
     * @param var double to swap
     * @return swapped double.
     */
    double ByteOrder::byteSwap(double var) {
        char* varArray = reinterpret_cast<char*>(&var);
        for (size_t i = 0;  i < 4;  i++)
            std::swap(varArray[7 - i],varArray[i]);
        return var;
    }

    template <typename T>
    void ByteOrder::byteSwapInPlace(T& var, size_t elements) {
        char *c = reinterpret_cast<char *>(&var);
        size_t varSize = sizeof(T);

        for (size_t j = 0;  j < elements;  j++) {
            for (size_t i = 0; i < varSize/2; i++) {
                std::swap(c[varSize - 1 - i], c[i]);
            }
            c += varSize;
        }
    }

    template <typename T>
    void ByteOrder::byteSwapInPlace(T* var, size_t elements) {
        if (var == nullptr) return;

        char *c = reinterpret_cast<char *>(var);
        size_t varSize = sizeof(T);

        for (size_t j = 0;  j < elements;  j++) {
            for (size_t i = 0; i < varSize/2; i++) {
                std::swap(c[varSize - 1 - i], c[i]);
            }
            c += varSize;
        }
    }

    /**
     * This method swaps an array of 2-byte data.
     * If source pointer is null, nothing is done.
     * If destination pointer is null, src is swapped in place.
     *
     * @param src pointer to data source.
     * @param elements number of 2-byte elements to swap.
     * @param dst pointer to destination or nullptr if data is to be swapped in place.
     * @return pointer to beginning of swapped data; null if src is null.
     */
    uint16_t* ByteOrder::byteSwap16(uint16_t* src, size_t elements, uint16_t* dst) {
        if (src == nullptr) return nullptr;

        if (dst == nullptr) {
            dst = src;
        }

        for (size_t i=0; i < elements; i++) {
            dst[i] = SWAP_16(src[i]);
        }
        return dst;
    }

    /**
     * This method swaps an array of 4-byte data.
     * If source pointer is null, nothing is done.
     * If destination pointer is null, src is swapped in place.
     *
     * @param src pointer to data source.
     * @param elements number of 4-byte elements to swap.
     * @param dst pointer to destination or nullptr if data is to be swapped in place.
     * @return pointer to beginning of swapped data; null if src is null.
     */
    uint32_t* ByteOrder::byteSwap32(uint32_t* src, size_t elements, uint32_t* dst) {
        if (src == nullptr) return nullptr;

        if (dst == nullptr) {
            dst = src;
        }

        for (size_t i=0; i < elements; i++) {
            dst[i] = SWAP_32(src[i]);
        }
        return dst;
    }

    /**
     * This method swaps an array of 8-byte data.
     * If source pointer is null, nothing is done.
     * If destination pointer is null, src is swapped in place.
     *
     * @param src pointer to data source.
     * @param elements number of 8-byte elements to swap.
     * @param dst pointer to destination or nullptr if data is to be swapped in place.
     * @return pointer to beginning of swapped data; null if src is null.
     */
    uint64_t* ByteOrder::byteSwap64(uint64_t* src, size_t elements, uint64_t* dst) {
        if (src == nullptr) return nullptr;

        if (dst == nullptr) {
            dst = src;
        }

        for (size_t i=0; i < elements; i++) {
            dst[i] = SWAP_64(src[i]);
        }
        return dst;
    }


    /**
     * This routine swaps nothing, it just copies the given number of 32 bit ints.
     * If source pointer is null, nothing is done.
     *
     * @param src   pointer to data to be copied
     * @param elements number of 32 bit ints to be copied
     * @param dst   pointer to where data is to be copied to.
     *               If null, nothing is done.
     */
    void ByteOrder::byteNoSwap32(const uint32_t* src, size_t elements, uint32_t* dst) {
        if (src == nullptr) return;

        if (dst == nullptr) {
            return;
        }

        for (size_t i=0; i < elements; i++) {
            dst[i] = src[i];
        }
    }


    // Enum value DEFINITIONS

    /** Little endian byte order. */
    const ByteOrder ByteOrder::ENDIAN_LITTLE(0, "ENDIAN_LITTLE");
    /** Big endian byte order. */
    const ByteOrder ByteOrder::ENDIAN_BIG(1, "ENDIAN_BIG");
    /** Unknown endian byte order. */
    const ByteOrder ByteOrder::ENDIAN_UNKNOWN(2, "ENDIAN_UNKNOWN");
    /** Local host's byte order. */
    const ByteOrder ByteOrder::ENDIAN_LOCAL = ByteOrder::getLocalByteOrder();


    bool ByteOrder::operator==(const ByteOrder &rhs) const {
        return value == rhs.value;
    }

    bool ByteOrder::operator!=(const ByteOrder &rhs) const {
        return value != rhs.value;
    }




}

