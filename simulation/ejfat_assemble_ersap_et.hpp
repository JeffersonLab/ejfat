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
#include <cinttypes>

#include "et.h"
#include "et_fifo.h"


#include "ejfat_assemble_ersap.hpp"


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

//            fprintf(stderr, "\nallLastBitsReceived: id count = %d, endCondition size = %lu\n",
//                               idCount, endCondition.size());

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

//            fprintf(stderr, "    id count = %d, endCondition size = %lu, return all bits found for tick %" PRIu64 "\n",
//                    idCount, endCondition.size(), tick);
            return true;
        }


        /**
         * Look at all data sources for a single tick
         * to see if the last bit has been set for each.
         * Return the percentage of last bits set (0 - 100).
         * {@link #getBuffers} already checked validity of data id info
         * passed in through bufIds.
         * When all last bits are found for a tick, all the entries
         * associated with that tick in the endCondition map are removed.
         *
         * @param tick         tick value to examine.
         * @param bufIds       set with all data id values in it.
         * @param endCondition map with all last bit data in it.
         * @return percent (0 - 100) of data sources for given tick which have set the "last" bit, or -1 if error.
         */
        static int percentLastBitsReceived(uint64_t tick, std::set<int> &bufIds,
                                        std::map<std::pair<uint64_t, uint16_t>, bool> & endCondition)
        {
            int idCount = bufIds.size();
            int endCount = 0, percent;
            bool allHere = true;

//            fprintf(stderr, "\npercentLastBitsReceived: id count = %d, endCondition size = %lu\n",
//                    idCount, endCondition.size());

            // There must be one end condition for each id
            if (endCondition.size() != idCount) {
                fprintf(stderr, "  too few sources reporting an end\n");
                return -1;
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
                    allHere = false;
                    continue;
                }

                // if last bit not set
                if (!i.second) {
                    allHere = false;
                    continue;
                }

                endCount++;
            }

            if (!allHere) {
                percent = 100 * endCount / idCount;
                return percent;
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

//            fprintf(stderr, "    id count = %d, endCondition size = %lu, return all bits found for tick %" PRIu64 "\n",
//                    idCount, endCondition.size(), tick);
            return 100;
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
         * @param stats         shared pointer to map, map elements are shared pointer to stats structure.
         *                      Use this to keep stats so it can be printed out somewhere.
         *
         * @throws  runtime_exception if ET buffer too small,
         *                            too many source ids to be held in fifo entry,
         *                            error in recvmsg,
         *                            no memory available,
         *                            error talking to ET system,
         *                            cannot find correct buffer in fifo entry,
         *                            data sources were NOT specified when calling et_fifo_openProducer(),
         */
        static void getBuffers(int udpSocket, et_fifo_id fid, bool debug,
                               std::shared_ptr<std::unordered_map<int, std::shared_ptr<packetRecvStats>>> stats)
        {
            // Do we bother to keep stats or not
            bool takeStats = stats != nullptr;
            std::unordered_map<int, std::shared_ptr<packetRecvStats>> statMap;
            if (takeStats) {
                // map with key = src id, val = pointer to struct for statistics
                statMap = *stats;
            }

            // Make this big enough to read a single jumbo packet
            size_t packetBufSize = 9100;
            char packetBuffer[packetBufSize];

            uint64_t tick;
            uint32_t bufLen, bufOffset;
            bool packetFirst, packetLast, firstReadForBuf;

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
            // array to hold event's control words
            int con[ET_STATION_SELECT_INTS];

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

            // Map to hold the max bytes per packet for each data source:
            //     key   = data source id
            //     value = max bytes per packet
            std::map<uint16_t, uint32_t> bytesPerPacket;

            // Map to hold the status of the "last" packet bit for each tick-dataId combo:
            //     key   = pair of {tick, dataId}
            //     value = true if last bit received, else false
            std::map<std::pair<uint64_t, uint16_t>, bool> endCondition;

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
            int srcIdCount = et_fifo_getIdCount(fid);
            if (debug) fprintf(stderr, "found data sources count = %d\n", srcIdCount);
            if (srcIdCount < 1) {
                throw std::runtime_error("data sources were NOT specified when calling et_fifo_openProducer()");
            }

            int bufIds[srcIdCount];
            int err = et_fifo_getBufIds(fid, bufIds);
            if (err < 0) {
                throw std::runtime_error("data sources were NOT specified when calling et_fifo_openProducer()");
            }

            // Translate this array into an ordered set for later ease of use
            std::set<int> srcIds;
            for (auto id : bufIds) {
                srcIds.insert(id);
            }

            while (true) {

                // Set true when all buffers of a tick have received the lastPacket
                bool tickCompleted = false;

                if (debug) fprintf(stderr, "getBuffers: remainingLen = %lu\n", remainingLen);

                while (true) {

                    // Read in one packet including reassembly header
                    int bytesRead = recvfrom(udpSocket, packetBuffer, packetBufSize, 0,  nullptr, nullptr);
                    if (bytesRead < 0) {
                        if (debug) fprintf(stderr, "recvmsg() failed: %s\n", strerror(errno));
                        throw std::runtime_error("recvmsg failed");
                    }

                    // Number of actual data bytes not counting RE header
                    nBytes = bytesRead - HEADER_BYTES;
                    // Set data source for future copy
                    readDataFrom = packetBuffer + HEADER_BYTES;

                    parseReHeader(packetBuffer, &version, &dataId, &bufOffset, &bufLen, &tick);
                    if (debug) {
                        fprintf(stderr, "\n\nPkt hdr: ver = %d, dataId = %hu, offset = %u, len = %u, tick = %" PRIu64 ", nBytes = %d\n",
                                version, dataId, bufOffset, bufLen, tick, nBytes);
                    }

                    // Track # packets read while assembling each buffer
                    int64_t packetCount;

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
 if (debug) fprintf(stderr, "  ev len = %" PRIu64 ", id = %d, hasData = %d\n", event->length, event->control[0], event->control[1]);

                        // Keep # of packets built into buffer, stored in the 6th ET control word of event.
                        // For fifo ET system, the 1st control word is used by ET system to contain srcId,
                        // the 2nd holds whether data is valid or not. The rest are available.
                        et_event_getcontrol(event, con);
                        packetCount = con[5];

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

                        // Have we read the whole buffer?
                        if (totalBytesWritten + nBytes >= bufLen) {
                            packetLast = true;
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

                       // Bytes previously written into buffer
                        totalBytesWritten = 0;
                        packetLast = false;
                        firstReadForBuf = true;
                        packetCount = 0;
                    }

                    if (bufOffset == 0) {
                        // Each data source may come over a different network/interface and
                        // thus have a different number of bytes per packet. Track it.
                        // If small payload (< MTU), then this is irrelevant, but save anyway.
                        bytesPerPacket[dataId] = nBytes;
                    }

                    if (debug) fprintf(stderr, "Received %d bytes from sender %hu, tick %" PRIu64 ", w/ offset %u, last = %s, firstReadForBuf = %s\n",
                                       nBytes, dataId, tick, bufOffset, btoa(packetLast), btoa(firstReadForBuf));

                    // Copy data into buffer
                    memcpy(buffer + bufOffset, readDataFrom, nBytes);
                    // Total bytes written into this buffer
                    totalBytesWritten += nBytes;
                    // Tell event how many bytes it now contains
                    et_event_setlength(event, totalBytesWritten);
                    // Remember that this event has data
                    et_fifo_setHasData(event, 1);
                    // Number of bytes left in this buffer
                    remainingLen = bufSizeMax - totalBytesWritten;

                    // record # packets for this buffer/event
                    con[5] = ++packetCount;
                    et_event_setcontrol(event, con, 6);

                    if (debug) fprintf(stderr, "remainingLen = %lu, offset = %u, first = %s, last = %s\n",
                                       remainingLen, bufOffset, btoa(bufOffset == 0), btoa(packetLast));

                    // Is this the last packet for a tick and data source?
                    if (packetLast) {
                        // Create key unique for every tick & dataId combo
                        std::pair<uint64_t, uint16_t> key {tick, dataId};

                        // Store this info so we can look at all data sources with this tick
                        // and figure out if all its data has been collected
                        endCondition[key] = true;

                        if (allLastBitsReceived(tick, srcIds, endCondition)) {
                            tickCompleted = true;
                        }
                    }

                    if (tickCompleted) {
                        if (takeStats) {
                            statMap[dataId]->acceptedBytes += totalBytesWritten;
                            statMap[dataId]->builtBuffers++;
                            statMap[dataId]->acceptedPackets += packetCount;
                        }

                        break;
                    }

                    // TODO: WHat do we do when we drop a packet and get stuck trying to finish creating a buffer?
                    // Need to find a way of labeling a buffer as having incomplete data.
                    // One way is to set first control word with:
                    // et_fifo_setHasData(event, false);


                    // read next packet
                }

                // Put complete array of buffers associated w/ one tick back into ET
                et_fifo_putEntry(entry);

                // Work on the next tick
            }
        }



    }



#endif // EJFAT_ASSEMBLE_ERSAP_ET_H
