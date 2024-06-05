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

#include "serverProducer.h"



namespace ejfat {

    
    /** Destructor that ends threads. */
    serverProducer::~serverProducer() {
        endThreads = true;
    }


    /**
     * Constructor which specifies the IP address to send data to and
     * the IP address to send the sync messages to along with their ports.
     * All other parameters else can either be set explicitly or left to their defaults.
     *
     * @param serverAddr     server's IP address (either ipv6 or ipv4) to send data to.
     * @param serverPort     server's UDP port to send data to.
     * @param direct         if true, bypass LB and send data direct to consumer (dataAdddr, dataPort).
     * @param id             id number of this sender: 0,1,2 ... (defaults to 0).
     * @param entropy        number (0,1,2, ...) to add to the base destination port for a given destination host.
     *                       Used on receiving end to read different sources on different UDP ports
     *                       for multithreading and ease of programming purposes. Defaults to 0.
     * @param delay          delay in microseconds between each event sent (defaults to 0).
     * @param delayPrescale  (1,2, ... N), a delay is taken after every Nth event (defaults to 1).
     * @param connect        if true, call connect() on each UDP socket, both for data and syncs.
     *                       This speeds up communication, but requires the receiving socket
     *                       to be up and runnin. Defaults to false.
     * @param mtu            max # of bytes to send in a single UDP packet (9000 &lt;= mtu &gt;= 500).
     * @param cores          vector of cores to run the sending code on.
     * @param version        version number of ejfat software repo (defaults to 2).
     * @param protocol       version number of ejfat communication protocol (defaults to 1).
     *
     * @throws EjfatException if bad IP address or port arg(s).
     */
    serverProducer::serverProducer(const std::string& serverAddr,
                                 uint16_t serverPort,
                                 bool direct,
                                 uint16_t id, int entropy,
                                 int delay, int delayPrescale,
                                 bool connect, int mtu,
                                 const std::vector<int>& cores,
                                 int version, int protocol):

            serverAddr(serverAddr), serverPort(serverPort),
            direct(direct), id(id), entropy(entropy),
            delay(delay), delayPrescale(delayPrescale),
            connectSocket(connect), mtu(mtu),
            cores(cores),
            version(version), protocol(protocol),
            endThreads(false)

    {
        delayCounter = delayPrescale;

        ipv6Data = isIPv6(serverAddr);
        bool ipv4Data = isIPv4(serverAddr);

        // Check for valid IP addrs
        if (!ipv6Data && !ipv4Data) {
            throwEjfatLine("invalid data IP addr arg");
        }

        // Check for valid ports
        if (serverPort < 1024 || serverPort > 65535) {
            throwEjfatLine("invalid data port arg");
        }

        if (mtu > 9000) mtu = 9000;
        else if (mtu < 500) mtu = 500;

        createSocketsAndStartThreads();
    }



    /** Method to set max UDP packet payload, create sockets, and startup threads. */
    void serverProducer::createSocketsAndStartThreads() {
        // 20 bytes = normal IPv4 packet header (60 is max), 8 bytes = max UDP packet header
        // https://stackoverflow.com/questions/42609561/udp-maximum-packet-size
        maxUdpPayload = mtu - 20 - 8 - HEADER_BYTES;

        createServerSocket();
        startupSendThread();
        startupStatisticsThread();
    }



    /**
     * Method to create a UDP socket for sending data to a simple server
     * which will be forwarded to a load balancer's data plane.
     */
    void serverProducer::createServerSocket() {
        // Socket for sending data message to LB

        if (ipv6Data) {
            // Configure settings in address struct
            // Clear it out
            memset(&sendAddrStruct6, 0, sizeof(sendAddrStruct6));
            // it is an INET address
            sendAddrStruct6.sin6_family = AF_INET6;
            // the port we are going to send to, in network byte order
            sendAddrStruct6.sin6_port = htons(serverPort);
            // the server IP address, in network byte order
            inet_pton(AF_INET6, serverAddr.c_str(), &sendAddrStruct6.sin6_addr);
        }
        else {
            memset(&sendAddrStruct, 0, sizeof(sendAddrStruct));
            sendAddrStruct.sin_family = AF_INET;
            sendAddrStruct.sin_port = htons(serverPort);
            sendAddrStruct.sin_addr.s_addr = inet_addr(serverAddr.c_str());
            memset(sendAddrStruct.sin_zero, '\0', sizeof sendAddrStruct.sin_zero);
        }


        if (ipv6Data) {
            // create a DGRAM (UDP) socket in the INET/INET6 protocol
            if ((serverSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                if (debug) perror("creating IPv6 server socket");
                throw EjfatException("error creating IPv6 server socket");
            }


#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            int sendBufBytes = udpSendBufSize <= 0 ? 25000000 : udpSendBufSize;
            setsockopt(serverSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif

            if (connectSocket) {
                int err = connect(serverSocket, (const sockaddr *) &sendAddrStruct6, sizeof(struct sockaddr_in6));
                if (err < 0) {
                    close(serverSocket);
                    if (debug) perror("Error connecting UDP server socket:");
                    throw EjfatException("error connecting UDP server socket");
                }
            }
        }
        else {
            if ((serverSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 server socket");
                throw EjfatException("error creating IPv4 server socket");
            }

// If you want to bind to an interface
//            struct ifreq ifr;
//            memset(&ifr, 0, sizeof(ifr));
//            snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "enp193s0f1np1");
//
//            if (setsockopt(serverSocket, SOL_SOCKET, SO_BINDTODEVICE, (void *)&ifr, sizeof(ifr)) < 0) {
//                perror("error setting socket option");
//                throw EjfatException("error setting socket option");
//            }
//            if (debug) fprintf(stderr, "UDP socket bound to enp193s0f1np1 interface\n");


#ifndef __APPLE__
            int sendBufBytes = udpSendBufSize <= 0 ? 25000000 : udpSendBufSize;
            setsockopt(serverSocket, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif

            if (connectSocket) {
                if (debug) fprintf(stderr, "Connection socket to host %s, port %hu\n", serverAddr.c_str(), serverPort);
                int err = connect(serverSocket, (const sockaddr *) &sendAddrStruct, sizeof(struct sockaddr_in));
                if (err < 0) {
                    close(serverSocket);
                    if (debug) perror("Error connecting UDP server socket:");
                    throw EjfatException("error connecting UDP server socket");
                }
            }
        }

        // set the don't fragment bit
#ifdef __linux__
        {
            int val = IP_PMTUDISC_DO;
            setsockopt(serverSocket, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
        }
#endif
    }


    /**
     * Method implementing a statics keeping and printing thread.
     * Null pointer passed as an arg.
     */
    void serverProducer::statisticsThreadFunc(void *arg) {

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

            if (endThreads) {
                return;
            }

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


    /**
     * Method to start up the statistics thread.
     * It won't start up more than one.
     */
    void serverProducer::startupStatisticsThread() {
        // Only want one of these threads running
        if (statThdStarted) return;

        std::thread t(&serverProducer::statisticsThreadFunc, this, nullptr);

        // Move the thread to object member
        statThread = std::move(t);

        statThdStarted = true;

       // t.join(); // Wait for the thread to finish
    }


    /**
     * Method implementing a thread to take events off an internal queue
     * and send them to the LB. Null pointer passed as an arg.
     */
    void serverProducer::sendThreadFunc(void *arg) {

        qItem *item;
        int spinMax = 100, spinCount = 0;

        while (true) {

            if (endThreads) {
                return;
            }

            // Dequeue event
            while (!queue.pop(item)) {
                // Spin 100X before using delays
                if (++spinCount > spinMax) {
                    std::this_thread::sleep_for(std::chrono::nanoseconds(200));
                    if (endThreads) {
                        return;
                    }
                }
            }

            spinCount = 0;

            // A blocking call to send
            sendEvent(item->event, item->bytes, item->tick);

            // Run the callback after sending, better not be blocking!!
            if (item->callback != nullptr) {
                item->callback(item->cbArg);
            }
        }
    }


    /**
     * Method to start up the sending thread.
     * It won't start up more than one.
     */
    void serverProducer::startupSendThread() {
        if (sendThdStarted) return;

        // TODO: Core affinity needs to be set in the thread itself, not this thread!!!


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

        std::thread t(&serverProducer::sendThreadFunc, this, nullptr);

        sendThread = std::move(t);
        sendThdStarted = true;
    }


    /**
     * Method to send an event to the LB's data plane.
     * This method blocks until the data is sent.
     * This method also sends a sync message to the LB's control plane
     * once every second. And if a delay is defined, it will execute
     * that delay after the event and sync are sent.
     *
     * @param event pointer to data to send.
     * @param bytes number of bytes to send.
     */
    void serverProducer::sendEvent(char *event, size_t bytes) {
        sendEvent(event, bytes, tick++);
    }


    // TODO: problem if data is sent at less than 1 Hz!!!


    /**
     * Method to send an event to the LB's data plane.
     * This method blocks until the data is sent.
     * This method also sends a sync message to the LB's control plane
     * once every second. And if a delay is defined, it will execute
     * that delay after the event and sync are sent.
     *
     * @param event pointer to data to send.
     * @param bytes number of bytes to send.
     * @param eventNumber number of event to send. This number is written into the EJFAT
     *                    header of each UDP packet sent and is used by the LB to ensure
     *                    it's delivery to the correct backend reassembler. This number
     *                    must be monotonically increasing. For example, it can be a
     *                    simple sequential count or it can be the time in milliseconds
     *                    or nanoseconds past epoch.
      * @throws EjfatException   if error in sending event.
     */
    void serverProducer::sendEvent(char *event, size_t bytes, uint64_t eventNumber) {

        if (event == nullptr || bytes == 0) {
            return;
        }

        this->tick = eventNumber;

        uint32_t offset = 0;
        int64_t packetsSent;
        int err;

        if (connectSocket) {
            err = sendPacketizedBufferSendNew(event, bytes, maxUdpPayload,
                                              serverSocket,
                                              tick, protocol, entropy, version, id,
                                              (uint32_t)bytes, &offset,
                                              0, 1, nullptr,
                                              true, true, debug, direct, &packetsSent);
        }
        else {
            if (ipv6Data) {
                err = sendPacketizedBufferSendNew(event, bytes, maxUdpPayload,
                                                  serverSocket,
                                                  tick, protocol, entropy, version, id,
                                                  bytes, &offset,
                                                  0, 1, nullptr,
                                                  true, true, debug,
                                                  direct, true,
                                                  &packetsSent, 0,
                                                  (sockaddr * ) & sendAddrStruct6, sizeof(struct sockaddr_in6));
            }
            else {
                err = sendPacketizedBufferSendNew(event, bytes, maxUdpPayload,
                                                  serverSocket,
                                                  tick, protocol, entropy, version, id,
                                                  bytes, &offset,
                                                  0, 1, nullptr,
                                                  true, true, debug,
                                                  direct, true,
                                                  &packetsSent, 0,
                                                  (sockaddr * ) & sendAddrStruct, sizeof(struct sockaddr_in));
            }
        }

        if (err < 0) {
            perror("sendPacketizedBufferSendNew");
            throwEjfatLine("error sending event");
        }

        totalEvents++;
        totalBytes   += bytes;
        totalPackets += packetsSent;

        // delay if any
        if (delay) {
            if (--delayCounter < 1) {
                std::this_thread::sleep_for(std::chrono::microseconds(delay));
                delayCounter = delayPrescale;
            }
        }
    }



//    /**
//     * Turn adding event to be sent onto internal queue into a blocking push.
//     *
//     * @param event
//     * @param bytes
//     * @param eventNumber
//     * @param callback
//     * @param cbArg
//     */
//    void serverProducer::addToSendQueueBlocking(char *event, size_t bytes, uint64_t eventNumber,
//                                               void* (*callback)(void *), void *cbArg) {
//
//        qItem *item = &qItemArray[currentQItem];
//
//        this->tick     = eventNumber;
//        item->tick     = eventNumber;
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
     * Non-blocking add of event onto internal queue to be sent by separate thread.
     * Event number handled internally.
     *
     * @param event     pointer to data to send.
     * @param bytes     number of bytes to send.
     * @param callback  routine to be called after event is dequeued and sent (default nullptr).
     * @param cbArg     arg to be passed to callback when executed (default nullptr).
     *
     * @return true if event placed on queue, else false.
     */
    bool serverProducer::addToSendQueue(char *event, size_t bytes, void* (*callback)(void *), void *cbArg) {
        return addToSendQueue(event, bytes, tick++, callback, cbArg);
    }


    /**
     * Non-blocking add of event onto internal queue to be sent by separate thread.
     *
     * @param event       pointer to data to send.
     * @param bytes       number of bytes to send.
     * @param eventNumber number of event to send. This number is written into the EJFAT
     *                    header of each UDP packet sent and is used by the LB to ensure
     *                    it's delivery to the correct backend reassembler. This number
     *                    must be monotonically increasing. For example, it can be a
     *                    simple sequential count or it can be the time in milliseconds
     *                    or nanoseconds past epoch.
     * @param callback    routine to be called after event is dequeued and sent (default nullptr).
     * @param cbArg       arg to be passed to callback when executed (default nullptr).
     *
     * @return true if event placed on queue, else false.
     */
    bool serverProducer::addToSendQueue(char *event, size_t bytes, uint64_t eventNumber,
                                       void* (*callback)(void *), void *cbArg) {

        qItem *item = &qItemArray[currentQItem];

        this->tick     = eventNumber;
        item->tick     = eventNumber;
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


