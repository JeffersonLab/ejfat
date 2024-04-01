//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 3/15/24.
//

#include "EjfatProducer.h"



namespace ejfat {


    /**
     * Constructor depending on first on the environmental variable EJFAT_URI.
     * If that exists and can be read and parsed, it may provide the necessary
     * info to send data to the LB and sync messages to the CP. If it does
     * <b>NOT</b>, then the fileName arg is used to find a file in which
     * this uri has been stored. If that exists and can be read and parsed, it
     * may provide the needed info. If so, everything is fine, if not, throw
     * an exception.
     *
     * @param fileName name of file with uri stored in it.
     *                 If no file, provide blank string.
     */
    EjfatProducer::EjfatProducer(const std::string &fileName) {

        bool foundUri = false;
        ejfatURI uriInfo;

        // First see if the EJFAT_URI environmental variable is defined, if so, parse it
        const char* uriStr = std::getenv("EJFAT_URI");
        if (uriStr != nullptr) {
            std::string uri(uriStr);
            bool parsed = parseURI(uri, uriInfo);
            if (parsed) {
                foundUri = true;
            }
        }

        // If no luck with env var, look in to a local file
        if (!foundUri && !fileName.empty()) {

            std::ifstream file(fileName);
            if (file.is_open()) {
                std::string uriLine;
                if (std::getline(file, uriLine)) {
                    bool parsed = parseURI(uriLine, uriInfo);
                    if (parsed) {
                        foundUri = true;
                    }
                }

                file.close();
            }
        }

        if (!foundUri) {
            // our luck ran out
            throwEjfatLine("no LB/CP info in env var or in file");
        }

        // Set internal members from struct obtained by parsing URI
        bool haveEverything = setFromURI(uriInfo);
        if (!haveEverything) {
            throwEjfatLine("URI did not have info to send data or sync msgs");
        }

        // 20 bytes = normal IPv4 packet header (60 is max), 8 bytes = max UDP packet header
        // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
        maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;

        createSyncSocket();
        createDataSocket();

        startupSendThread();
        startupStatisticsThread();
    }



    EjfatProducer::EjfatProducer(const std::string &dataAddress, const std::string &syncAddress) :
            dataAddr(dataAddress), syncAddr(syncAddress) {

        maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;

        createSyncSocket();
        createDataSocket();

        startupSendThread();
        startupStatisticsThread();
    }


    EjfatProducer::EjfatProducer(const std::string &dataAddress, const std::string &syncAddress,
                                 uint16_t dataPort, uint16_t syncPort, int mtu,
                                 std::vector<int> &cores,
                                 int delay, int delayPrescale, bool connect,
                                 uint16_t id, int entropy, int version, int protocol) :

            dataAddr(dataAddress), syncAddr(syncAddress),
            dataPort(dataPort), syncPort(syncPort),
            mtu(mtu), delay(delay), delayPrescale(delayPrescale),
            cores(cores), connectSocket(connect),
            id(id), entropy(entropy), version(version), protocol(protocol)

    {
        maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;
        delayCounter = delayPrescale;

        createSyncSocket();
        createDataSocket();

        startupSendThread();
        startupStatisticsThread();
    }



    /**
     * Set this object internal member from the struct obtained from parsing an ejfat URI.
     * @param uri ref to struct with CP/LB connection info.
     * @return true if all need info is there, else false;
     */
    bool EjfatProducer::setFromURI(ejfatURI & uri) {

        bool haveWhatsNeeded = uri.haveData && uri.haveSync;
        if (!haveWhatsNeeded) return false;

        dataAddr = uri.dataAddrV4;
        dataPort = uri.dataPort;

        syncAddr = uri.syncAddrV4;
        syncPort = uri.syncPort;

        return true;
    }


    void EjfatProducer::createSyncSocket() {
        // Socket for sending sync message to CP
        if (ipv6) {
            if ((syncSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                if (debug) perror("creating IPv6 sync socket");
                throw EjfatException("error creating IPv6 sync socket");
            }

            memset(&syncAddrStruct6, 0, sizeof(syncAddrStruct6));
            syncAddrStruct6.sin6_family = AF_INET6;
            syncAddrStruct6.sin6_port = htons(syncPort);
            inet_pton(AF_INET6, syncAddr.c_str(), &syncAddrStruct6.sin6_addr);

            if (connectSocket) {
                int err = connect(syncSocket, (const sockaddr *) &syncAddrStruct6, sizeof(struct sockaddr_in6));
                if (err < 0) {
                    close(syncSocket);
                    if (debug) perror("Error connecting UDP sync socket:");
                    throw EjfatException("error connecting UDP sync socket");
                }
            }
        }
        else {
            if ((syncSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 sync socket");
                throw EjfatException("error creating IPv4 sync socket");
            }

            memset(&syncAddrStruct, 0, sizeof(syncAddrStruct));
            syncAddrStruct.sin_family = AF_INET;
            syncAddrStruct.sin_port = htons(syncPort);
            syncAddrStruct.sin_addr.s_addr = inet_addr(syncAddr.c_str());
            memset(syncAddrStruct.sin_zero, '\0', sizeof syncAddrStruct.sin_zero);

            if (connectSocket) {
fprintf(stderr, "Connecting sync socket to host %s, port %hu\n", syncAddr.c_str(), syncPort);
                int err = connect(syncSocket, (const sockaddr *) &syncAddrStruct, sizeof(struct sockaddr_in));
                if (err < 0) {
                    close(syncSocket);
                    if (debug) perror("Error connecting UDP sync socket:");
                    throw EjfatException("error connecting UDP sync socket");
                }
            }
        }
    }



    void EjfatProducer::createDataSocket() {
        // Socket for sending data message to LB

        if (ipv6) {
            // Configure settings in address struct
            // Clear it out
            memset(&sendAddrStruct6, 0, sizeof(sendAddrStruct6));
            // it is an INET address
            sendAddrStruct6.sin6_family = AF_INET6;
            // the port we are going to send to, in network byte order
            sendAddrStruct6.sin6_port = htons(dataPort);
            // the server IP address, in network byte order
            inet_pton(AF_INET6, dataAddr.c_str(), &sendAddrStruct6.sin6_addr);
        }
        else {
            memset(&sendAddrStruct, 0, sizeof(sendAddrStruct));
            sendAddrStruct.sin_family = AF_INET;
            sendAddrStruct.sin_port = htons(dataPort);
            sendAddrStruct.sin_addr.s_addr = inet_addr(dataAddr.c_str());
            memset(sendAddrStruct.sin_zero, '\0', sizeof sendAddrStruct.sin_zero);
        }


        if (ipv6) {
            // create a DGRAM (UDP) socket in the INET/INET6 protocol
            if ((dataSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                if (debug) perror("creating IPv6 sync socket");
                throw EjfatException("error creating IPv6 sync socket");
            }


#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            int sendBufBytes = udpSendBufSize <= 0 ? 25000000 : udpSendBufSize;
            setsockopt(dataSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif

            if (connectSocket) {
                int err = connect(dataSocket, (const sockaddr *) &sendAddrStruct6, sizeof(struct sockaddr_in6));
                if (err < 0) {
                    close(dataSocket);
                    if (debug) perror("Error connecting UDP sync socket:");
                    throw EjfatException("error connecting UDP sync socket");
                }
            }
        }
        else {
            if ((dataSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 sync socket");
                throw EjfatException("error creating IPv4 sync socket");
            }

// If you want to bind to an interface
//            struct ifreq ifr;
//            memset(&ifr, 0, sizeof(ifr));
//            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "enp193s0f1np1");
//
//            if (setsockopt(dataSocket, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) < 0) {
//                perror("error setting socket option");
//                throw EjfatException("error setting socket option");
//            }
//            if (debug) fprintf(stderr, "UDP socket bound to enp193s0f1np1 interface\n");


#ifndef __APPLE__
            int sendBufBytes = udpSendBufSize <= 0 ? 25000000 : udpSendBufSize;
            setsockopt(dataSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif

            if (connectSocket) {
                if (debug) fprintf(stderr, "Connection socket to host %s, port %hu\n", dataAddr.c_str(), dataPort);
                int err = connect(dataSocket, (const sockaddr *) &sendAddrStruct, sizeof(struct sockaddr_in));
                if (err < 0) {
                    close(dataSocket);
                    if (debug) perror("Error connecting UDP sync socket:");
                    throw EjfatException("error connecting UDP sync socket");
                }
            }
        }

        // set the don't fragment bit
#ifdef __linux__
        {
            int val = IP_PMTUDISC_DO;
            setsockopt(dataSocket, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
        }
#endif
    }


    //-----------------------------------------------------------
    // Statistics printout thread
    void EjfatProducer::statisticsThreadFunc(void *arg) {

        uint64_t packetCount, byteCount, eventCount;
        uint64_t prevTotalPackets, prevTotalBytes, prevTotalEvents;
        uint64_t currTotalPackets, currTotalBytes, currTotalEvents;

        // Ignore first rate calculation as it's most likely a bad value
        bool skipFirst = true;

        double rate, avgRate;
        int64_t totalT = 0, time;
        struct timespec t1, t2, firstT;

        // Get the current time
        clock_gettime(CLOCK_MONOTONIC, &t1);
        firstT = t1;

        while (true) {

            prevTotalBytes   = totalBytes;
            prevTotalPackets = totalPackets;
            prevTotalEvents  = totalEvents;

            // Delay 4 seconds between printouts
            std::this_thread::sleep_for(std::chrono::seconds(4));

            // Read time
            clock_gettime(CLOCK_MONOTONIC, &t2);

            // time diff in microseconds
            time   = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
            totalT = (1000000L * (t2.tv_sec - firstT.tv_sec)) + ((t2.tv_nsec - firstT.tv_nsec)/1000L);

            currTotalBytes   = totalBytes;
            currTotalPackets = totalPackets;
            currTotalEvents  = totalEvents;

            if (skipFirst) {
                // Don't calculate rates until data is coming in
                if (currTotalPackets > 0) {
                    skipFirst = false;
                }
                firstT = t1 = t2;
                totalT = totalBytes = totalPackets = totalEvents = 0;
                continue;
            }

            // Use for instantaneous rates
            byteCount   = currTotalBytes   - prevTotalBytes;
            packetCount = currTotalPackets - prevTotalPackets;
            eventCount  = currTotalEvents  - prevTotalEvents;

            // Reset things if #s rolling over
            if ( (byteCount < 0) || (totalT < 0) )  {
                totalT = totalBytes = totalPackets = totalEvents = 0;
                firstT = t1 = t2;
                continue;
            }

            // Packet rates
            rate = 1000000.0 * ((double) packetCount) / time;
            avgRate = 1000000.0 * ((double) currTotalPackets) / totalT;
            printf("Packets:  %3.4g Hz,  %3.4g Avg\n", rate, avgRate);

            // Data rates (with NO header info)
            rate = ((double) byteCount) / time;
            avgRate = ((double) currTotalBytes) / totalT;
            printf("Data   :  %3.4g MB/s,     %3.4g Avg\n", rate, avgRate);

            // Event rates
            rate = 1000000.0 * ((double) eventCount) / time;
            avgRate = 1000000.0 * ((double) currTotalEvents) / totalT;
            printf("Events :  %3.4g Hz,  %3.4g Avg, total %" PRIu64 "\n\n", rate, avgRate, totalEvents);

            t1 = t2;
        }
    }


    void EjfatProducer::startupStatisticsThread() {
        // Only want one of these threads running
        if (statThdStarted) return;

        void *arg;
        std::thread t(&EjfatProducer::statisticsThreadFunc, this, arg);

        // Move the thread to object member
        statThread = std::move(t);

        statThdStarted = true;

       // t.join(); // Wait for the thread to finish
    }


    //-----------------------------------------------------------
    // Event sending thread
    void EjfatProducer::sendThreadFunc(void *arg) {

        qItem *item;
        int spinMax = 100, spinCount = 0;

        while (true) {

            while (!queue.pop(item)) {
                if (++spinCount > spinMax) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(200));
                }
            }

            spinCount = 0;

            // A blocking call
            sendEvent(item->event, item->bytes, item->tick);

            // Run the callback after sending, better not be blocking!!
            if (item->callback != nullptr) {
                item->callback(item->cbArg);
            }
        }
    }


    void EjfatProducer::startupSendThread() {
        if (sendThdStarted) return;

#ifdef __linux__
        size_t coreCount = cores.size();

        if (coreCount > 0) {
            // Create a cpu_set_t object representing a set of CPUs. Clear it and mark given CPUs as set.
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);

            for (int i=0; i < coreCount; i++) {
                if (cores[i] >= 0) {
                    std::cerr << "Run send thread on core " << cores[i] << "\n";
                    CPU_SET(cores[i], &cpuset);
                }
            }

            pthread_t current_thread = pthread_self();
            int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
            }
        }
#endif

        void *arg;
        std::thread t(&EjfatProducer::sendThreadFunc, this, arg);

        sendThread = std::move(t);
        sendThdStarted = true;
    }


    void EjfatProducer::sendSyncMsg(uint64_t tick, uint64_t currentTimeNanos, uint32_t evtRate) {

        if (debug) fprintf(stderr, "send tick %" PRIu64 ", evtRate %u\n\n", tick, evtRate);

        setSyncData(syncBuf, version, id, tick, evtRate, currentTimeNanos);
        int err;
        if (connectSocket) {
            err = (int) send(syncSocket, syncBuf, 28, 0);
        }
        else {
            if (ipv6) {
                err = (int) sendto(syncSocket, syncBuf, 28, 0, (sockaddr * ) & syncAddrStruct6, sizeof(struct sockaddr_in6));
            }
            else {
                err = (int) sendto(syncSocket, syncBuf, 28, 0,  (sockaddr * ) & syncAddrStruct, sizeof(struct sockaddr_in));
            }
        }


        if (err == -1) {
            perror("sendSyncMsg");
            throwEjfatLine(" error sending sync msg");
        }
    }



    void EjfatProducer::sendEvent(char *event, size_t bytes) {
        sendEvent(event, bytes, tick++);
    }



    void EjfatProducer::sendEvent(char *event, size_t bytes, uint64_t tick) {

        if (event == nullptr || bytes == 0) {
            return;
        }

        this->tick = tick;

        uint32_t offset = 0;
        int64_t packetsSent;
        int err;

        if (connectSocket) {
            err = sendPacketizedBufferSendNew(event, bytes, maxUdpPayload,
                                              dataSocket,
                                              tick, protocol, entropy, version, id,
                                              (uint32_t)bytes, &offset,
                                              0, 1, nullptr,
                                              true, true, debug, false, &packetsSent);
        }
        else {
            if (ipv6) {
                err = sendPacketizedBufferSendNew(event, bytes, maxUdpPayload,
                                                  dataSocket,
                                                  tick, protocol, entropy, version, id,
                                                  bytes, &offset,
                                                  0, 1, nullptr,
                                                  true, true, debug,
                                                  false, true,
                                                  &packetsSent, 0,
                                                  (sockaddr * ) & sendAddrStruct6, sizeof(struct sockaddr_in6));
            }
            else {
                err = sendPacketizedBufferSendNew(event, bytes, maxUdpPayload,
                                                  dataSocket,
                                                  tick, protocol, entropy, version, id,
                                                  bytes, &offset,
                                                  0, 1, nullptr,
                                                  true, true, debug,
                                                  false, true,
                                                  &packetsSent, 0,
                                                  (sockaddr * ) & sendAddrStruct, sizeof(struct sockaddr_in));
            }
        }


        eventsSinceLastSync++;
        totalEvents++;
        totalBytes   += bytes;
        totalPackets += packetsSent;

        //------------------------------------------------
        // Deal with sync message to CP

        // Get the current time point
        auto nowT = std::chrono::high_resolution_clock::now();
        // Convert the time point to nanoseconds since the epoch
        auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(nowT.time_since_epoch()).count();
        uint64_t currentTimeNanos = static_cast<uint64_t>(now);

        // Calculate the time difference in nanoseconds
        auto timeDiff = currentTimeNanos - lastSyncTimeNanos;

        // if >= 1 sec ...
        if (timeDiff >= 1000000000UL) {
            // Calculate event rate in Hz
            uint32_t evtRate = eventsSinceLastSync/(timeDiff/1000000000UL);

            if (debug) fprintf(stderr, "send tick %" PRIu64 ", evtRate %u\n\n", tick, evtRate);

            // Send sync message which will throw exception if problem
            sendSyncMsg(tick, currentTimeNanos, evtRate);

            lastSyncTimeNanos = currentTimeNanos;
            eventsSinceLastSync = 0;
        }

        // delay if any
        if (delay) {
            if (--delayCounter < 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
                delayCounter = delayPrescale;
            }
        }
    }



//    /**
//     * Turn this into a blocking push onto the internal queue of events to be sent.
//     *
//     * @param event
//     * @param bytes
//     * @param callback
//     * @param cbArg
//     */
//    void EjfatProducer::addToSendQueueBlocking(char *event, size_t bytes, uint64_t tick,
//                                               void* (*callback)(void *), void *cbArg) {
//
//        qItem *item = &qItemArray[currentQItem];
//
//        this->tick     = tick;
//        item->tick     = tick;
//        item->event    = event;
//        item->bytes    = bytes;
//        item->cbArg    = cbArg;
//        item->callback = callback;
//
//        int spinMax = 100, spinCount = 0;
//        while (!queue.push(item)) {
//            if (++spinCount > spinMax) {
//                std::this_thread::sleep_for(std::chrono::nanoseconds(200));
//            }
//        }
//
//        currentQItem = (currentQItem + 1) % ARRAYSIZE;
//    }



    /**
     * Non-blocking push onto the internal queue of events to be sent.
     * Tick handled internally.
     *
     * @param event
     * @param bytes
     * @param callback
     * @param cbArg
     * @return true if event placed on queue, else false.
     */
    bool EjfatProducer::addToSendQueue(char *event, size_t bytes, void* (*callback)(void *), void *cbArg) {
        return addToSendQueue(event, bytes, tick++, callback, cbArg);
    }


    /**
     * Non-blocking push onto the internal queue of events to be sent. Specifies tick.
     *
     * @param event
     * @param bytes
     * @param tick
     * @param callback
     * @param cbArg
     * @return true if event placed on queue, else false.
     */
    bool EjfatProducer::addToSendQueue(char *event, size_t bytes, uint64_t tick,
                                       void* (*callback)(void *), void *cbArg) {

        qItem *item = &qItemArray[currentQItem];

        this->tick     = tick;
        item->tick     = tick;
        item->event    = event;
        item->bytes    = bytes;
        item->cbArg    = cbArg;
        item->callback = callback;

        if (queue.push(item)) {
            currentQItem = (currentQItem + 1) % ARRAYSIZE;
            return true;
        }

        return false;
    }



}


