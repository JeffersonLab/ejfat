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

#include "EjfatServer.h"


namespace ejfat {


    /** Destructor that ends threads. */
    EjfatServer::~EjfatServer() {
        endThreads = true;
    }


    /**
     * Constructor which specifies the IP address to send data to and
     * the IP address to send the sync messages to. Everything else is
     * set to their defaults, including port numbers.
     *
     * @param uri          URI containing details of LB/CP connections(default "").
     * @param fileName     name of environmental variable containing URI (default /tmp/ejfat_uri).
     * @param ids          vector of data source ids (defaults to single source of id=0).
     * @param dataPort     UDP port to receive data on (defaults to 19500).
     * @param consumerPort UDP port to receive messages from consumers on (defaults to 18300).
     * @param useIpv6      if true, listen on local ipv6 sockets.
     * @param connect      if true, call connect on data & sync sockets to LB.
     * @param debug        if true, printout debug statements (default false).
     * @param startingCore first core to run the sending thread on (default, -1, is no core affinity).
     * @param coreCount    number of cores the sending thread will run on (default 2).
     * @param version        version number of ejfat software repo (defaults to 2).
     * @param protocol       version number of ejfat communication protocol (defaults to 1).
     *
     * @throws EjfatException if no information about the LB/CP is available or could be parsed.
     */
    EjfatServer::EjfatServer(const std::string& uri,
                             const std::string& fileName,
                             const std::vector<int> &ids,
                             uint16_t dataPort, uint16_t consumerPort,
                             bool useIpv6, bool connect, bool debug,
                             int startingCore, int coreCount,
                             int version, int protocol) :

            dataPortIn(dataPort), consumerPort(consumerPort),
            useIPv6(useIpv6), connectSockets(connect), debug(debug),
            ids(ids), startingCore(startingCore),
            coreCount(coreCount), endThreads(false),
            version(version), protocol(protocol)

    {
        ejfatURI uriInfo;
        bool haveEverything = false;

        // First see if the uri arg is defined, if so, parse it
        if (!uri.empty()) {
            bool parsed = parseURI(uri, uriInfo);
            if (parsed) {
                // URI is in correct format, so set internal members
                // from struct obtained by parsing URI.
                haveEverything = setFromURI(uriInfo);
            }
        }

        // If no luck with URI, look into file
        if (!haveEverything && !fileName.empty()) {

            std::ifstream file(fileName);
            if (file.is_open()) {
                std::string uriLine;
                if (std::getline(file, uriLine)) {
                    bool parsed = parseURI(uriLine, uriInfo);
                    if (parsed) {
                        haveEverything = setFromURI(uriInfo);
                    }
                }

                file.close();
            }
        }

        if (!haveEverything) {
            // our luck ran out
            throwEjfatLine("no LB/CP info in URI or in file");
        }

        printUri(std::cerr, uriInfo);

        if (useIpv6) {
            ipv6DataAddrIn = true;
            ipv6ConsumerAddr = true;
        }

        // Fill with 0s
        ticks.assign(ids.size(), 0);
        bufsSent.assign(ids.size(), 0);

        createSocketsAndStartThreads();
    }



    /** Method to set max UDP packet payload, create sockets, and startup threads. */
    void EjfatServer::createSocketsAndStartThreads() {
        createDataInputSocket();
        createDataOutputSocket();
        createSyncSocket();
        createConsumerSocket();

        startupSyncThread();
        startupSendThread();
        startupConsumerThread();
    }



    /**
     * Set this object's internal members from the struct
     * obtained from parsing an ejfat URI.
     *
     * @param uri ref to struct with CP/LB connection info.
     * @return true if all needed info is there, else false;
     */
    bool EjfatServer::setFromURI(ejfatURI & uri) {

        // TODO: do we really want this???
        if (!uri.haveInstanceToken) return false;

        cpAddr = uri.cpAddr;
        cpPort = uri.cpPort;
        instanceToken = uri.instanceToken;
        lbId = uri.lbId;


        bool haveWhatsNeeded = uri.haveData && uri.haveSync;
        if (!haveWhatsNeeded) return false;

        // data address and port
        if (uri.useIPv6Data) {
            dataAddrLB = uri.dataAddrV6;
            ipv6DataAddrLB = true;
        }
        else {
            dataAddrLB = uri.dataAddrV4;
            ipv6DataAddrLB = false;
        }
        dataPortLB = uri.dataPort;

        // sync address and port
        if (uri.useIPv6Sync) {
            syncAddr = uri.syncAddrV6;
            ipv6SyncAddr = true;
        }
        else {
            syncAddr = uri.syncAddrV4;
            ipv6SyncAddr = false;
        }
        syncPort = uri.syncPort;



        return true;
    }



    /**
     * Method to create a single UDP socket for reading data from sources and
     * another socket to send data to the LB.
     */
    void EjfatServer::createDataInputSocket() {

        // Creating socket from which to read data from sources

        if (ipv6DataAddrIn) {
            struct sockaddr_in6 serverAddr6{};

            // Create IPv6 UDP socket
            if ((dataSocketIn = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                exit(1);
            }

            // Set & read back UDP receive buffer size
            setsockopt(dataSocketIn, SOL_SOCKET, SO_RCVBUF, &udpRecvBufSize, sizeof(udpRecvBufSize));

            // Configure settings in address struct
            // Clear it out
            memset(&serverAddr6, 0, sizeof(serverAddr6));
            // it is an INET address
            serverAddr6.sin6_family = AF_INET6;
            // the port we are going to receiver from, in network byte order
            serverAddr6.sin6_port = htons(dataPortIn);
            if (!listeningAddr.empty()) {
                inet_pton(AF_INET6, listeningAddr.c_str(), &serverAddr6.sin6_addr);
            }
            else {
                serverAddr6.sin6_addr = in6addr_any;
            }

            // Bind socket with address struct
            int err = bind(dataSocketIn, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                exit(1);
            }
        }
        else {
            // Create UDP socket
            if ((dataSocketIn = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                exit(1);
            }

            // Set & read back UDP receive buffer size
            setsockopt(dataSocketIn, SOL_SOCKET, SO_RCVBUF, &udpRecvBufSize, sizeof(udpRecvBufSize));

            // Configure settings in address struct
            struct sockaddr_in serverAddr{};
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(dataPortIn);
            if (!listeningAddr.empty()) {
                serverAddr.sin_addr.s_addr = inet_addr(listeningAddr.c_str());
            }
            else {
                serverAddr.sin_addr.s_addr = INADDR_ANY;
            }
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            // Bind socket with address struct
            int err = bind(dataSocketIn, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
            if (err != 0) {
                fprintf(stderr, "bind socket error\n");
                exit(1);
            }
        }
    }


    /**
     * Method to create a UDP socket for sending data to LB.
     */
    void EjfatServer::createDataOutputSocket() {
        // Creating socket from which to send data to LB

        if (ipv6DataAddrLB) {
            // Configure settings in address struct
            // Clear it out
            memset(&sendAddrStruct6, 0, sizeof(sendAddrStruct6));
            // it is an INET address
            sendAddrStruct6.sin6_family = AF_INET6;
            // the port we are going to send to, in network byte order
            sendAddrStruct6.sin6_port = htons(dataPortLB);
            // the server IP address, in network byte order
            inet_pton(AF_INET6, dataAddrLB.c_str(), &sendAddrStruct6.sin6_addr);
        }
        else {
            memset(&sendAddrStruct, 0, sizeof(sendAddrStruct));
            sendAddrStruct.sin_family = AF_INET;
            sendAddrStruct.sin_port = htons(dataPortLB);
            sendAddrStruct.sin_addr.s_addr = inet_addr(dataAddrLB.c_str());
            memset(sendAddrStruct.sin_zero, '\0', sizeof sendAddrStruct.sin_zero);
        }


        if (ipv6DataAddrLB) {
            // create a DGRAM (UDP) socket in the INET/INET6 protocol
            if ((dataSocketLB = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                if (debug) perror("creating IPv6 sync socket");
                throw EjfatException("error creating IPv6 sync socket");
            }


#ifndef __APPLE__
            // Try to increase send buf size - by default to 25 MB
            int sendBufBytes = udpSendBufSize <= 0 ? 25000000 : udpSendBufSize;
            setsockopt(dataSocketLB, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif

            if (connectSockets) {
                int err = connect(dataSocketLB, (const sockaddr *) &sendAddrStruct6, sizeof(struct sockaddr_in6));
                if (err < 0) {
                    close(dataSocketLB);
                    if (debug) perror("Error connecting UDP sync socket:");
                    throw EjfatException("error connecting UDP sync socket");
                }
            }
        }
        else {
            if ((dataSocketLB = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
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
            setsockopt(dataSocketLB, SOL_SOCKET, SO_SNDBUF, &sendBufBytes, sizeof(sendBufBytes));
#endif

            if (connectSockets) {
                if (debug) fprintf(stderr, "Connection socket to host %s, port %hu\n", dataAddrLB.c_str(), dataPortLB);
                int err = connect(dataSocketLB, (const sockaddr *) &sendAddrStruct, sizeof(struct sockaddr_in));
                if (err < 0) {
                    close(dataSocketLB);
                    if (debug) perror("Error connecting UDP sync socket:");
                    throw EjfatException("error connecting UDP sync socket");
                }
            }
        }

        // set the don't fragment bit
#ifdef __linux__
        {
            int val = IP_PMTUDISC_DO;
            setsockopt(dataSocketLB, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val));
        }
#endif
    }


    /**
     * Method to create a UDP socket for sending sync messages to CP.
     */
    void EjfatServer::createSyncSocket() {
        // Socket for sending sync message to CP
        if (ipv6SyncAddr) {
            if ((syncSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                if (debug) perror("creating IPv6 sync socket");
                throw EjfatException("error creating IPv6 sync socket");
            }

            memset(&syncAddrStruct6, 0, sizeof(syncAddrStruct6));
            syncAddrStruct6.sin6_family = AF_INET6;
            syncAddrStruct6.sin6_port = htons(syncPort);
            inet_pton(AF_INET6, syncAddr.c_str(), &syncAddrStruct6.sin6_addr);

            if (connectSockets) {
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

            if (connectSockets) {
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


    /**
     * Method to create a single UDP socket for input from consumers
     * which this server will pass on to the CP.
     */
    void EjfatServer::createConsumerSocket() {

        // Creating socket from which to read data from sources

        if (ipv6DataAddrIn) {
            struct sockaddr_in6 serverAddr6{};

            // Create IPv6 UDP socket
            if ((consumerSocket = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                exit(1);
            }

            // Set & read back UDP receive buffer size
            setsockopt(consumerSocket, SOL_SOCKET, SO_RCVBUF, &udpRecvBufSize, sizeof(udpRecvBufSize));

            // Configure settings in address struct
            // Clear it out
            memset(&serverAddr6, 0, sizeof(serverAddr6));
            // it is an INET address
            serverAddr6.sin6_family = AF_INET6;
            // the port we are going to receiver from, in network byte order
            serverAddr6.sin6_port = htons(consumerPort);
            if (!listeningAddr.empty()) {
                inet_pton(AF_INET6, listeningAddr.c_str(), &serverAddr6.sin6_addr);
            }
            else {
                serverAddr6.sin6_addr = in6addr_any;
            }

            // Bind socket with address struct
            int err = bind(consumerSocket, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                exit(1);
            }
        }
        else {
            // Create UDP socket
            if ((consumerSocket = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                exit(1);
            }

            // Set & read back UDP receive buffer size
            setsockopt(consumerSocket, SOL_SOCKET, SO_RCVBUF, &udpRecvBufSize, sizeof(udpRecvBufSize));

            // Configure settings in address struct
            struct sockaddr_in serverAddr{};
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(consumerPort);
            if (!listeningAddr.empty()) {
                serverAddr.sin_addr.s_addr = inet_addr(listeningAddr.c_str());
            }
            else {
                serverAddr.sin_addr.s_addr = INADDR_ANY;
            }
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            // Bind socket with address struct
            int err = bind(consumerSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
            if (err != 0) {
                fprintf(stderr, "bind socket error\n");
                exit(1);
            }
        }
    }


    /**
     * Method implementing a thread to read events from a socket,
     * sent by a single sender, and place them onto an internal queue.
     *
     * @throws EjfatException if socket read error
     */
    void EjfatServer::consumerThreadFunc() {

        simpleCmd cmd;
        uint16_t port;
        std::string ipAddr;
        float fill;
        float pidErr;
        bool isReady;
        float weight = 1.F;

        // Storage for packet
        char pkt[65536];

        while (!endThreads) {

            // Read UDP packet
            ssize_t bytesRead = recvfrom(consumerSocket, pkt, 65536, 0, nullptr, nullptr);
            if (bytesRead < 0) {
                perror("recvfrom fail");
                throwEjfatLine("error reading event");
            }
            else if (bytesRead < 12) {
                // Must have 1 byte for cmd, 1 for len, 2 for port, at least 7 for IP + 1 for null
                if (debug) std::cerr << "ignore packet w/ not enough data" << std::endl;
                // ignore packet
                continue;
            }

            bool success = getCommand(pkt, cmd);
            if (!success) {
                // no recognizable command contained, ignore packet
                continue;
            }

            // Update CP with latest fill level and PID error from consumer
            if (cmd == UPDATE) {
                // This call is always successful in this context
                parseUpdateStatusData(pkt, 65536, port, ipAddr, fill, pidErr, isReady);

                std::string key = ipAddr + ":" + std::to_string(port);
                std::shared_ptr <LbControlPlaneClient> client = nullptr;

                try {
                    // See if there's an entry in local map for this consumer
                    client = LbClients.at(key);
                }
                catch (const std::out_of_range &e) {
                    // No such client exists
                    std::cerr << "Consumer for key = " << key <<
                              " does NOT exist, register first!" << std::endl;

                    // ignore packet
                    continue;
                }

                client->update(fill, pidErr, isReady);
                if (debug) std::cout << "send state to CP" << std::endl;

                // Send to server
                int err = client->SendState();
                if (err == 1) {
                    std::cerr << "GRPC client " << client->getName()
                              << " communication error during sending state, exit" << std::endl;
                    exit(1);
                }
            }
                // Register consumer with CP
            else if (cmd == REGISTER) {
                // This call is always successful in this context
                parseSimpleRegisterData(pkt, 65536, port, ipAddr);

                // For more on portRange, look into loadbalancer.proto file in ersap-grpc git repo
                int sourceCount = ids.size();
                int portRangeValue = getPortRange(sourceCount);
                auto range = PortRange(portRangeValue);
                if (debug) std::cout << "GRPC client port range = " << portRangeValue << std::endl;

                // Create LbControlPlaneClient object
                std::string key = ipAddr + ":" + std::to_string(port);
                // Need to give this consumer a name based on IP addr and port
                std::string myName = "be_" + key;

                auto client = std::make_shared<LbControlPlaneClient>(cpAddr, cpPort,
                                                                     ipAddr, port, range,
                                                                     myName, instanceToken, lbId, weight);
                LbClients[key] = client;
                std::cout << "Created LbControlPlaneClient for " << ipAddr << ":" << port << std::endl;

                // Register this client with the control plane's grpc server &
                // wait for server to send session token in return.
                int err = client->Register();
                if (err == 1) {
                    std::cerr << "GRPC client " << client->getName() << " communication error when registering, exit"
                              << std::endl;
                    exit(1);
                }

                if (debug) std::cout << "GRPC client " << client->getName() << " is registered!" << std::endl;
            }
                // Deregister consumer with CP
            else if (cmd == DEREGISTER) {

                std::string key = ipAddr + ":" + std::to_string(port);
                std::shared_ptr <LbControlPlaneClient> client = nullptr;

                try {
                    // See if there's an entry in local map for this consumer
                    client = LbClients.at(key);
                }
                catch (const std::out_of_range &e) {
                    // No such client exists
                    std::cerr << "Consumer for key = " << key <<
                              " does NOT exist, no need to deregister!" << std::endl;

                    // ignore packet
                    continue;
                }

                if (debug) std::cout << "send deregistration msg to CP" << std::endl;

                // Send to server
                int err = client->Deregister();
                if (err == 1) {
                    std::cerr << "GRPC client " << client->getName()
                              << " communication error during sending state, exit" << std::endl;
                    exit(1);
                }
            }
        }
    }


    /**
     * Method to start up the threads to broker communications between consumers and this server.
     * Will not start them up more than once.
     */
    void EjfatServer::startupConsumerThread() {
        if (consumerThdStarted) return;
        // Start the thread
        std::thread t(&EjfatServer::consumerThreadFunc, this);
        // Store the thread object
        consumerThread = std::move(t);
        consumerThdStarted = true;
    }


    /**
     * Method implementing a thread to read events from a socket,
     * sent by a single sender, and place them onto an internal queue.
     */
    void EjfatServer::sendThreadFunc(sendThdArg *arg) {

        sendThdArg *tArg = (sendThdArg *)arg;

#ifdef __linux__

        int core = tArg->core;
        int coreCount = tArg->coreCount;

        if (core > -1) {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);

            for (int i=0; i < coreCount; i++) {
                if (debug) std::cout << "Run assemble thd for source " << srcId << " on core " << (core + i) << "\n";
                CPU_SET(core+i, &cpuset);
            }

            pthread_t current_thread = pthread_self();
            int rc = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
            if (rc != 0) {
                std::cerr << "Error calling pthread_setaffinity_np: " << rc << "\n";
            }
        }

#endif
        // Reassembly (RE) header size in bytes
        int err;

        // Storage for packet
        char pkt[65536];

        while (!endThreads) {
            // Read UDP packet
            ssize_t bytesRead = recvfrom(dataSocketIn, pkt, 65536, 0, nullptr, nullptr);
            if (bytesRead < 0) {
                endThreads = true;
                perror("recvfrom fail");
                throwEjfatLine("error reading event");
            }
            else if (bytesRead < HEADER_BYTES) {
                if (debug) std::cerr << "ignore packet w/ not enough data" << std::endl;
                // ignore packet
                continue;
            }

            // Pull out the version & protocol from each pkt's LB header to check compatibility
            uint8_t ver = *((uint8_t *) (pkt + 2));
            uint8_t pro = *((uint8_t *) (pkt + 3));
            if (version != ver || protocol != pro) {
                if (debug) std::cerr << "ignore packet w/ version = " << ver <<
                                        " and protocol = " << pro << std::endl;
                // ignore packet
                continue;
            }

            // Send pkt to receiver
            if (!connectSockets) {
                if (ipv6DataAddrLB) {
                    err = sendto(dataSocketLB, pkt, bytesRead, 0, (sockaddr * ) & sendAddrStruct6,
                                 sizeof(struct sockaddr_in6));
                }
                else {
                    err = sendto(dataSocketLB, pkt, bytesRead, 0, (sockaddr * ) & sendAddrStruct,
                                 sizeof(struct sockaddr_in));
                }
            }
            else {
                err = send(dataSocketLB, pkt, bytesRead, 0);
            }

            if (err < 0) {
                endThreads = true;
                perror("sendto");
                throwEjfatLine("error sending event");
            }

            // Pull out the id & tick from each pkt's RE header (not LB header)
            // and store that for use in thread to send sync msgs to CP.
            uint64_t tick  = *((uint64_t *) (pkt + LB_HEADER_BYTES + 12));
            uint16_t srcId = *((uint16_t *) (pkt + LB_HEADER_BYTES + 2));
            tick  = ntohll(tick);
            srcId = ntohs(srcId);

            uint64_t prevTick = ticks[srcId];
            ticks[srcId] = tick;

            // Store how many events have been sent from this source.
            // When tick # changes, we're working on the next event.
            if (tick != prevTick) {
                bufsSent[srcId]++;
            }
        }
    }


    /**
     * Method to start up the receiving threads.
     * Will not start them up more than once.
     */
    void EjfatServer::startupSendThread() {

        if (sendThdStarted) return;

        // Create an arg to pass to the thread
        sendThdArg *targ = (sendThdArg *) calloc(1, sizeof(sendThdArg));
        if (targ == nullptr) {
            std::cerr << "out of mem" << std::endl;
            exit(1);
        }

        if (startingCore == -1) {
            // no core affinity
            targ->core = -1;
        }
        else {
            targ->core = startingCore;
        }
        targ->coreCount = coreCount;

        // Start the thread
        std::thread t(&EjfatServer::sendThreadFunc, this, targ);

        // Store the thread object
        sendThread = std::move(t);

        sendThdStarted = true;
    }


    /**
     * Method implementing a thread to send sync msgs to the CP.
     */
    void EjfatServer::sendSyncFunc() {

        char syncBuf[28];

        // For the rate calculation get the starting time point
        auto startT = std::chrono::high_resolution_clock::now();
        // Convert the time point to nanoseconds since the epoch
        auto nano = std::chrono::duration_cast<std::chrono::nanoseconds>(startT.time_since_epoch()).count();
        uint64_t startTimeNanos = static_cast<uint64_t>(nano);


        while (!endThreads) {

            // Delay 1 second between syncs
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Get the current time point
            auto endT = std::chrono::high_resolution_clock::now();
            // Convert the time point to nanoseconds since the epoch
            auto tEnd = std::chrono::duration_cast<std::chrono::nanoseconds>(endT.time_since_epoch()).count();
            uint64_t currentTimeNanos = static_cast<uint64_t>(tEnd);

            // Calculate the time difference in nanoseconds
            auto timeDiff = currentTimeNanos - startTimeNanos;

            for (int srcId : ids) {
                uint64_t tick = ticks[srcId];
                uint64_t evtsSent = bufsSent[srcId];

                // Calculate event rate in Hz
                uint32_t evtRate = evtsSent / (timeDiff / 1000000000UL);

                if (debug) std::cerr << "for src " << srcId << ", sent tick " << tick <<
                                        ", evtRate " << evtRate << std::endl << std::endl;

                // Send sync message to CP
                setSyncData(syncBuf, version, srcId, tick, evtRate, currentTimeNanos);
                int err = send(syncSocket, syncBuf, 28, 0);
                if (err == -1) {
                    perror("send");
                    throwEjfatLine("error sending event");
                }

                bufsSent[srcId] = 0;
            }

            startTimeNanos = currentTimeNanos;
        }
    }


    /**
     * Method to start up the sync msg sending thread.
     * Will not start them up more than once.
     */
    void EjfatServer::startupSyncThread() {
        if (syncThdStarted) return;

        // Start the thread
        std::thread t(&EjfatServer::sendSyncFunc, this);

        // Store the thread object
        syncThread = std::move(t);

        syncThdStarted = true;
    }


}


