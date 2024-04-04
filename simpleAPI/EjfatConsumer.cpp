//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 4/02/24.
//

#include "EjfatConsumer.h"



namespace ejfat {

    
    /** Destructor that ends threads. */
    EjfatConsumer::~EjfatConsumer() {
        endThreads = true;
    }



    /**
     * Constructor which specifies the IP address to send data to and
     * the IP address to send the sync messages to. Everything else is
     * set to their defaults, including port numbers.
     *
     * @param dataAddr    IP address (either ipv6 or ipv4) to receive data on.
     * @param cpAddr      IP address (currently only ipv4) to talk to control plane on.
     * @param ids         vector of data source ids (defaults to single source of id=0).
     * @param envVar      name of environmental variable containing URI (default EJFAT_URI).
     * @param fileName    name of environmental variable containing URI (default /tmp/ejfat_uri).
     * @param dataPort    starting UDP port to receive data on. The first src in ids will
     *                    received on this port, the second will be received on dataPort + 1, etc.
     *                    (defaults to 17750).
     * @param cpPort      TCP port to talk to the control plane on (defaults to 18347).
     * @param cores          comma-separated list of cores to run the sending code on.
     * @param delay          delay in microseconds between each event sent (defaults to 0).
     * @param delayPrescale  (1,2, ... N), a delay is taken after every Nth event (defaults to 1).
     * @param connect        if true, call connect() on each UDP socket, both for data and syncs.
     *                       This speeds up communication, but requires the receiving socket
     *                       to be up and runnin. Defaults to false.
     * @param entropy        number to add to the base destination port for a given destination host.
     *                       Used on receiving end to read different sources on different UDP ports
     *                       for multithreading and ease of programming purposes. Defaults to 0.
     * @param version        version number of ejfat software repo (defaults to 2).
     * @param protocol       version number of ejfat communication protocol (defaults to 1).
     *
     * @throws EjfatException if no information about the reserved LB is available or could be parsed.
     */
    EjfatConsumer::EjfatConsumer(const std::string &dataAddr, const std::string &cpAddr,
                                 const std::vector<int> &ids,
                                 const std::string& envVar,
                                 const std::string& fileName,
                                 uint16_t dataPort, uint16_t cpPort,
                                 int startingCore, int coreCount,
                                 float Kp, float Ki, float Kd, float weight) :

            dataAddr(dataAddr), cpAddr(cpAddr),
            dataPort(dataPort), cpPort(cpPort),
            ids(ids), startingCore(startingCore), coreCount(coreCount),
            Kp(Kp), Ki(Ki), Kd(Kp), weight(weight)

    {
        // Get the URI created by calling lbreserve.
        // This can be in the env var or file.
        std::string uri = getURI(envVar, fileName);
        if (uri.empty()) {
            throwEjfatLine("cannot find URI information in env var or file");
        }

        // Parse the URI
        ejfatURI parsedURI;
        bool parsed = parseURI(uri, parsedURI);
        if (!parsed) {
            throwEjfatLine("cannot parse URI information from env var or file");
        }

        // Set internal members from the URI info (instance token and lbId)
        setFromURI(parsedURI);

        ipv6DataAddr = isIPv6(dataAddr);


        // Based on the number of data sources, fill in maps with real objects

        // For each source ...
        for (size_t i = 0; i < ids.size(); ++i) {
            int srcId = ids[i];

            // Create a (shared pointer of a) queue for this source and place into map
            queues.emplace(srcId, std::make_shared<boost::lockfree::spsc_queue<qItem*, boost::lockfree::capacity<QSIZE>>>());

            // Create an vector of qItem objects.
            // By using a vector we invoke the constructor qItem() for each element.
            std::vector<qItem> items(VECTOR_SIZE);
            // Move vector into map of all vectors - central storage
            qItemVectors[srcId] = std::move(items);
            // Element of items vector that will be placed onto Q, start with 0
            currentQItems[srcId] = 0;
            // Create a struct to hold stats & place into map
            allStats.emplace(srcId, packetRecvStats());
        }


        // Need to give this back end a name (no, not "horse's"),
        // base part of it on least significant 6 digits of current time in microsec
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int time = now.tv_nsec/1000L;
        char name[256];
        sprintf(name, "be_%06d/lb/%s", (time % 1000000), lbId.c_str());
        myName = name;


        // For more on portRange, look into loadbalancer.proto file in ersap-grpc git repo

        int sourceCount = ids.size();
        int portRange;

        // Convert integer range in PortRange enum
        // Set port range according to sourceCount
        switch (sourceCount) {
            case 1:
                portRange = 0;
                break;
            case 2:
                portRange = 1;
                break;
            case 3: case 4:
                portRange = 2;
                break;
            case 5: case 6: case 7: case 8:
                portRange = 3;
                break;
            case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16:
                portRange = 4;
                break;
            default:
                // up to 32 inputs
                portRange = 5;
                break;
        }

        auto range = PortRange(portRange);

        LbClient = std::make_shared<LbControlPlaneClient> (cpAddr, cpPort,
                                                           dataAddr, dataPort, range,
                                                           myName, instanceToken, lbId, weight);

        // Register this client with the control plane's grpc server &
        // wait for server to send session token in return.
        int32_t err = LbClient->Register();
        if (err == 1) {
            printf("GRPC client %s communication error with server when registering, exit\n", myName.c_str());
            exit(1);
        }

        printf("GRPC client %s registered!\n", myName.c_str());


        createSocketsAndStartThreads();
    }



    /** Method to set max UDP packet payload, create sockets, and startup threads. */
    void EjfatConsumer::createSocketsAndStartThreads() {

        createDataSockets();
        startupRecvThreads();
        startupStatisticsThread();
    }



    /**
     * Set this object's internal members from the struct
     * obtained from parsing an ejfat URI.
     *
     * @param uri ref to struct with CP/LB connection info.
     * @return true if all needed info is there, else false;
     */
    bool EjfatConsumer::setFromURI(ejfatURI & uri) {

        if (!uri.haveInstanceToken) return false;

        instanceToken = uri.instanceToken;
        lbId = uri.lbId;
        return true;
    }



    /**
     * Method to create a UDP socket for reading data from a each source.
     */
    void EjfatConsumer::createDataSockets() {

        //---------------------------------------------------
        // Create 1 socket to read data from each source ID
        //---------------------------------------------------

        // For each source ...
        for (size_t i = 0; i < ids.size(); ++i) {

            int dataSocket;

            int srcId = ids[i];
            // Use this port. Note: when giving the constructor the vector of source ids,
            // they must be in the order of sequential destination port #s.
            uint16_t port = dataPort + i;

            if (ipv6DataAddr) {
                struct sockaddr_in6 serverAddr6{};

                // Create IPv6 UDP socket
                if ((dataSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                    perror("creating IPv6 client socket");
                    exit(1);
                }

                // Set & read back UDP receive buffer size
                socklen_t size = sizeof(int);
                setsockopt(dataSocket, SOL_SOCKET, SO_RCVBUF, &udpRecvBufSize, sizeof(udpRecvBufSize));

                // Configure settings in address struct
                // Clear it out
                memset(&serverAddr6, 0, sizeof(serverAddr6));
                // it is an INET address
                serverAddr6.sin6_family = AF_INET6;
                // the port we are going to receiver from, in network byte order
                serverAddr6.sin6_port = htons(port);
                if (!listeningAddr.empty()) {
                    inet_pton(AF_INET6, listeningAddr.c_str(), &serverAddr6.sin6_addr);
                }
                else {
                    serverAddr6.sin6_addr = in6addr_any;
                }

                // Bind socket with address struct
                int err = bind(dataSocket, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
                if (err != 0) {
                    if (debug) fprintf(stderr, "bind socket error\n");
                    exit(1);
                }
            }
            else {
                // Create UDP socket
                if ((dataSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                    perror("creating IPv4 client socket");
                    exit(1);
                }

                // Set & read back UDP receive buffer size
                socklen_t size = sizeof(int);
                setsockopt(dataSocket, SOL_SOCKET, SO_RCVBUF, &udpRecvBufSize, sizeof(udpRecvBufSize));

                // Configure settings in address struct
                struct sockaddr_in serverAddr{};
                memset(&serverAddr, 0, sizeof(serverAddr));
                serverAddr.sin_family = AF_INET;
                serverAddr.sin_port = htons(port);
                if (!listeningAddr.empty()) {
                    serverAddr.sin_addr.s_addr = inet_addr(listeningAddr.c_str());
                }
                else {
                    serverAddr.sin_addr.s_addr = INADDR_ANY;
                }
                memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

                // Bind socket with address struct
                int err = bind(dataSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
                if (err != 0) {
                    fprintf(stderr, "bind socket error\n");
                    exit(1);
                }
            }

            // store socket here
            dataSockets[srcId] = dataSocket;
        }
    }


    /**
     * Method implementing a statics keeping and printing thread.
     * Null pointer passed as an arg.
     */
    void EjfatConsumer::statisticsThreadFunc(void *arg) {

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
    void EjfatConsumer::startupStatisticsThread() {
        // Only want one of these threads running
        if (statThdStarted) return;

        std::thread t(&EjfatConsumer::statisticsThreadFunc, this, nullptr);

        // Move the thread to object member
        statThread = std::move(t);

        statThdStarted = true;
    }



    /**
     * Method implementing a thread to talk to the control plane.
     */
    void EjfatConsumer::grpcThreadFunc(recvThdArg *arg) {

        recvThdArg *tArg = (recvThdArg *) arg;
        int srcId = tArg->srcId;



//        size_t fill_level = queue.read_available();

    }

        /**
         * Method to start up the grpc thread.
         * It won't start up more than one.
         */
    void EjfatConsumer::startupGrpcThread() {
        // Only want one of these threads running
        if (grpcThdStarted) return;

        std::thread t(&EjfatConsumer::grpcThreadFunc, this, nullptr);

        // Move the thread to object member
        grpcThread = std::move(t);

        grpcThdStarted = true;
    }


    /**
     * Method implementing a thread to read events from a socket,
     * sent by a single sender, and place them onto an internal queue.
     */
    void EjfatConsumer::recvThreadFunc(recvThdArg *arg) {

        recvThdArg *tArg = (recvThdArg *)arg;
        int srcId = tArg->srcId;


#ifdef __linux__

        int core = tArg->core;
        int coreCount = tArg->coreCount;

        if (core > -1) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);

            for (int i=0; i < coreCount; i++) {
                if (debug) std::cerr << "Run assemble thd for source " << srcId << " on core " << (core + i) << "\n";
                CPU_SET(core+i, &cpuset);
            }

            pthread_t current_thread = pthread_self();
            int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
            }
        }

#endif

        // Need to figure out if source was killed and restarted.
        // If this is the case, then the tick # will be much smaller than the previous value.
        uint64_t tick = 0xffffffffffffffffL, prevTick, largestTick = 0;
        bool restarted = false, firstLoop = true;
        uint16_t dataId;

        auto queue        = queues[srcId];
        auto & qItemVec   = qItemVectors[srcId];
        int currentQItem  = currentQItems[srcId];
        int dataSocket    = dataSockets[srcId];
        auto stats        = &allStats[srcId];
        bool takeStats    = stats == nullptr;


        while (true) {

            // Get empty buffer from array
            qItem* item    = &qItemVec[currentQItem];
            char*  dataBuf = item->event;
            size_t bufSize = item->bufBytes;

            prevTick = tick;
            // tell reassembler that ticks are not in any particular order
            tick = 0xffffffffffffffffL;

            //-------------------------------------------------------------
            // Get reassembled buffer
            //-------------------------------------------------------------

            //TODO: this calls recvfrom which is blocking since dataSocket is
            // blocking. For nonblocking behavior one can modify the socket:
            //        int enable = 1;
            //        setsockopt(socket_fd, SOL_SOCKET, SO_NONBLOCK, &enable, sizeof(enable));
            // Perhaps a timeout behavior can be implemented

            ssize_t nBytes = getCompleteAllocatedBuffer(&dataBuf, &bufSize, dataSocket,
                                                        debug, &tick, &dataId, stats, 1);

            if (nBytes < 0) {
                fprintf(stderr, "Error in receiving data, %ld\n", nBytes);
                return;
            }

            // Check to see if dataId is the source we're expecting
            if (dataId != srcId) {
                // Big problems! Expecting source id = srcId, but getting dataId
                fprintf(stderr, "Error in receiving data, %ld\n", nBytes);
                return;
            }

            // The getCompleteAllocatedBuffer routine may have allocated memory.
            // So make sure we store its location and size in Q item.
            item->event     = dataBuf;
            item->bufBytes  = bufSize;
            item->dataBytes = nBytes;

            // Place onto queue
            if (queue->push(item)) {
                currentQItem = (currentQItem + 1) % VECTOR_SIZE;
            }
            else {
                // Failed to place item on queue since it's full
                // Dump the data and get the next one!
                continue;
            }

            // The first tick received may be any value depending on # of backends receiving
            // packets from load balancer.
            if (firstLoop) {
                prevTick = tick;
                firstLoop = false;
            }

            if (takeStats) {
                stats->builtBuffers++;
            }

            // See if data source was restarted with new, lower starting event number.
            if (tick >= prevTick) {
                largestTick = tick;
            }

            // How do we tell if a data source has been restarted? Hopefully this is good enough.
            bool restarted = (largestTick - tick > 1000);

            if (restarted) {
                //fprintf(stderr, "\nRestarted data source %d\n", sourceId);

                // Tell stat thread when restart happened so that rates can be calculated within reason
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                restartTime = now;

                if (takeStats) {
                    clearStats(stats);
                }
            }
        }
    }


    /**
     * Method to start up the receiving threads.
     * Will not start them up more than once.
     */
    void EjfatConsumer::startupRecvThreads() {

        if (recvThdsStarted) return;

        // For each source, start up a receiving thread
        for (size_t i = 0; i < ids.size(); ++i) {
            int srcId = ids[i];

            // Create an arg to pass to the thread
            recvThdArg *targ = (recvThdArg *) calloc(1, sizeof(recvThdArg));
            if (targ == nullptr) {
                std::cerr << "out of mem" << std::endl;
                exit(1);
            }

            targ->srcId = srcId;
            if (startingCore == -1) {
                // no core affinity
                targ->core = -1;
            }
            else {
                // each receiving thread uses different cores
                targ->core = startingCore + i*coreCount;
            }
            targ->coreCount = coreCount;

            // Start the thread
            std::thread t(&EjfatConsumer::recvThreadFunc, this, targ);

            // Store the thread object
            recvThreads[srcId] = std::move(t);
        }

        recvThdsStarted = true;
    }



    /**
     * Non-blocking retrieval of event sent from the specified data source.
     * Once an item is taken off the internal queue by this method, room is
     * created for the receiving thread to place another item onto it. If
     * long term access to the return event is desired, the caller
     * must copy it.
     * <p>
     * Note: it's possible, but unlikely, that the user calls this method
     * successively more times than the max number of simultaneously available
     * events (511) while continuing to access the event returned by the first
     * call. This will result in the receiving thread overwriting the event
     * while it's being used.
     * </p>
     *
     * @param event      pointer filled with pointer to event data.
     * @param bytes      pointer filled with event size in bytes.
     * @param eventNum   pointer filled with event number.
     * @param srcId      id of data source.
     *
     * @return true if event retrieved from queue, else false. If false, the queue
     *         may be empty or the source id may not be valid for this consumer.
     */
    bool EjfatConsumer::getEvent(char **event, size_t *bytes, uint64_t* eventNum, int srcId) {

        if (queues.count(srcId) == 0) {
            if (debug) std::cout << srcId << " is not a valid data source id for this consumer" << std::endl;
            return false;
        }

        qItem *item;
        auto queue = queues[srcId];
        int currentQItem  = currentQItems[srcId];

        // Dequeue event
        if (!queue->pop(item)) return false;

        if (bytes    != nullptr) *bytes    = item->bufBytes;
        if (eventNum != nullptr) *eventNum = item->tick;
        if (event != nullptr && *event != nullptr) *event = item->event;

        return true;
    }


}


