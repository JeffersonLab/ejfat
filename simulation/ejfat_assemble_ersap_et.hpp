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
#include <unordered_set>
#include <algorithm>
#include <vector>
#include <memory>
#include <utility>
#include <string>
#include <cinttypes>
#include <iostream>

#include "et.h"
#include "et_fifo.h"


#include "ejfat_assemble_ersap.hpp"


    namespace ejfat {

        //-----------------------------------------------------------------------
        // Be sure to print to stderr as programs may pipe data to stdout!!!
        //-----------------------------------------------------------------------



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
         * @param tickPrescale  the prescale value (event Nth) expected of incoming ticks.
         * @param stats         shared pointer to map, map elements are shared pointer to stats structure.
         *                      Use this to keep stats so it can be printed out somewhere.
         *                      Map has key = src id, val = pointer to struct for statistics.
         *
         * @throws  runtime_exception if ET buffer too small,
         *                            too many source ids to be held in fifo entry,
         *                            error in recvmsg,
         *                            no memory available,
         *                            error talking to ET system,
         *                            cannot find correct buffer in fifo entry,
         *                            data sources were NOT specified when calling et_fifo_openProducer(),
         */
        static void getBuffers(int udpSocket, et_fifo_id fid, bool debug, int tickPrescale,
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

            uint64_t tick, prevTick = UINT64_MAX, biggestTick = 0;
            uint32_t bufLen, bufOffset;
            bool packetFirst, packetLast, firstReadForBuf;

            // Initial size to make internal buffers
            size_t   bufSizeMax = et_fifo_getBufSize(fid);
            size_t   remainingLen, totalBytesWritten = 0;

            int  version, nBytes, invalidPkts=0;
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

            // Map to hold the ET fifo entry (multiple bufs) for each tick.
            // There is one buffer (et_event) for each dataId.
            //     key   = tick
            //     value = array of ET events (buffers)
            std::unordered_map<uint64_t, et_fifo_entry *> buffers;

            //------------------------------------------------
            // We're using buffers created in the ET system -
            // one for each tick and dataId combo.
            // A fifo contains a group of buffers/events for 1 tick.
            //
            // We will have to keep looping to read everything,
            // since we don't know how much data is coming or in what order.
            //
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

            // Translate this array into an ordered (sorted by key) set for later ease of use
            std::set<int> srcIds;
            for (auto id : bufIds) {
                srcIds.insert(id);
            }

            // Have a list of 10 empty entries for use
            std::unordered_set<et_fifo_entry *> freeEntries;
            for (int i=0; i < 10; i++) {
                entry = et_fifo_entryCreate(fid);
                if (entry == nullptr) {
                    throw std::runtime_error("no memory");
                }
                freeEntries.insert(entry);
            }



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

                // Check to see if source id is expected
                if (srcIds.count(dataId) == 0) {
                    if (++invalidPkts > 100) {
                        throw std::runtime_error("received over 100 pkts w/ wrong data id");
                    }
                    // Ignore pkt and go to next
                    continue;
                }

                // Track # packets read while assembling each buffer
                int64_t packetCount;

                if (tick != prevTick) {
                    // Packet with different tick than last time came in

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
                        if (debug)
                            fprintf(stderr, "  ev len = %" PRIu64 ", id = %d, hasData = %d\n", event->length,
                                    event->control[0], event->control[1]);

                        // Keep # of packets built into buffer, stored in the 6th ET control word of event.
                        // For fifo ET system, the 1st control word is used by ET system to contain srcId,
                        // the 2nd holds whether data is valid or not. The rest are available.
                        et_event_getcontrol(event, con);
                        packetCount = con[5];

                        // Get data array
                        et_event_getdata(event, (void **) &buffer);

                        // Bytes previously written into buffer
                        et_event_getlength(event, &totalBytesWritten);
                        firstReadForBuf = false;
                    }
                    else {
                        auto itt = freeEntries.cbegin();
                        if (itt == freeEntries.cend()) {
                            // There is no free fifo entry available, so make another one
                            fprintf(stderr, "fifo entry must be created\n");
                            et_fifo_entry *e = et_fifo_entryCreate(fid);
                            if (e == nullptr) {
                                throw std::runtime_error("out of memory");
                            }
                            entry = e;
                        }
                        else {
                            entry = *itt;
                            // remove from set while being used
                            freeEntries.erase(itt);
                        }

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

                        et_event_getdata(event, (void **) &buffer);

                        // Keep # of packets built into buffer, stored in the 6th ET control word of event.
                        et_event_getcontrol(event, con);
                        packetCount = 0;

                        // Bytes previously written into buffer
                        totalBytesWritten = 0;
                        firstReadForBuf = true;

                        // Track the biggest tick to be received from any source
                        if (tick > biggestTick) {
                            //std::cout << "biggest tick " << tick << std::endl;
                            biggestTick = tick;
                        }
                    }
                }
                else {
                    // Same tick as last time, so use same entry.

                    // Find the buffer associated with dataId or the first unused
                    event = et_fifo_getBuf(dataId, entry);
                    if (event == NULL) {
                        // Major error
                        throw std::runtime_error("too many source ids for data to be held in fifo entry");
                    }
//                    if (debug)
//                        fprintf(stderr, "  ev len = %" PRIu64 ", id = %d, hasData = %d\n", event->length,
//                                event->control[0], event->control[1]);

                    et_event_getcontrol(event, con);
                    packetCount = con[5];

                    // Get data array
                    et_event_getdata(event, (void **) &buffer);

                    // Bytes previously written into buffer
                    et_event_getlength(event, &totalBytesWritten);
                    firstReadForBuf = false;
                }

                prevTick = tick;

                if (debug) fprintf(stderr, "Received %d bytes from sender %hu, tick %" PRIu64 ", w/ offset %u, last = %s, firstReadForBuf = %s\n",
                                   nBytes, dataId, tick, bufOffset, btoa(packetLast), btoa(firstReadForBuf));

                // Room for packet?
                if (bufLen > bufSizeMax) {
                    // No room in buffer, ET system event size needs to be changed to accommodate this!
                    throw std::runtime_error("ET event too small, make > " +
                                             std::to_string(bufLen) + " bytes");
                }

                // Copy data into buffer
                memcpy(buffer + bufOffset, readDataFrom, nBytes);

                // Total bytes written into this buffer
                totalBytesWritten += nBytes;

                // Tell event how many bytes it now contains
                et_event_setlength(event, totalBytesWritten);

                // record # packets for this buffer/event
                con[5] = ++packetCount;
                et_event_setcontrol(event, con, 6);

                // Have we read the whole buffer?
                packetLast = false;
                if (totalBytesWritten >= bufLen) {
                    packetLast = true;
                }

                if (debug) {
                    // Number of bytes left in this buffer
                    remainingLen = bufSizeMax - totalBytesWritten;

                    fprintf(stderr, "remainingLen = %lu, offset = %u, first = %s, last = %s\n",
                                   remainingLen, bufOffset, btoa(bufOffset == 0), btoa(packetLast));
                }

                // Is this the last packet for a tick and data source?
                if (packetLast) {
                    // Store in event that buffer is now fully assembled
                    et_fifo_setHasData(event, 1);

                    // Has all data in every buffer of this entry been collected?
                    bool tickCompleted = false;
                    if (et_fifo_allHaveData(fid, entry, nullptr, nullptr)) {
                        tickCompleted = true;
                    }

                    if (tickCompleted) {
                        if (takeStats) {
                            // Each tick has a component from each incoming data source - update all.
                            et_event **evs = et_fifo_getBufs(entry);
                            for (int i=0; i < srcIdCount; i++) {
                                int id = bufIds[i];
                                et_event *ev = evs[i];
                                et_event_getcontrol(ev, con);
                                et_event_getlength(ev, &totalBytesWritten);

                                statMap[id]->acceptedBytes += totalBytesWritten;
                                statMap[id]->builtBuffers++;
                                statMap[id]->acceptedPackets += con[5];
                            }
                        }

                        // Take this out of buffers map
                        buffers.erase(tick);

                        // Put complete array of buffers associated w/ one tick back into ET
                        et_fifo_putEntry(entry);

                        // Put entry back into freeEntries for reuse
                        freeEntries.insert(entry);
                    }
                }

                // Do some house cleaning at this point.
                // There may be missing packets which have kept some ticks from some sources
                // from being completely reassembled and need to cleared out.
                // Take the biggest tick received and place all existing ticks
                // less than it by 2*tickPrescale and still being constructed, back into the ET system.
                // Each of the buffers in such a fifo entry will be individually labelled as
                // having valid data or not. Thus the reader of such a fifo entry will be
                // able to tell if a buffer was not reassembled properly.


                // Using the iterator and the "erase" method, as shown,
                // will avoid problems invalidating the iterator
                for (auto iter = buffers.cbegin(); iter != buffers.cend();) {

                    uint64_t tik = iter->first;
                    et_fifo_entry *entrie = iter->second;
                    //std::cout << "   try " << tck << std::endl;

//std::cout << "entry + (4 * tickPrescale) " << (tck + 4 * tickPrescale) << "< ?? bigT = " <<  bigTick << std::endl;

                    // Remember, tick values do NOT wrap around.
                    // It may make more sense to have the inequality as:
                    // tck < bigTick - 4*tickPrescale
                    // Except then we run into trouble early with negative # in unsigned arithemtic
                    // showing up as huge #s. So have negative term switch sides.
                    // The idea is that any tick < 4 prescales below max Tick need to be removed from maps
                    if (tik + 4 * tickPrescale < biggestTick) {
//std::cout << "Remove tick " << tik << ", tick + (4*prescale) " << (tik + 4 * tickPrescale) << " < bigT " <<  biggestTick << std::endl;
                        if (takeStats) {
                            // Each tick has a component from each incoming data source - update all.
                            et_event **evs = et_fifo_getBufs(entry);
                            for (int i=0; i < srcIdCount; i++) {
                                int id = bufIds[i];
                                et_event *ev = evs[i];
                                if (ev != nullptr) {
                                    et_event_getcontrol(event, con);
                                    et_event_getlength(event, &totalBytesWritten);

                                    statMap[id]->discardedBytes += totalBytesWritten;
                                    statMap[id]->discardedBuffers++;
                                    statMap[id]->discardedPackets += con[5];
                                }
                            }
                        }

                        iter = buffers.erase(iter);

                        // Take this fifo entry and release it back to the ET system.
                        // Each event in this entry has already been labelled as "having data"
                        // if it's been fully reassembled. So reader of this fifo entry
                        // needs to be aware.
                        et_fifo_putEntry(entrie);

                        // Put entry back into freeEntries for reuse
                        freeEntries.insert(entrie);
                    }
                    else {
                        ++iter;
                    }
                }

                // read next packet
            }
        }



    }



#endif // EJFAT_ASSEMBLE_ERSAP_ET_H
