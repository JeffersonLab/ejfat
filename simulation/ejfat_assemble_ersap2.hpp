//
// Copyright 2022, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


/**
 * @file Contains routines to receive UDP packets that have been "packetized"
 * (broken up into smaller UDP packets by an EJFAT packetizer).
 * The receiving program handles sequentially numbered packets that may arrive out-of-order
 * coming from an FPGA-based between this and the sending program.
 */
#ifndef EJFAT_ASSEMBLE_ERSAP2_H
#define EJFAT_ASSEMBLE_ERSAP2_H


#include <unordered_map>
#include <vector>
#include <memory>
#include <utility>

#include "et.h"
#include "et_fifo.h"


#include "ejfat_assemble_ersap.hpp"


namespace ersap {
    namespace ejfat {

        //-----------------------------------------------------------------------
        // Be sure to print to stderr as programs may pipe data to stdout!!!
        //-----------------------------------------------------------------------


        /**
         * Look at all data sources for a single tick
         * to see if the last bit has been set for each.
         * If so, return true, else return false.
         *
         * @param endCondition map with all last bit data in it.
         * @param tick tick value to examine.
         * @return return true if all data sources for a single tick have set the "last" bit,
         *         else false.
         */
        static bool allLastBitsReceived(std::unordered_map<std::pair<uint64_t, uint16_t>, bool> & endCondition,
                                        uint64_t tick) {

            bool allReceived = false;

            // Look at all data sources for a single tick,
            // to see if the last bit has been set for each.
            // If so, return true, else return false.
            for (auto & i : endCondition) {
                // ignore other ticks
                if (i.first.first != tick) {
                    continue;
                }

                // if last bit not set
                if (!i.second) {
                    return false;
                }

                // protect against tick not found in map
                allReceived = true;
            }

            // TODO: Should remove this tick from the map!!!

            return allReceived;
        }




        /**
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets.
         *
         * @param dataBuf       place to store assembled packets.
         * @param bufLen        byte length of dataBuf.
         * @param udpSocket     UDP socket to read.
         * @param debug         turn debug printout on & off.
         * @param veryFirstRead this is the very first time data will be read for a sequence of same-tick packets.
         * @param last          to be filled with "last" bit id from RE header,
         *                      indicating the last packet in a series used to send data.
         * @param tick          to be filled with tick from RE header.
         * @param expSequence   value-result parameter which gives the next expected sequence to be
         *                      read from RE header and returns its updated value
         *                      indicating its sequence in the flow of packets.
         * @param bytesPerPacket  pointer to int which get filled with the very first packet's data byte length
         *                        (not including header). This gives us an indication of the MTU.
         * @param outOfOrderPackets reference to map that holds out-of-order packets between calls to this function.
         *
         * @return 0 if all last bits found.
         *         1 if a buffer is full.
         *         If there's an error in recvmsg, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         */
        static int getPacketizedBuffers(int udpSocket, et_fifo_id fid, size_t bufSizeMax, uint64_t *finishedTick,
                                        bool debug, bool veryFirstRead, bool *last,
                                           std::unordered_map<uint16_t, uint32_t> & bytesPerPacket,
                                           std::unordered_map<std::pair<uint64_t, uint16_t>, bool> & endCondition,
                                           std::map<uint64_t, et_fifo_entry *> & buffers,
                                           std::unordered_map<std::pair<uint64_t, uint16_t>, uint32_t> & expSequence,
                                           std::unordered_map<std::tuple<uint32_t, uint16_t, uint64_t>,
                                                              std::tuple<std::unique_ptr<std::vector<char>>, bool, bool>>
                                                        & outOfOrderPackets) {

            // TODO: build if sequence is file offset

            uint64_t tick;
            uint32_t sequence, expectedSequence;

            bool packetFirst, packetLast, firstReadForBuf = false, tooLittleRoom = false;

            // Flag to indicate when to return from this routine.
            // Set when any single receiving buffer is full,
            // or when all buffers of the next tick have received the lastPacket.
            bool timeToReturn = false;
            bool lastBitsReceived = false;

            int  version, nBytes;
            uint16_t dataId;
            size_t   maxPacketBytes = 0, remainingLen = bufSizeMax;
            ssize_t  totalBytesWritten = 0;

            // Make this big enough to read a single jumbo packet
            size_t packetBufSize = 10000;
            char packetBuffer[packetBufSize];

            // Buffer to copy data into
            char* buffer;
            char* readDataFrom;
            et_fifo_entry *entry;

            // Key into buffers and expSequence maps and part of value in outOfOrderPackets
            std::pair<uint64_t, uint16_t> key;
            std::vector<char> *vec;


 //           if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu\n", remainingLen);

            while (true) {

                // Read in one packet including reassembly header
                int bytesRead = recvfrom(udpSocket, packetBuffer, packetBufSize, 0,  nullptr, nullptr);
                if (bytesRead < 0) {
                    if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));
                    return(-1);
                }

                // Number of actual data bytes not counting RE header
                nBytes = bytesRead - HEADER_BYTES;
                // Set data source for future copy
                readDataFrom = packetBuffer + HEADER_BYTES;

                parseReHeader(packetBuffer, &version, &packetFirst, &packetLast, &dataId, &sequence, &tick);


                // TODO: What if zero-length packet data??? nBytes = 0?



                // Create the key to both the map of buffers and map of next-expected-sequence
                key = {tick, dataId};

                // Use tick value to look into the map of associated fifo entries
                auto it = buffers.find(tick);

                // If fifo entry for this tick already exists ...
                if (it != buffers.end()) {
                    entry = it->second;
                    et_event* pe = et_fifo_getBuf(dataId, entry);
                    if (pe == NULL) {
                        // Major error
                        throw std::runtime_error("too many source ids for data to be held in fifo entry");
                    }

                    // Find the next expected sequence
                    expectedSequence = expSequence[key];

                    // Get data array
                    buffer = reinterpret_cast<char *>(pe->pdata);

                    // Bytes previously written into buffer
                    totalBytesWritten = pe->length;

                    firstReadForBuf = false;
                    veryFirstRead = false;
                }
                else {
                    // There is no fifo entry for this tick so get one and store in map
                    int err = et_fifo_newEntry(fid, &entry);
                    if (err != ET_OK) {
                        throw std::runtime_error(et_perror(err));
                    }
                    
                    // If it does NOT exist, create it.
                    // First expected sequence is 0
                    expectedSequence = 0;
                    // Put expected seq into map for future access
                    expSequence[key] = 0;
                    // Bytes previously written into buffer
                    totalBytesWritten = 0;

                    // Make a buffer in which to place data for this dataId and tick
                    auto ptr = std::make_unique<std::vector<char>>(bufSizeMax);
                    vec = ptr.get();
                    buffer = ptr->data();
                    // Put it into map for future access
                    buffers.emplace(key, ptr);

                    firstReadForBuf = true;
                    veryFirstRead = true;
                }


                if (sequence == 0) {
                    if (!packetLast) {
                        if (debug) fprintf(stderr, "Expecting first bit to be set on first packet but wasn't\n");
                        return BAD_FIRST_LAST_BIT;
                    }

                    // Each data source may come over a different network/interface and
                    // thus have a different number of bytes per packet. Track it.
                    bytesPerPacket[dataId] = maxPacketBytes = nBytes;
                }
                else if (packetFirst) {
                    if (debug) fprintf(stderr, "Expecting first bit NOT to be set on read but was\n");
                    return BAD_FIRST_LAST_BIT;
                }


                if (debug) fprintf(stderr, "Received %d bytes from sender %hu, tick %llu, in packet #%d, last = %s, firstReadForBuf = %s\n",
                                   nBytes, dataId, tick, sequence, btoa(packetLast), btoa(firstReadForBuf));


                // Check to see if packet is in sequence, if not ...
                if (sequence != expectedSequence) {
                    if (debug) fprintf(stderr, "\n    ID %hu: Got seq %u, expecting %u\n", dataId, sequence, expectedSequence);

                    // If we get a sequence that we already received, ERROR!
                    if (sequence < expectedSequence) {
                        if (debug) fprintf(stderr, "    Already got seq %u once before!\n", sequence);
                        return OUT_OF_ORDER;
                    }

                    // Limit how man out-of-order packets we're going to store (1000 packets) while we wait
                    if (outOfOrderPackets.size() >= 1000) {
                        if (debug) fprintf(stderr, "    Reached limit of 1000 stored packets!\n");
                        return OUT_OF_ORDER;
                    }

                    // Since it's out of order, what was written into packetBuffer
                    // will need to be copied and stored.
                    auto ptr = std::make_unique<std::vector<char>>(nBytes);
                    memcpy(ptr->data(), packetBuffer + HEADER_BYTES, nBytes);
                    ptr->resize(nBytes);

                    if (debug) fprintf(stderr, "    Save and store packet %u, packetLast = %s\n", sequence, btoa(packetLast));

                    // Put it into map. The key of sequence & tick & dataId is unique for each packet
                    outOfOrderPackets.emplace(std::tuple<uint32_t, uint16_t, uint64_t>
                                                {sequence, dataId, tick},
                                              std::tuple<std::unique_ptr<std::vector<char>>, bool, bool>
                                                {std::move(ptr), packetLast, packetFirst});
                    // Read next packet
                    continue;
                }

                while (true) {
                    if (debug) fprintf(stderr, "\nPacket %u in proper order, last = %s\n", sequence, btoa(packetLast));

                    // Packet was in proper order, write it into appropriate buffer

                    // Copy data into buffer
                    memcpy(buffer + totalBytesWritten, readDataFrom, nBytes);
                    // Total bytes written into this buffer
                    totalBytesWritten += nBytes;
                    // Tell vector how many bytes it now contains
                    vec->resize(totalBytesWritten);
                    // Number of bytes left in this buffer
                    remainingLen = bufSizeMax - totalBytesWritten;
                    // Next expected sequence
                    expSequence[key] = ++expectedSequence;

                    if (debug) fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s\n",
                                       remainingLen, expectedSequence, btoa(packetFirst), btoa(packetLast));


                    // Another mtu of data (as reckoned by source) will exceed buffer space, so return
                    if (remainingLen < bytesPerPacket[dataId]) {
                        tooLittleRoom = true;
                        timeToReturn  = true;
                    }

                    // Is this the last packet for a tick and data source?
                    if (packetLast) {
                        // Create key unique for every tick & dataId combo
                        std::pair<uint64_t, uint16_t> kee {tick, dataId};

                        // Store this info so we can look at all data sources with this tick
                        // and figure out if all its data has been collected
                        endCondition[kee] = packetLast;

                        if (allLastBitsReceived(endCondition, tick)) {
                            lastBitsReceived = true;
                            timeToReturn = true;
                        }
                    }

                    if (timeToReturn) {
                        break;
                    }

                    // Since we have room and don't have all the last packets,
                    // check out-of-order packets for this tick and dataId
                    if (!outOfOrderPackets.empty()) {
                        if (debug) fprintf(stderr, "We have stored packets\n");

                        // Create key (unique for every packet)
                       std::tuple<uint32_t, uint16_t, uint64_t> kee {expectedSequence, dataId, tick};

                        // Use key to look into the map
                        auto iter = outOfOrderPackets.find(kee);

                        // If packet of interest exists ...
                        if (it != buffers.end()) {
                            // Get info
                            auto & dataPtr = std::get<0>(iter->second);
                            packetLast     = std::get<1>(iter->second);
                            packetFirst    = std::get<2>(iter->second);
                            sequence       = expectedSequence;
                            vec            = dataPtr.get();
                            nBytes         = dataPtr->size();
                            readDataFrom   = dataPtr->data();

                            // Remove packet from map
                            iter = outOfOrderPackets.erase(iter);
                            if (debug) fprintf(stderr, "Go and add stored packet %u, size of map = %lu, last = %s\n",
                                               expectedSequence, outOfOrderPackets.size(), btoa(packetLast));

                            // Write this packet into main buffer now
                            continue;
                        }
                    }

                    break;
                }

                if (timeToReturn) {
                    break;
                }

                // read next packet
            }

            if (lastBitsReceived) {
                // TODO: Clean up if tick finished!!!! Perhaps better done is calling routine.
                *finishedTick = tick;
                return 0;
            }

            //if (debug) fprintf(stderr, "getPacketizedBuffer: passing offset = %u\n\n", expectedSequence);
            return 1;
        }







        /**
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets.
         *
         * @param userBuf       address of pointer to data buffer if noCopy is true.
         *                      Otherwise, this is used to return a locally allocated data buffer.
         * @param userBufLen    pointer to byte length of dataBuf if noCopy is true.
         *                      Otherwise it returns the size of the data buffer returned.
         * @param port          UDP port to read on.
         * @param listeningAddr if specified, this is the IP address to listen on (dot-decimal form).
         * @param noCopy        If true, write data directly into userBuf. If there's not enough room, an error is thrown.
         *                      If false, an internal buffer is allocated and returned in the userBuf arg.
         * @param debug         turn debug printout on & off.
         *
         * @return 0 if success.
         *         If there's an error in recvmsg, it will return RECV_MSG.
         *         If the packet data is NOT completely read (truncated), it will return TRUNCATED_MSG.
         *         If the buffer is too small to receive a single packet's data, it will return BUF_TOO_SMALL.
         *         If a packet is out of order and no recovery is possible (e.g. duplicate sequence),
         *              it will return OUT_OF_ORDER.
         *         If a packet has improper value for first or last bit, it will return BAD_FIRST_LAST_BIT.
         *         If cannot allocate memory, it will return OUT_OF_MEM.
         *         If userBuf is null or *userBuf is null when noCopy is true, it will return BAD_ARG.
         */
        static int getBuffers(uint16_t port, const char* listeningAddr, bool debug, uint64_t* bufTick,
                              et_fifo_id fid)
        {

            port = port < 1024 ? 7777 : port;

            // Create UDP socket
            int udpSocket = socket(PF_INET, SOCK_DGRAM, 0);

            // Configure settings in address struct
            struct sockaddr_in serverAddr{};
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(port);
            if (listeningAddr != nullptr && strlen(listeningAddr) > 0) {
                serverAddr.sin_addr.s_addr = inet_addr(listeningAddr);
            }
            else {
                serverAddr.sin_addr.s_addr = INADDR_ANY;
            }
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            // Bind socket with address struct
            int err = bind(udpSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
            if (err != 0) {
                // TODO: handle error properly
                if (debug) fprintf(stderr, "bind socket error\n");
            }


            ssize_t nBytes, totalBytes = 0;
            bool last, firstRead = true;
            // Start with sequence 0 in very first packet to be read
            uint32_t sequence = 0;
            uint64_t tick = 0;

            //------------------------------------------------
            // Maps used to store quantities while sorting
            //------------------------------------------------

            // Map to hold the ET fifo entry (multiple bufs) for each tick:
            //     key   = tick
            //     value = array of ET events (buffers)
           std::map<uint64_t, et_fifo_entry *> bufStore;

            // Map to hold out-of-order packets:
            //     key   = tuple of {sequence, dataId, tick} (unique for each packet)
            //     value = tuple of {smart ptr to vector of packet data bytes,
            //                       is last packet, is first packet}
            std::unordered_map<std::tuple<uint32_t, uint16_t, uint64_t>,
                               std::tuple<std::unique_ptr<std::vector<char>>, bool, bool>> outOfOrderPackets;

            // Map to hold the max bytes per packet for each data source:
            //     key   = data source id
            //     value = max bytes per packet
            std::unordered_map<uint16_t, uint32_t> bytesPerPacket;

            // Map to hold the status of the "last" packet bit for each tick-dataId combo:
            //     key   = pair of {tick, dataId}
            //     value = true if last bit received, else false
            std::unordered_map<std::pair<uint64_t, uint16_t>, bool> endCondition;

            // Map to hold the next expected sequence for each tick-dataId combo:
            //     key   = pair of {tick, dataId}
            //     value = next expected sequence
            std::unordered_map<std::pair<uint64_t, uint16_t>, uint32_t> expSequence;

            //------------------------------------------------


            // We're using our own internally allocated buffers.
            // We may have to loop through to read everything, since we don't know how much data is coming
            // Create an internal buffer that we'll fill and return.
            // If it's too small, it will be reallocated and data copied over.

            // Initial size to make internal buffers
            size_t dataBufLen = 100000;


            while (true) {
                err = getPacketizedBuffers(udpSocket, fid, dataBufLen, &tick,
                                           debug, firstRead, &last,
                                           bytesPerPacket, endCondition,
                                           bufStore, expSequence,
                                           outOfOrderPackets);
                if (err < 0) {
                    if (debug) fprintf(stderr, "Error in getPacketizerBuffer, %ld\n", nBytes);
                    // Return the error
                    return err;
                }
                else if (err == 0) {
                    // Got all last bits
                    // Pull out all buffers associated with "tick" and return to caller
                    et_fifo_entry *pentry;
                    for (auto & entry : bufStore) {
                        // ignore other ticks
                        if (entry.first != tick) {
                            continue;
                        }

                        // Put fifo entry (buf array) back into ET
                        pentry = entry.second;
                        et_fifo_putEntry(pentry);
                    }
                    *bufTick = tick;
                    return 0;
                }
                else if (err == 1) {
                    // a particular buffer was too small

                }

                totalBytes += nBytes;
                getDataFrom += nBytes;
                remaningBytes -= nBytes;
                firstRead = false;

                //printBytes(dataBuf, nBytes, "buffer ---->");

                if (last) {
                    if (debug) fprintf(stderr, "Read last packet from incoming data, quit\n");
                    break;
                }

                // Do we have to allocate more memory? Double it if so.
                if (remaningBytes < bytesPerPacket) {
                    dataBuf = (char *)realloc(dataBuf, 2*dataBufLen);
                    if (dataBuf == nullptr) {
                        return OUT_OF_MEM;
                    }
                    getDataFrom = dataBuf + totalBytes;
                    dataBufLen *= 2;
                    remaningBytes = dataBufLen - totalBytes;
                    if (debug) fprintf(stderr, "Reallocate buffer to %u bytes\n", dataBufLen);
                }

                if (debug) fprintf(stderr, "Read %ld bytes from incoming reassembled packet\n", nBytes);
            }

            *userBuf = dataBuf;
            *userBufLen = totalBytes;


            if (debug) fprintf(stderr, "Read %ld incoming data bytes\n", totalBytes);

            return 0;
        }

    }
}


#endif // EJFAT_ASSEMBLE_ERSAP2_H
