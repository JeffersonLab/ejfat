//
// Copyright 2020, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef UTIL_BYTEORDER_H
#define UTIL_BYTEORDER_H


#include <string>
#include <iostream>


namespace ejfat {


    /** Macro for swapping 16 bit types. */
#define SWAP_16(x) \
    ((uint16_t)((((uint16_t)(x)) >> 8) | \
                (((uint16_t)(x)) << 8)))

    /** Macro for swapping 32 bit types. */
#define SWAP_32(x) \
    ((uint32_t)((((uint32_t)(x)) >> 24) | \
                (((uint32_t)(x) & 0x00ff0000) >>  8) | \
                (((uint32_t)(x) & 0x0000ff00) <<  8) | \
                (((uint32_t)(x)) << 24)))

    /** Macro for swapping 64 bit types. */
#define SWAP_64(x) \
    ((uint64_t)((((uint64_t)(x)) >> 56) | \
                (((uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | \
                (((uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | \
                (((uint64_t)(x) & 0x000000ff00000000ULL) >>  8) | \
                (((uint64_t)(x) & 0x00000000ff000000ULL) <<  8) | \
                (((uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | \
                (((uint64_t)(x) & 0x000000000000ff00ULL) << 40) | \
                (((uint64_t)(x)) << 56)))


    /**
     * Numerical values associated with endian byte order.
     *
     * @version 6.0
     * @since 6.0 4/16/2019
     * @author timmer
     */
    class ByteOrder {

    public:

        static const ByteOrder ENDIAN_LITTLE;
        static const ByteOrder ENDIAN_BIG;

        static const ByteOrder ENDIAN_UNKNOWN;
        static const ByteOrder ENDIAN_LOCAL;

    private:

        /** Value of this endian type. */
        int value;

        /** Store a name for each ByteOrder object. */
        std::string name;

        /**
         * Constructor.
         * @param val   int value of this headerType object.
         * @param name  name (string representation) of this headerType object.
         */
        ByteOrder(int val, std::string name) : value(val), name(std::move(name)) {}

    public:

        /**
         * Get the object name.
         * @return the object name.
         */
        std::string getName() const {return name;}

        /**
         * Is this big endian?
         * @return true if big endian, else false.
         */
        bool isBigEndian()    const {return (value == ENDIAN_BIG.value);}

        /**
         * Is this little endian?
         * @return true if little endian, else false.
         */
        bool isLittleEndian() const {return (value == ENDIAN_LITTLE.value);}

        /**
         * Is this endian same as the local host?
         * @return true if endian is the same as the local host, else false.
         */
        bool isLocalEndian()  const {return (value == ENDIAN_LOCAL.value);}

        /**
         * Get the oppposite endian (little if this is big and vice versa).
         * @return he oppposite endian.
         */
        ByteOrder getOppositeEndian() const {
            return isBigEndian() ? ENDIAN_LITTLE : ENDIAN_BIG;
        }


        bool operator==(const ByteOrder &rhs) const;

        bool operator!=(const ByteOrder &rhs) const;


        /**
         * Get the byte order of the local host.
         * @return byte order of the local host.
         */
        static ByteOrder const & getLocalByteOrder() {
            if (isLocalHostBigEndian()) {
                return ENDIAN_BIG;
            }
            return ENDIAN_LITTLE;
        }

        /**
         * Get the byte order of the local host.
         * @return byte order of the local host.
         */
        static ByteOrder const & nativeOrder() {
            return getLocalByteOrder();
        };

        /**
         * Is the local host big endian?
         * @return true if the local host is big endian, else false.
         */
        static bool isLocalHostBigEndian() {
            int32_t i = 1;
            return !*((char *) &i);
        }

        /**
         * Is the argument the opposite of the local host's endian?
         * @param order byte order to compare to local host's.
         * @return true if the argument is the opposite of the local host's endian.
         */
        static bool needToSwap(ByteOrder & order) {
            return !(order == getLocalByteOrder());
        }

        // Templates to swap stuff in place

        /**
         * Templated method to swap data in place.
         * @tparam T type of data.
         * @param var reference to data to be swapped.
         */
        template <typename T>
        static void byteSwapInPlace(T& var);

        /**
         * Templated method to swap array data in place.
         * @tparam T data type.
         * @param var reference to data to be swapped.
         * @param elements number of data elements to be swapped.
         */
        template <typename T>
        static void byteSwapInPlace(T& var, size_t elements);

        /**
         * Templated method to swap array data in place.
         * If source pointer is null, nothing is done.
         * @tparam T data type.
         * @param var pointer to data to be swapped.
         * @param elements number of data elements to be swapped.
         */
        template <typename T>
        static void byteSwapInPlace(T* var, size_t elements);

        // Methods to swap and return floats and doubles
        static float byteSwap(float var);
        static double byteSwap(double var);

        // Swapping arrays
        static uint16_t* byteSwap16(uint16_t* src, size_t elements, uint16_t* dst);
        static uint32_t* byteSwap32(uint32_t* src, size_t elements, uint32_t* dst);
        static uint64_t* byteSwap64(uint64_t* src, size_t elements, uint64_t* dst);
        static void      byteNoSwap32(const uint32_t* src, size_t elements, uint32_t* dst);

    };

}


#endif // UTIL_BYTEORDER_H
