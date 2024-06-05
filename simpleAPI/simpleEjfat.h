//
// Created by Carl Timmer on 5/30/24.
//

#ifndef EJFAT_SIMPLEEJFAT_H
#define EJFAT_SIMPLEEJFAT_H

#ifdef __linux__
    #define htonll(x) ((1==htonl(1)) ? (x) : (((uint64_t)htonl((x) & 0xFFFFFFFFUL)) << 32) | htonl((uint32_t)((x) >> 32)))
    #define ntohll(x) ((1==ntohl(1)) ? (x) : (((uint64_t)ntohl((x) & 0xFFFFFFFFUL)) << 32) | ntohl((uint32_t)((x) >> 32)))
#endif

namespace ejfat {



    enum simpleCmd {
        REGISTER   = 1,
        UPDATE     = 2,
        DEREGISTER = 3
    };


    /**
      * Write the de-registration msg from consumer to simple server into the buffer.
      * This msg eventually gets modified and passed on to the CP.
      *
      * <ol>
      * <li>data receiving port of consumer</li>
      * <li>length in bytes of following IP address</li>
      * <li>IP address in chars</li>
      * </ol>
      * <pre>
      *
      *  0                   1                   2                   3
      *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |     Cmd       |   IP Length   |            Data Port          |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              IP Address ... (ending w/ '\0')                  |
      *  |                                                               |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      * </pre>
      *
      * @param buffer   buffer in which to write msg.
      * @param bufLen   # of available bytes in buffer.
      * @param port     data receiving port of caller.
      * @param ipAddr   data receiving IP address of caller.
      *
      * @return # of bytes written including terminating null char of ipAddr,
      *         else -1 if buffer nullptr or not enough space in buffer to write.
      */
    static int setSimpleDeregisterData(char *buffer, uint32_t bufLen,
                                       uint16_t port, std::string & ipAddr) {

        // Check to see if room in buffer
        // (1 byte cmd, 1 byte len, 2 bytes port, ip addr, 1 byte '\0')
        uint8_t ipLen = static_cast<uint8_t>(ipAddr.length());
        if (buffer == nullptr || (bufLen < 4 + ipLen + 1)) {
            return -1;
        }

        // Write command
        uint8_t cmd = DEREGISTER;
        *((uint8_t *)(buffer)) = cmd;

        // IP address length
        *((uint8_t *)(buffer + 1)) = ipLen;

        // Put the data-receiving port in network byte order (big endian)
        *((uint16_t *)(buffer + 2)) = htons(port);

        // write IP addr
        ipAddr.copy(buffer + 4, ipLen);
        buffer[4 + ipLen] = '\0'; // Null-terminate the string in the buffer

        return (5 + ipLen);
    }



    /**
      * Write the registration msg from consumer to simple server into the buffer.
      * This msg eventually gets modified and passed on to the CP.
      *
      * <ol>
      * <li>data receiving port of consumer</li>
      * <li>length in bytes of following IP address</li>
      * <li>IP address in chars</li>
      * </ol>
      * <pre>
      *
      *  0                   1                   2                   3
      *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |     Cmd       |   IP Length   |            Data Port          |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              IP Address ... (ending w/ '\0')                  |
      *  |                                                               |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      * </pre>
      *
      * @param buffer   buffer in which to write msg.
      * @param bufLen   # of available bytes in buffer.
      * @param port     data receiving port of caller.
      * @param ipAddr   data receiving IP address of caller.
      *
      * @return # of bytes written including terminating null char of ipAddr,
      *         else -1 if buffer nullptr or not enough space in buffer to write.
      */
    static int setSimpleRegisterData(char *buffer, uint32_t bufLen,
                                      uint16_t port, std::string & ipAddr) {

        // Check to see if room in buffer
        // (1 byte cmd, 1 byte len, 2 bytes port, ip addr, 1 byte '\0')
        uint8_t ipLen = static_cast<uint8_t>(ipAddr.length());
        if (buffer == nullptr || (bufLen < 4 + ipLen + 1)) {
            return -1;
        }

        // Write command
        uint8_t cmd = REGISTER;
        *((uint8_t *)(buffer)) = cmd;

        // IP address length
        *((uint8_t *)(buffer + 1)) = ipLen;

        // Put the data-receiving port in network byte order (big endian)
        *((uint16_t *)(buffer + 2)) = htons(port);

        // write IP addr
        ipAddr.copy(buffer + 4, ipLen);
        buffer[4 + ipLen] = '\0'; // Null-terminate the string in the buffer

        return (5 + ipLen);
    }


    /**
     * Parse the registration msg from consumer to simple server from the buffer.
     * Return parsed values in pointer args.
     *
     * <pre>
     *
     *  0                   1                   2                   3
     *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |     Cmd       |   IP Length   |            Data Port          |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     *  |              IP Address ... (ending w/ '\0')                  |
     *  |                                                               |
     *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer to parse.
     * @param bufLen   # of available bytes in buffer.
     * @param port     ref filled with data receiving port of caller.
     * @param ipAddr   ref filled with data receiving IP address of caller.
     * @return true if success, false if not enough room in buffer from which to extract data.
     */
    static bool parseSimpleRegisterData(const char* buffer, uint32_t bufLen,
                                        uint16_t & port, std::string & ipAddr) {
        if (buffer == nullptr) return false;

        uint8_t len = *reinterpret_cast<const uint8_t *>(buffer+1);
        port = ntohs(*reinterpret_cast<const uint16_t *>(buffer+2));

        // Check to see if room in buffer to completely read its contents
        if (bufLen < 4 + len + 1) {
            return false;
        }

        // Copy IP address from buffer to ipAddr string, assumes ending '\0'
        ipAddr.assign(buffer + 4);

        return true;
    }



    /**
     * Parse a msg from consumer and gets it's command.
     *
     * <pre>
     *
     *  0
     *  0 1 2 3 4 5 6 7 8
     *  +-+-+-+-+-+-+-+-+
     *  |     Cmd       |
     *  +-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer to parse.
     * @param cmd      ref to enum filled with command value in buffer.
     * @return true if success, false if buffer is null or buffer contains a bad value.
     */
    static bool getCommand(const char* buffer, simpleCmd & cmd) {
        if (buffer == nullptr) return false;

        uint8_t val = *reinterpret_cast<const uint8_t *>(buffer);
        simpleCmd enumValue = static_cast<simpleCmd>(val);

        // Check if the enum value is valid
        if (enumValue == REGISTER || enumValue == UPDATE || enumValue == DEREGISTER) {
            cmd = enumValue;
            return true;
        }
        return false;
    }


    /**
      * Write the registration msg from consumer to simple server into the buffer.
      * This msg eventually gets modified and passed on to the CP.
      *
      * <ol>
      * <li>data receiving port of consumer</li>
      * <li>length in bytes of following IP address</li>
      * <li>IP address in chars</li>
      * </ol>
      * <pre>
      *
      *  0                   1                   2                   3
      *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |     Cmd       |   IP Length   |            Data Port          |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              IP Address ... (ending w/ '\0')                  |
      *  |                                                               |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              fill  (4 bytes)                                  |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              pidErr  (4 bytes)                                |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              isReady  (4 bytes)                               |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      * </pre>
      *
      * @param buffer   buffer in which to write msg.
      * @param bufLen   # of available bytes in buffer.
      * @param port     data receiving port of caller.
      * @param ipAddr   data receiving IP address of caller.
      * @param fill     % of fifo filled
      * @param pidErr   pid error (units of % fifo filled)
      * @param isReady  true if consumer ready for more data.
      *
      * @return # of bytes written including terminating null char of ipAddr,
      *         else -1 if buffer nullptr or not enough space in buffer to write.
      */
    static int setUpdateStatusData(char *buffer, uint32_t bufLen,
                                   uint16_t port, std::string & ipAddr,
                                   float fill, float pidErr, bool isReady) {

        // Check to see if room in buffer
        // (1 byte cmd, 1 byte len, 2 bytes port, ip addr, 1 byte '\0', 2 floats, 1 int)
        uint8_t ipLen = static_cast<uint8_t>(ipAddr.length());
        if (buffer == nullptr || (bufLen < 16 + ipLen + 1)) {
            return -1;
        }

        // Write command
        uint8_t cmd = static_cast<uint8_t>(UPDATE);
        *((uint8_t *)(buffer)) = cmd;

        // IP address length
        *((uint8_t *)(buffer + 1)) = ipLen;

        // Put the data-receiving port in network byte order (big endian)
        *((uint16_t *)(buffer + 2)) = htons(port);

        // write IP addr
        ipAddr.copy(buffer + 4, ipLen);
        buffer[4 + ipLen] = '\0'; // Null-terminate the string in the buffer

        // Convert float to binary representation
        uint32_t floatAsBinary;
        static_assert(sizeof(fill) == sizeof(floatAsBinary), "Size mismatch between float and uint32_t");
        std::memcpy(&floatAsBinary, &fill, sizeof(fill));
        // Put the fill (as binary) in network byte order (big endian)
        *((uint32_t *)(buffer + 4 + ipLen + 1)) = htonl(floatAsBinary);

        std::memcpy(&floatAsBinary, &pidErr, sizeof(pidErr));
        *((uint32_t *)(buffer + 8 + ipLen + 1)) = htonl(floatAsBinary);

        if (isReady) {
            *((uint32_t *)(buffer + 12 + ipLen + 1)) = htonl(1);
        }
        else {
            *((uint32_t *)(buffer + 12 + ipLen + 1)) = 0;
        }

        return (13 + ipLen);
    }


    /**
     * Parse the update status msg from consumer to simple server from the buffer.
     * Return parsed values in pointer args.
     *
     * <pre>
     *
      *  0                   1                   2                   3
      *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |     Cmd       |   IP Length   |            Data Port          |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              IP Address ... (ending w/ '\0')                  |
      *  |                                                               |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              fill  (4 bytes)                                  |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              pidErr  (4 bytes)                                |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
      *  |              isReady  (4 bytes)                               |
      *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     * </pre>
     *
     * @param buffer   buffer to parse.
     * @param bufLen   # of available bytes in buffer.
     * @param port     ref filled with data receiving port of caller.
     * @param ipAddr   ref filled with data receiving IP address of caller.
     * @param fill     ref filled with % of fifo filled
     * @param pidErr   ref filled with pid error (units of % fifo filled)
     * @param isReady  ref filled with true if consumer ready for more data.
    */
    static bool parseUpdateStatusData(const char* buffer, uint32_t bufLen,
                                        uint16_t & port, std::string & ipAddr,
                                        float & fill, float & pidErr, bool & isReady) {

        if (buffer == nullptr) return false;

        uint8_t len = *reinterpret_cast<const uint8_t *>(buffer+1);
        port = ntohs(*reinterpret_cast<const uint16_t *>(buffer+2));

        // Check to see if room in buffer to completely read its contents
        if (bufLen < 16 + len + 1) {
            return false;
        }

        // Copy IP address from buffer to ipAddr string, assumes ending '\0'
        ipAddr.assign(buffer + 4);

        // Convert endianness if necessary
        uint32_t floatAsBinary = *reinterpret_cast<const uint32_t *>(buffer + 4 + len + 1);
        floatAsBinary = ntohl(floatAsBinary);
        // Convert binary representation back to float
        static_assert(sizeof(fill) == sizeof(floatAsBinary), "Size mismatch between float and uint32_t");
        std::memcpy(&fill, &floatAsBinary, sizeof(floatAsBinary));

        floatAsBinary = *reinterpret_cast<const uint32_t *>(buffer + 8 + len + 1);
        floatAsBinary = ntohl(floatAsBinary);
        std::memcpy(&pidErr, &floatAsBinary, sizeof(floatAsBinary));

        int ready = ntohl(*reinterpret_cast<const int *>(buffer + 12 + len + 1));
        isReady = static_cast<bool>(ready);

        return true;
    }
}

#endif //EJFAT_SIMPLEEJFAT_H
