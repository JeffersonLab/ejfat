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
 * <p>
 * The {@link #getBuffers} routine accepts input from multiple data sources and multiple
 * ticks, assembles them into 1 buffer for each tick/id combo and writes them out for
 * users to access.
 * </p>
 * This routine accesses (read and writes) buffers from an ET system through a new API which
 * presents the ET system as a FIFO. By using the ET system, the reading of packets in this
 * frontend is decoupled from users on the backend that process the data.0
 */
#ifndef EJFAT_ASSEMBLE_ERSAP_ET_H
#define EJFAT_ASSEMBLE_ERSAP_ET_H


#include <set>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <memory>
#include <utility>
#include <string>

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
         * {@link #getBuffers} already checked validity of data id info
         * passed in through bufIds.
         * When all last bits are found for a tick, all the entries
         * associated with that tick in the endCondition map are removed.
         *
         * @param tick         tick value to examine.
         * @param bufIds       set with all data id values in it.
         * @param endCondition map with all last bit data in it.
         * @return return true if all data sources for given tick have set the "last" bit,
         *         else false.
         */
        static bool allLastBitsReceived(uint64_t tick, std::set<int> &bufIds,
                                        std::map<std::pair<uint64_t, uint16_t>, bool> & endCondition)
        {
            int idCount = bufIds.size();

            fprintf(stderr, "\nallLastBitsReceived: id count = %d, endCondition size = %lu\n",
                               idCount, endCondition.size());

            // There must be one end condition for each id
            if (endCondition.size() != idCount) {
                fprintf(stderr, "  too few sources reporting an end\n");
                return false;
            }

            // Look at all data sources for a single tick,
            // to see if the last bit has been set for each.
            // If so, return true, else return false.
            for (const auto & i : endCondition) {
                // ignore other ticks
                if (i.first.first != tick) {
                    continue;
                }

                // if dataId not contained in vector of ids
                if (bufIds.count(i.first.second == 0)) {
                    return false;
                }

                // if last bit not set
                if (!i.second) {
                    return false;
                }
            }


            // Since we're done with this tick, remove it from the map
            for (auto it = endCondition.cbegin(); it != endCondition.cend(); ) {
                if (it->first.first == tick) {
                    endCondition.erase(it++);    // or "it = m.erase(it)" since C++11
                }
                else {
                    ++it;
                }
            }

            fprintf(stderr, "    id count = %d, endCondition size = %lu, return all bits found for tick %llu\n",
                    idCount, endCondition.size(), tick);
            return true;
        }


        /**
         * Assemble incoming packets into the given buffer.
         * It will return when the buffer has less space left than it read from the first packet
         * or when the "last" bit is set in a packet.
         * This routine allows for out-of-order packets.
         *
         * @param udpSocket     socket on which to read UDP packets.
         *                      Otherwise, this is used to return a locally allocated data buffer.
         * @param fid           id for using ET system configured as FIFO.
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
        static int getBuffers(int udpSocket, et_fifo_id fid, bool debug)
        {

            // Make this big enough to read a single jumbo packet
            size_t packetBufSize = 10000;
            char packetBuffer[packetBufSize];

            // TODO: build if sequence is file offset

            uint64_t tick;
            uint32_t sequence, expectedSequence;
            bool packetFirst, packetLast, firstReadForBuf;

            // Set true when all buffers of a tick have received the lastPacket
            bool tickCompleted;

            // Initial size to make internal buffers
            size_t   bufSizeMax = et_fifo_getBufSize(fid);
            size_t   remainingLen = bufSizeMax, totalBytesWritten = 0;

            int  version, nBytes;
            uint16_t dataId;

            // Buffer to copy data into
            char* buffer;
            char* readDataFrom;
            et_event *event;
            et_fifo_entry *entry;

            // Key into buffers and expSequence maps and part of value in outOfOrderPackets
            std::pair<uint64_t, uint16_t> key;

            //------------------------------------------------
            // Maps used to store quantities while sorting.
            // Note: most of these could be unordered_maps but,
            // unfortunately, pair and tuples cannot be used as
            // keys in that case.
            //------------------------------------------------

            // Map to hold the ET fifo entry (multiple bufs) for each tick:
            //     key   = tick
            //     value = array of ET events (buffers)
            std::map<uint64_t, et_fifo_entry *> buffers;

            // Map to hold out-of-order packets:
            //     key   = tuple of {sequence, dataId, tick} (unique for each packet)
            //     value = tuple of {smart ptr to vector of packet data bytes,
            //                       is last packet, is first packet}
            std::map<std::tuple<uint32_t, uint16_t, uint64_t>,
                    std::tuple<std::unique_ptr<std::vector<char>>, bool, bool>> outOfOrderPackets;

            // Map to hold the max bytes per packet for each data source:
            //     key   = data source id
            //     value = max bytes per packet
            std::map<uint16_t, uint32_t> bytesPerPacket;

            // Map to hold the status of the "last" packet bit for each tick-dataId combo:
            //     key   = pair of {tick, dataId}
            //     value = true if last bit received, else false
            std::map<std::pair<uint64_t, uint16_t>, bool> endCondition;

            // Map to hold the next expected sequence for each tick-dataId combo:
            //     key   = pair of {tick, dataId}
            //     value = next expected sequence
            std::map<std::pair<uint64_t, uint16_t>, uint32_t> expSequence;

            //------------------------------------------------
            // We're using buffers created in the ET system -
            // one for each tick and dataId combo.
            // A fifo contains a group of buffers/events for 1 tick.
            //-----------------------------------------------

            //-----------------------------------------------
            // We will have to keep looping to read everything,
            // since we don't know how much data is coming or in what order.
            //-----------------------------------------------

            //-----------------------------------------------
            // We must check, before starting, to see if the et_fifo_openProducer()
            // was called with all source id's.
            // Only then we can figure out if we have a last-bit set from each
            // data source. Otherwise we're out of luck.
            // WE NEED TO KNOW FROM WHOM WE'RE EXPECTING DATA!
            //-----------------------------------------------
            int idCount = et_fifo_getIdCount(fid);
            if (debug) fprintf(stderr, "found data sources count = %d\n", idCount);
            if (idCount < 1) {
                if (debug) fprintf(stderr, "data sources were NOT specified when calling et_fifo_openProducer()!\n");
                return(-1);
            }

            int bufIds[idCount];
            int err = et_fifo_getBufIds(fid, bufIds);
            if (err != ET_OK) {
                if (debug) fprintf(stderr, "data sources were NOT specified when calling et_fifo_openProducer()!\n");
                return(-1);
            }

            // Translate this array into an ordered set for later ease of use
            std::set<int> bufferIds;
            for (auto id : bufIds) {
                bufferIds.insert(id);
            }

            while (true) {

                // TODO: build if sequence is file offset

                // Set true when all buffers of a tick have received the lastPacket
                tickCompleted = false;

                if (debug) fprintf(stderr, "getPacketizedBuffer: remainingLen = %lu\n", remainingLen);

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
                    if (debug) {
                        fprintf(stderr, "\n\nPkt hdr: ver = %d, first = %s, last = %s, dataId = %hu, seq = %u, tick = %llu, nBytes = %d\n",
                                version, btoa(packetFirst), btoa(packetLast), dataId, sequence, tick, nBytes);
                    }

                    // Create the key to both the map of buffers and map of next-expected-sequence
                    key = {tick, dataId};

                    // Use tick value to look into the map of associated fifo entries
                    auto it = buffers.find(tick);

                    // If fifo entry for this tick already exists ...
                    if (it != buffers.end()) {
if (debug) fprintf(stderr, "fifo entry already exists, look for id = %hu\n", dataId);
                        entry = it->second;
                        // Find the buffer associated with dataId or the first unused
                        event = et_fifo_getBuf(dataId, entry);
                        if (event == NULL) {
                            // Major error
                            throw std::runtime_error("too many source ids for data to be held in fifo entry");
                        }
 if (debug) fprintf(stderr, "  ev len = %llu, id = %d, hasData = %d\n", event->length, event->control[0], event->control[1]);

                        // Find the next expected sequence
                        expectedSequence = expSequence[{tick, dataId}];

                        // Get data array
                        buffer = reinterpret_cast<char *>(event->pdata);

                        // Bytes previously written into buffer
                        totalBytesWritten = event->length;

                        // Room for packet?
                        if (totalBytesWritten + nBytes > bufSizeMax) {
                            // No room in buffer, ET system event size needs to be changed to accommodate this!
                            throw std::runtime_error("ET event too small, make > " +
                                                     std::to_string(totalBytesWritten + nBytes) + " bytes");
                        }

                        firstReadForBuf = false;
                    }
                    else {
if (debug) fprintf(stderr, "fifo entry must be created\n");
                        entry = et_fifo_entryCreate(fid);
                        if (entry == nullptr) {
                            throw std::runtime_error("no memory");
                        }

                        // There is no fifo entry for this tick, so get one and store in map
                        err = et_fifo_newEntry(fid, entry);
                        if (err != ET_OK) {
                            throw std::runtime_error(et_perror(err));
                        }

                        // Put fifo entry into map for future access
                        buffers[tick] = entry;

                        // Get data array, assume room for 1 packet
                        event = et_fifo_getBuf(dataId, entry);
                        if (event == NULL) {
                            throw std::runtime_error("cannot find buf w/ id = " + std::to_string(dataId));
                        }

                        buffer = reinterpret_cast<char *>(event->pdata);

                        // First expected sequence is 0
                        expectedSequence = 0;
                        // Put expected seq into map for future access
                        expSequence[key] = 0;
                        // Bytes previously written into buffer
                        totalBytesWritten = 0;

                        firstReadForBuf = true;
                    }

                    if (sequence == 0) {
                        if (!packetFirst) {
                            if (debug) fprintf(stderr, "Expecting first bit to be set on first packet but wasn't\n");
                            return BAD_FIRST_LAST_BIT;
                        }

                        // Each data source may come over a different network/interface and
                        // thus have a different number of bytes per packet. Track it.
                        // If small payload (< MTU), then this is irrelevant, but save anyway.
                        bytesPerPacket[dataId] = nBytes;
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

                        if (debug) fprintf(stderr, "    Save and store packet %u, packetLast = %s, storage has %lu\n",
                                           sequence, btoa(packetLast), outOfOrderPackets.size());

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
                        // Tell event how many bytes it now contains
                        et_event_setlength(event, totalBytesWritten);
                        // Remember that this event has data
                        et_fifo_setHasData(event, 1);
                        // Number of bytes left in this buffer
                        remainingLen = bufSizeMax - totalBytesWritten;
                        // Next expected sequence
                        expSequence[key] = ++expectedSequence;

                        if (debug) fprintf(stderr, "remainingLen = %lu, expected offset = %u, first = %s, last = %s\n",
                                           remainingLen, expectedSequence, btoa(packetFirst), btoa(packetLast));

                        // Is this the last packet for a tick and data source?
                        if (packetLast) {
                            // Create key unique for every tick & dataId combo
                            std::pair<uint64_t, uint16_t> kee {tick, dataId};

                            // Store this info so we can look at all data sources with this tick
                            // and figure out if all its data has been collected
                            endCondition[kee] = packetLast;

                            if (allLastBitsReceived(tick, bufferIds, endCondition)) {
                                tickCompleted = true;
                            }
                        }

                        if (tickCompleted) {
                            break;
                        }

                        // Since we have room and don't have all the last packets,
                        // check out-of-order packets for this tick and dataId
                        if (!outOfOrderPackets.empty()) {
                            if (debug) fprintf(stderr, "We have stored packets, look for exp seq = %u, id = %hu, tick = %llu\n",
                                               expectedSequence, dataId, tick);

                            // Create key (unique for every packet)
                            std::tuple<uint32_t, uint16_t, uint64_t> kee {expectedSequence, dataId, tick};

                            // Use key to look into the map
                            auto iter = outOfOrderPackets.find(kee);

                            // If packet of interest exists ...
                            if (iter != outOfOrderPackets.end()) {
                                // Get info
                                auto & dataPtr = std::get<0>(iter->second);
                                packetLast     = std::get<1>(iter->second);
                                packetFirst    = std::get<2>(iter->second);
                                sequence       = expectedSequence;
                                nBytes         = dataPtr->size();
                                readDataFrom   = dataPtr->data();

                                // Remove packet from map
                                outOfOrderPackets.erase(iter);
                                if (debug) fprintf(stderr, "Go and add stored packet %u, size of map = %lu, last = %s\n",
                                                   expectedSequence, outOfOrderPackets.size(), btoa(packetLast));

                                // Room for packet?
                                if (totalBytesWritten + nBytes > bufSizeMax) {
                                    // No room in buffer, ET system event size needs to be changed to accommodate this!
                                    throw std::runtime_error("ET event too small, make > " +
                                                             std::to_string(totalBytesWritten + nBytes) + " bytes");
                                }

                                // Write this packet into main buffer now
                                continue;
                            }
                        }

                        break;
                    }

                    if (tickCompleted) {
                        break;
                    }

                    // read next packet
                }

                // Put complete array of buffers associated w/ one tick back into ET
                et_fifo_putEntry(entry);

                // Work on the next tick
            }
            return 0;
        }



    }
}



#endif // EJFAT_ASSEMBLE_ERSAP_ET_H
