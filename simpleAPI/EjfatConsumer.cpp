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



    /** Function implementing a PID loop. */
    template<class X>
    X pid(                      // Proportional, Integrative, Derivative Controller
            const X& setPoint,  // Desired Operational Set Point
            const X& prcsVal,   // Measure Process Value
            const X& delta_t,   // Time delta between determination of last control value
            const X& Kp,        // Konstant for Proprtional Control
            const X& Ki,        // Konstant for Integrative Control
            const X& Kd,        // Konstant for Derivative Control
            const X& prev_err,  // previous error
            const X& prev_err_t // # of microseconds earlier that previous error was recorded
    )
    {
        static X integral_acc = 0;   // For Integral (Accumulated Error)
        X error = setPoint - prcsVal;
        integral_acc += error * delta_t;
        X derivative = (error - prev_err) * 1000000. / prev_err_t;
//    if (prev_err_t == 0 || prev_err_t != prev_err_t) {
//        derivative = 0;
//    }
        return Kp * error + Ki * integral_acc + Kd * derivative;  // control output
    }



    /** Destructor that ends threads. */
    EjfatConsumer::~EjfatConsumer() {
        endThreads = true;

        // Unregister this client with the grpc server
        LbClient->Deregister();
    }



    /**
     * Constructor which allows data to be sent directly from sender to this consumer,
     * bypassing the LB/CP. No communication with CP is done, hence no grpc lib is necessary.
     *
     * @param dataPort     starting UDP port to receive data on. The first src in ids will
     *                     received on this port, the second will be received on dataPort + 1, etc.
     *                     (defaults to 17750).
     * @param ids          vector of data source ids (defaults to single source of id=0).
     * @param debug        if true, printout debug statements (default false).
     * @param jointStats   if true, printout combined stats for all sources (default false).
     * @param startingCore first core to run the receiving threads on (default 0).
     * @param coreCount    number of cores each receiving thread will run on (default 2).
     *
     * @throws EjfatException if no information about the LB/CP is available or could be parsed.
     */
    EjfatConsumer::EjfatConsumer(uint16_t dataPort,
                                 const std::vector<int> &ids,
                                 bool debug, bool jointStats,
                                 int startingCore, int coreCount) :

            dataPort(dataPort),
            debug(debug), jointStats(jointStats), ids(ids),
            startingCore(startingCore), coreCount(coreCount),
            direct(true), endThreads(false)

    {
        queueSize = 0;
        queue = std::make_shared<boost::lockfree::queue<qItem*, boost::lockfree::capacity<QSIZE>>>();

        // Based on the number of data sources, fill in maps with real objects

        // For each source ...
        for (size_t i = 0; i < ids.size(); ++i) {
            int srcId = ids[i];

            // Create a vector of qItem objects.
            // By using a vector we invoke the constructor qItem() for each element.
            // This vector must be larger than QSIZE or it's possible to overwrite
            // events while being used (in extreme cases). Doing things this way
            // removes the need for mutex handling and contention when inserting qItems.
            std::vector<qItem> items(VECTOR_SIZE);
            // Move vector into map of all vectors - central storage
            qItemVectors[srcId] = std::move(items);
            // Element of items vector that will be placed onto Q, start with 0
            currentQItems[srcId] = 0;
            // Create a struct to hold stats & place into map
            allStats.emplace(srcId, packetRecvStats());
        }

        if (debug) std::cout << "Event consumer is ready to receive!" << std::endl;

        createSocketsAndStartThreads();
    }




    /**
     * Constructor which specifies the IP address to send data to and
     * the IP address to send the sync messages to. Everything else is
     * set to their defaults, including port numbers.
     *
     * @param dataAddr     IP address (either ipv6 or ipv4) to receive data on.
     * @param dataPort     starting UDP port to receive data on. The first src in ids will
     *                     received on this port, the second will be received on dataPort + 1, etc.
     *                     (defaults to 17750).
     * @param ids          vector of data source ids (defaults to single source of id=0).
     * @param uri          URI containing details of LB/CP connections(default "").
     * @param fileName     name of environmental variable containing URI (default /tmp/ejfat_uri).
     * @param debug        if true, printout debug statements (default false).
     * @param jointStats   if true, printout combined stats for all sources (default false).
     * @param startingCore first core to run the receiving threads on (default 0).
     * @param coreCount    number of cores each receiving thread will run on (default 2).
     * @param Kp           PID proportional constant used for error signal to CP (default 0.).
     * @param Ki           PID integral constant used for error signal to CP (default 0.).
     * @param Kd           PID differential constant used for error signal to CP (default 0.).
     * @param setPt        PID set point for queue fill level (default 0.).
     * @param weight       initial weight of this backend for CP to use in comparison to other backends.
     *                     User determines scales and values (default 1.).
     *
     * @throws EjfatException if no information about the LB/CP is available or could be parsed.
     */
    EjfatConsumer::EjfatConsumer(const std::string &dataAddr, uint16_t dataPort,
                                 const std::vector<int> &ids,
                                 const std::string& uri,
                                 const std::string& fileName,
                                 bool debug, bool jointStats,
                                 int startingCore, int coreCount,
                                 float Kp, float Ki, float Kd,
                                 float setPt, float weight) :

            dataAddr(dataAddr), dataPort(dataPort),
            debug(debug), jointStats(jointStats),
            ids(ids), startingCore(startingCore), coreCount(coreCount),
            Kp(Kp), Ki(Ki), Kd(Kp), setPoint(setPt),
            weight(weight), direct(false), endThreads(false)

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

        ipv6DataAddr = isIPv6(dataAddr);

        queueSize = 0;
        queue = std::make_shared<boost::lockfree::queue<qItem*, boost::lockfree::capacity<QSIZE>>>();

        // Based on the number of data sources, fill in maps with real objects

        // For each source ...
        for (size_t i = 0; i < ids.size(); ++i) {
            int srcId = ids[i];

            // Create a vector of qItem objects.
            // By using a vector we invoke the constructor qItem() for each element.
            // This vector must be larger than QSIZE or it's possible to overwrite
            // events while being used (in extreme cases). Doing things this way
            // removes the need for mutex handling and contention when inserting qItems.
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
        snprintf(name, 256, "be_%06d/lb/%s", (time % 1000000), lbId.c_str());
        myName = name;


        // For more on portRange, look into loadbalancer.proto file in ersap-grpc git repo
        int sourceCount = ids.size();
        int portRangeValue = getPortRange(sourceCount);
        auto range = PortRange(portRangeValue);
        if (debug) std::cout << "GRPC client port range = " << portRangeValue << std::endl;

        LbClient = std::make_shared<LbControlPlaneClient> (cpAddr, cpPort,
                                                           dataAddr, dataPort, range,
                                                           myName, instanceToken, lbId, weight);

        std::cout << "Created LbControlPlaneClient" << std::endl;
        // Register this client with the control plane's grpc server &
        // wait for server to send session token in return.
        int32_t err = LbClient->Register();
        if (err == 1) {
            std::cerr << "GRPC client " << myName << " communication error when registering, exit" << std::endl;
            exit(1);
        }

        if (debug) std::cout << "GRPC client " << myName << " is registered!" << std::endl;

        createSocketsAndStartThreads();
    }



    /** Method to set max UDP packet payload, create sockets, and startup threads. */
    void EjfatConsumer::createSocketsAndStartThreads() {

        createDataSockets();
        startupRecvThreads();
        startupStatisticsThread();
        if (!direct) startupGrpcThread();
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

        cpAddr = uri.cpAddr;
        cpPort = uri.cpPort;
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
     * Method to start up the statistics thread.
     * It won't start up more than one.
     */
    void EjfatConsumer::startupStatisticsThread() {
        // Only want one of these threads running
        if (statThdStarted) return;

        std::thread t(&EjfatConsumer::statisticsThreadFunc, this);
        // Move the thread to object member
        statThread = std::move(t);
        statThdStarted = true;
    }



    /** Thread implementing thread to send to print out statistics for multiple sources. */
    void EjfatConsumer::statisticsThreadFunc() {

        int64_t byteCount, pktCount, bufCount, readTime;
        int64_t discardByteCount, discardPktCount, discardBufCount;

        int sourceCount = ids.size();
        packetRecvStats* pstats;


        int64_t prevTotalPkts[sourceCount];
        int64_t prevTotalBytes[sourceCount];
        int64_t prevBuiltBufs[sourceCount];

        int64_t prevDiscardPkts[sourceCount];
        int64_t prevDiscardBytes[sourceCount];
        int64_t prevDiscardBufs[sourceCount];

// Comment out all the latency measurement stuff
//        int64_t prevTotalBuildTime[sourceCount];
//        int64_t currTotalBuildTime[sourceCount];

        int64_t currTotalPkts[sourceCount];
        int64_t currTotalBytes[sourceCount];
        int64_t currBuiltBufs[sourceCount];

        memset(currBuiltBufs, 0, sizeof(currBuiltBufs));

        int64_t currDiscardPkts[sourceCount];
        int64_t currDiscardBytes[sourceCount];
        int64_t currDiscardBufs[sourceCount];


        bool dataArrived[sourceCount];
        for (int i=0; i < sourceCount; i++) {
            dataArrived[i] = false;
        }

        int skippedFirst[sourceCount];
        for (int i=0; i < sourceCount; i++) {
            skippedFirst[i] = 0;
        }

        // Total time is different for each source since they may all start
        // sending their data at different times.
        int64_t totalMicroSecs[sourceCount];
        struct timespec tStart[sourceCount];


        double pktRate, pktAvgRate, dataRate, dataAvgRate;
        double  evRate,  evAvgRate, latencyInstAvg, latencyTotalAvg;
        int64_t microSec;
        struct timespec tEnd, t1;
        bool rollOver = false, allSrcsSending = false;
        int sendingSrcCount = 0;

        // Get the current time
        clock_gettime(CLOCK_MONOTONIC, &t1);
        restartTime.tv_sec  = t1.tv_sec;
        restartTime.tv_nsec = t1.tv_nsec;

        // We've got to handle "sourceCount" number of data sources - each with their own stats
        while (true) {

            // Loop for zeroing stats when first starting - for accurate rate calc
            for (int i=0; i < sourceCount; i++) {

                if (dataArrived[i] && (skippedFirst[i] == 1)) {
                    // Data is now coming in. To get an accurate rate, start w/ all stats = 0
                    int src = ids[i];
                    if (debug) std::cerr << "\nData now coming in for src " << src << std::endl;
                    pstats = &allStats[src];
                    currTotalBytes[i]   = pstats->acceptedBytes    = 0;
                    currTotalPkts[i]    = pstats->acceptedPackets  = 0;
                    currBuiltBufs[i]    = pstats->builtBuffers     = 0;

                    currDiscardPkts[i]  = pstats->discardedPackets = 0;
                    currDiscardBytes[i] = pstats->discardedBytes   = 0;
                    currDiscardBufs[i]  = pstats->discardedBuffers = 0;

//                    currTotalBuildTime[i] = pstats->readTime = 0;

                    // Start the clock for this source
                    clock_gettime(CLOCK_MONOTONIC, &tStart[i]);
                    if (debug) fprintf(stderr, "started clock for src %d\n", src);

                    // From now on we skip this zeroing step
                    skippedFirst[i]++;
                }
            }

            for (int i=0; i < sourceCount; i++) {
                prevTotalPkts[i]   = currTotalPkts[i];
                prevTotalBytes[i]  = currTotalBytes[i];
                prevBuiltBufs[i]   = currBuiltBufs[i];

                prevDiscardPkts[i]  = currDiscardPkts[i];
                prevDiscardBytes[i] = currDiscardBytes[i];
                prevDiscardBufs[i]  = currDiscardBufs[i];

//                prevTotalBuildTime[i] = currTotalBuildTime[i];
            }

            // Check to see if we need to quit
            if (endThreads) return;

            // Delay 4 seconds between printouts
            std::this_thread::sleep_for(std::chrono::seconds(4));

            // Read time
            clock_gettime(CLOCK_MONOTONIC, &tEnd);

            // Time taken by last loop
            microSec = (1000000L * (tEnd.tv_sec - t1.tv_sec)) + ((tEnd.tv_nsec - t1.tv_nsec)/1000L);

            for (int i=0; i < sourceCount; i++) {
                // Total time - can be different for each source
                totalMicroSecs[i] = (1000000L * (tEnd.tv_sec - tStart[i].tv_sec)) + ((tEnd.tv_nsec - tStart[i].tv_nsec)/1000L);

                int src = ids[i];
                pstats = &allStats[src];

                // If the total # of built bufs goes down, it's only because the reassembly thread
                // believes the source was restarted and zeroed out the stats. So, in that case,
                // clear everything for that source and start over.
                bool restarted = currBuiltBufs[i] > pstats->builtBuffers;
                if (restarted) {
                    if (debug) std::cerr << "\nLooks like data source " << src << " restarted, clear stuff" << std::endl;
                    prevTotalPkts[i]    = 0;
                    prevTotalBytes[i]   = 0;
                    prevBuiltBufs[i]    = 0;

                    prevDiscardPkts[i]  = 0;
                    prevDiscardBytes[i] = 0;
                    prevDiscardBufs[i]  = 0;

//                    prevTotalBuildTime[i] = 0;

                    // The reassembly thread records when a source is restarted, use that time!
                    totalMicroSecs[i] = (1000000L * (tEnd.tv_sec - restartTime.tv_sec)) + ((tEnd.tv_nsec - restartTime.tv_nsec)/1000L);
                    tStart[i].tv_sec  = restartTime.tv_sec;
                    tStart[i].tv_nsec = restartTime.tv_nsec;
                }

                currTotalBytes[i]   = pstats->acceptedBytes;
                currBuiltBufs[i]    = pstats->builtBuffers;
                currTotalPkts[i]    = pstats->acceptedPackets;

                currDiscardPkts[i]  = pstats->discardedPackets;
                currDiscardBytes[i] = pstats->discardedBytes;
                currDiscardBufs[i]  = pstats->discardedBuffers;

//                currTotalBuildTime[i] = pstats->readTime;

                if (currTotalBytes[i] < 0) {
                    rollOver = true;
                }
            }

            // Don't start calculating stats until data has come in for a full cycle.
            // Keep track of when that starts.
            if (!allSrcsSending) {
                for (int i = 0; i < sourceCount; i++) {
                    if (!dataArrived[i] && currTotalPkts[i] > 0) {
                        dataArrived[i] = true;
                        sendingSrcCount++;

                        if (sendingSrcCount == sourceCount) {
                            allSrcsSending = true;
                        }
                    }
                }
            }


            // Start over tracking bytes and packets if #s roll over
            if (rollOver) {
                for (int i=0; i < sourceCount; i++) {
                    int src = ids[i];
                    pstats = &allStats[src];

                    currTotalBytes[i]   = pstats->acceptedBytes    = 0;
                    currTotalPkts[i]    = pstats->acceptedPackets  = 0;
                    currBuiltBufs[i]    = pstats->builtBuffers     = 0;

                    currDiscardPkts[i]  = pstats->discardedPackets = 0;
                    currDiscardBytes[i] = pstats->discardedBytes   = 0;
                    currDiscardBufs[i]  = pstats->discardedBuffers = 0;

//                    currTotalBuildTime[i] = pstats->readTime = 0;

                    prevTotalPkts[i]    = 0;
                    prevTotalBytes[i]   = 0;
                    prevBuiltBufs[i]    = 0;

                    prevDiscardPkts[i]  = 0;
                    prevDiscardBytes[i] = 0;
                    prevDiscardBufs[i]  = 0;

//                    prevTotalBuildTime[i] = 0;

                    totalMicroSecs[i]   = microSec;
                    printf("Stats ROLLING OVER\n");
                }

                t1 = tEnd;
                rollOver = false;
                continue;
            }

            // Do all stats together?
            if (jointStats && sourceCount > 1) {

                int activeSources = 0;
                byteCount = pktCount = bufCount = 0, readTime = 0;
                discardByteCount = discardPktCount = discardBufCount = 0;
                int64_t totalDiscardBufs = 0L, totalDiscardPkts = 0L, totalDiscardBytes = 0L;
                int64_t totalBytes = 0L, totalBuilt = 0L, totalMicro = 0L, totalPkts = 0L, totalReadTime = 0L, avgMicroSec;

                for (int i = 0; i < sourceCount; i++) {
                    // Data not coming in yet from this source so do NO calcs
                    if (!dataArrived[i]) continue;

                    // Skip first stat cycle as the rate calculations will be off
                    if (skippedFirst[i] < 1) {
                        skippedFirst[i]++;
                        continue;
                    }

                    activeSources++;

                    // Use for instantaneous rates/values
//                    readTime  += currTotalBuildTime[i] - prevTotalBuildTime[i];
                    byteCount += currTotalBytes[i] - prevTotalBytes[i];
                    pktCount  += currTotalPkts[i]  - prevTotalPkts[i];
                    bufCount  += currBuiltBufs[i]  - prevBuiltBufs[i];

                    // Can't tell how many bufs & packets are completely dropped
                    // unless we know exactly what's coming in
                    discardByteCount += currDiscardBytes[i] - prevDiscardBytes[i];
                    discardPktCount  += currDiscardPkts[i]  - prevDiscardPkts[i];
                    discardBufCount  += currDiscardBufs[i]  - prevDiscardBufs[i];

//                    totalReadTime     += currTotalBuildTime[i];
                    totalBytes        += currTotalBytes[i];
                    totalBuilt        += currBuiltBufs[i];
                    totalMicro        += totalMicroSecs[i];
                    totalPkts         += currTotalPkts[i];
                    totalDiscardBufs  += currDiscardBufs[i];
                    totalDiscardPkts  += currDiscardPkts[i];
                    totalDiscardBytes += currDiscardBytes[i];
                }

                if (activeSources > 0) {
                    avgMicroSec = totalMicro/activeSources;

//                    latencyTotalAvg = ((double) totalReadTime) / totalBuilt;
//                    printf("Latency:  %3.4g nanosec,   %3.4g Avg\n", (double)readTime/bufCount, latencyTotalAvg);
//
//                    printf("Latency:  ");
//                    for (int j=0; j < sourceCount; j++) {
//                        int src = ids[j];
//                        double latencyAvg = ((double) currTotalBuildTime[j]) / currBuiltBufs[j];
//                        printf("(%d) %3.4g, ", src, latencyAvg);
//                    }
//                    printf("\n");

                    pktRate    = 1000000.0 * ((double) pktCount) / microSec;
                    pktAvgRate = 1000000.0 * ((double) totalPkts) / avgMicroSec;

                    printf("Packets:  %3.4g Hz,    %3.4g Avg\n", pktRate, pktAvgRate);

                    // Actual Data rates (no header info)
                    dataRate    = ((double) byteCount) / microSec;
                    dataAvgRate = ((double) totalBytes) / avgMicroSec;

                    printf("   Data:  %3.4g MB/s,  %3.4g Avg\n", dataRate, dataAvgRate);

                    // Event rates
                    evRate    = 1000000.0 * ((double) bufCount) / microSec;
                    evAvgRate = 1000000.0 * ((double) totalBuilt) / avgMicroSec;
                    printf(" Events:  %3.4g Hz,    %3.4g Avg, total %" PRId64 "\n",
                            evRate, evAvgRate, totalBuilt);

                    printf("Discard:  %" PRId64 ", (%" PRId64 " total) evts,   pkts: %" PRId64 ", %" PRId64 " total\n\n",
                            discardBufCount, totalDiscardBufs, discardPktCount, totalDiscardPkts);
                }
            }
            else {

                // Do individual stat printouts for each source
                for (int i = 0; i < sourceCount; i++) {
                    // Data not coming in yet from this source so do NO calcs
                    if (!dataArrived[i]) continue;

                    // Skip first stat cycle as the rate calculations will be off
                    if (skippedFirst[i] < 1) {
                        skippedFirst[i]++;
                        continue;
                    }

                    int src = ids[i];
                    pstats = &allStats[src];

                    // Use for instantaneous rates/values
//                    readTime  = currTotalBuildTime[i] - prevTotalBuildTime[i];
                    byteCount = currTotalBytes[i] - prevTotalBytes[i];
                    pktCount  = currTotalPkts[i]  - prevTotalPkts[i];
                    bufCount  = currBuiltBufs[i]  - prevBuiltBufs[i];

                    // Can't tell how many bufs & packets are completely dropped
                    // unless we know exactly what's coming in
                    discardByteCount = currDiscardBytes[i] - prevDiscardBytes[i];
                    discardPktCount  = currDiscardPkts[i]  - prevDiscardPkts[i];
                    discardBufCount  = currDiscardBufs[i]  - prevDiscardBufs[i];

//                    latencyInstAvg  = ((double) readTime) / bufCount;
//                    latencyTotalAvg = ((double) currTotalBuildTime[i]) / currBuiltBufs[i];
//                    printf("%d Latency:  %3.4g nanosec,    %3.4g Avg\n", ids[i], latencyInstAvg, latencyTotalAvg);

                    pktRate = 1000000.0 * ((double) pktCount) / microSec;
                    pktAvgRate = 1000000.0 * ((double) currTotalPkts[i]) / totalMicroSecs[i];

                    printf("%d Packets:  %3.4g Hz,    %3.4g Avg\n", ids[i], pktRate, pktAvgRate);

                    // Actual Data rates (no header info)
                    dataRate = ((double) byteCount) / microSec;
                    dataAvgRate = ((double) currTotalBytes[i]) / totalMicroSecs[i];

                    printf("     Data:  %3.4g MB/s,  %3.4g Avg, bufs %" PRId64 "\n",
                            dataRate, dataAvgRate, pstats->builtBuffers);

                    // Event rates
                    evRate = 1000000.0 * ((double) bufCount) / microSec;
                    evAvgRate = 1000000.0 * ((double) currBuiltBufs[i]) / totalMicroSecs[i];
                    printf("   Events:  %3.4g Hz,    %3.4g Avg, total %" PRIu64 "\n",
                            evRate, evAvgRate, currBuiltBufs[i]);

                    printf("  Discard:    %" PRId64 ", (%" PRId64 " total) evts,   pkts: %" PRId64 ", %" PRId64 " total\n\n",
                            discardBufCount, currDiscardBufs[i], discardPktCount, currDiscardPkts[i]);
                }
            }

            t1 = tEnd;
            restartTime.tv_sec  = t1.tv_sec;
            restartTime.tv_nsec = t1.tv_nsec;
        }
    }



    /**
     * Method implementing a thread to send status updates to the control plane.
     * It uses a PID control function to report an error/control signal along with
     * the queue fill percent.
     */
    void EjfatConsumer::grpcThreadFunc() {

        // Max # of fifo or queue entries available to consumer
        int fifoCapacity = QSIZE;


        // Add stuff to prevent anti-aliasing.
        // If sampling fifo level every N millisec but that level is changing much more quickly,
        // the sent value will NOT be an accurate representation. It will include a lot of noise.
        // To prevent this, keep a running average of the fill %, so its reported value is a more
        // accurate portrayal of what's really going on.

        // Find # of loops (samples) to comprise one reporting period.
        // Report time needs to be integer multiple of sampleTime.
        // Note: sampleTime is in microsec and reportTime is in millisec.

        // sampleTime is adjusted to get close to the actual desired sampling rate
        int adjustedSampleTime = sampleTime;

        // # of loops (samples) to comprise one reporting period =
        int loopMax   = 1000*reportTime / sampleTime;

        // Track # loops made
        int loopCount = loopMax;

        float pidError = 0.F;

        // fcount = number of fill values to average when reporting to grpc.
        // Keep fcount sample times worth (1 sec) of errors so we can use error from 1 sec ago
        // for PID derivative term. For now, fill w/ 0's.
        float oldestPidError, oldPidErrors[fcount];
        memset(oldPidErrors, 0, fcount*sizeof(float));

        // Keep a running avg of fifo fill over fcount samples
        float runningFillTotal = 0., fillAvg;
        int fillValues[fcount];
        memset(fillValues, 0, fcount*sizeof(float));
        // Keep circulating thru array. Highest index is fcount - 1.
        float prevFill, curFill, fillPercent;
        // Set first and last indexes right here
        int currentIndex = 0, earliestIndex = 1;

        // Change to floats for later computations
        float fifoCapacityFlt = static_cast<float>(fifoCapacity);
        float fcountFlt = static_cast<float>(fcount);


        // time stuff
        struct timespec t1, t2;
        int64_t totalTime, time; // microsecs
        int64_t totalTimeGoal = sampleTime * fcount;
        int64_t times[fcount];
        float deltaT; // "time" in secs
        int64_t absTime, prevAbsTime;

        // Get curren time
        clock_gettime(CLOCK_MONOTONIC, &t1);
        // Turn that into microsec epoch time
        prevAbsTime = 1000000L*(t1.tv_sec) + (t1.tv_nsec)/1000L;
        // Load times with current time for more accurate first round of rates
        for (int i=0; i < fcount; i++) {
            times[i] = prevAbsTime;
        }


        while (true) {

            // Check to see if we need to quit
            if (endThreads) return;

            // Delay between data points.
            // sampleTime is adjusted below to get close to the actual desired sampling rate.
            std::this_thread::sleep_for(std::chrono::microseconds(adjustedSampleTime));

            // Read time
            clock_gettime(CLOCK_MONOTONIC, &t2);
            // time diff in microsec
            time = (1000000L * (t2.tv_sec - t1.tv_sec)) + ((t2.tv_nsec - t1.tv_nsec)/1000L);
            // Convert to sec
            deltaT = static_cast<float>(time)/1000000.F;
            // Current epoch time in microsec
            absTime = 1000000L*t2.tv_sec + t2.tv_nsec/1000L;
            t1 = t2;


            // Keep count of total time taken for last fcount periods.

            // Record current time
            times[currentIndex] = absTime;
            // Subtract from that the earliest time to get the total time in microsec
            totalTime = absTime - times[earliestIndex];
            // Keep things from blowing up if we've goofed somewhere
            if (totalTime < totalTimeGoal) totalTime = totalTimeGoal;
            // Get oldest pid error for calculating PID derivative term
            oldestPidError = oldPidErrors[earliestIndex];


            // Read current fifo fill level
            curFill = static_cast<float>(queueSize);


            // Previous value at this index
            prevFill = fillValues[currentIndex];
            // Store current val at this index
            fillValues[currentIndex] = curFill;
            // Add current val and remove previous val at this index from the running total.
            // That way we have added loopMax number of most recent entries at ony one time.
            runningFillTotal += curFill - prevFill;

            // Under crazy circumstances, runningFillTotal could be < 0 !
            // Would have to have high fill, then IMMEDIATELY drop to 0 for about a second.
            // This would happen at very small input rate as otherwise it takes too much
            // time for events in q to be processed and q level won't drop as quickly
            // as necessary to see this effect.
            // If this happens, set runningFillTotal to 0 as the best approximation.
            if (runningFillTotal < 0.) {
//std::cerr << "NEG runningFillTotal (" << runningFillTotal << "), set to 0!!" << std::endl;
                runningFillTotal = 0.;
            }

            fillAvg = runningFillTotal / fcountFlt;
            fillPercent = fillAvg / fifoCapacityFlt;
            pidError = pid<float>(setPoint, fillPercent, deltaT, Kp, Ki, Kd, oldestPidError, totalTime);

            // Track pid error
            oldPidErrors[currentIndex] = pidError;

            // Set indexes for next round
            earliestIndex++;
            earliestIndex = (earliestIndex == fcount) ? 0 : earliestIndex;

            currentIndex++;
            currentIndex = (currentIndex == fcount) ? 0 : currentIndex;

            if (currentIndex == 0) {
                // Use totalTime to adjust the effective sampleTime so that we really do sample
                // at the desired rate. This accounts for all the computations
                // that this code does which slows down the actual sample rate.
                // Do this adjustment once every fcount samples.
                float factr = totalTimeGoal/totalTime;
                adjustedSampleTime = sampleTime * factr;

                // If totalTime, for some reason, is really big, we don't want the adjusted time to be 0
                // since a sleep_for(0) is very short. However, sleep_for(1) is pretty much the same as
                // sleep_for(500).
                if (adjustedSampleTime == 0) {
                    adjustedSampleTime = 500;
                }

//std::cerr << "sampleTime = " << adjustedSampleTime << ", totalT = " << totalTime << std::endl;
            }


            // Every "loopMax" loops
            if (--loopCount <= 0) {

                if (debug) std::cout << "update fill% and pidError" << std::endl;
                // Update the changing variables
                LbClient->update(fillPercent, pidError);

                if (debug) std::cout << "send state to CP" << std::endl;
                // Send to server
                int err = LbClient->SendState();
                if (err == 1) {
                    std::cerr << "GRPC client " << myName << " communication error during sending state, exit" << std::endl;
                    exit(1);
                }

                // Print out every 4 sec
                if (absTime - prevAbsTime >= 4000000) {
                    prevAbsTime = absTime;
                    fprintf(stdout, "     Fifo level %d  Avg:  %.2f,  %.2f%%,  pid err %f\n\n",
                           ((int)curFill), fillAvg, (100.F*fillPercent), pidError);
                }

                loopCount = loopMax;
            }
        }
    }



    /**
     * Method to start up the grpc thread.
     * It won't start up more than one.
     */
    void EjfatConsumer::startupGrpcThread() {
        if (grpcThdStarted) return;

        std::thread t(&EjfatConsumer::grpcThreadFunc, this);
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

        if (debug) std::cerr << "in recv thd for src " << srcId << std::endl;

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

        // Need to figure out if source was killed and restarted.
        // If this is the case, then the tick # will be much smaller than the previous value.
        uint64_t tick = 0xffffffffffffffffL, prevTick, largestTick = 0;
        bool restarted = false, firstLoop = true;
        uint16_t dataId;

   //     auto queue        = queues[srcId];
        auto & qItemVec   = qItemVectors[srcId];
        int currentQItem  = currentQItems[srcId];
        int dataSocket    = dataSockets[srcId];
        auto stats        = &allStats[srcId];
        bool takeStats    = stats != nullptr;


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

            // Check to see if we need to quit
            if (endThreads) return;

            //TODO: this calls recvfrom which is blocking since dataSocket is
            // blocking. For nonblocking behavior one can modify the socket:
            //        int enable = 1;
            //        setsockopt(socket_fd, SOL_SOCKET, SO_NONBLOCK, &enable, sizeof(enable));
            // Perhaps a timeout behavior can be implemented
            ssize_t nBytes = getCompleteAllocatedBuffer(&dataBuf, &bufSize, dataSocket,
                                                        debug, &tick, &dataId, stats, 1);

            if (nBytes < 0) {
                std::cerr << "Error in receiving data, " << nBytes << std::endl;
                return;
            }

            // Check to see if dataId is the source we're expecting
            if (dataId != srcId) {
                // Big problems! Expecting source id = srcId, but getting dataId
                std::cerr << "Error receiving, got src " << dataId << ", but expecting " << srcId << std::endl;
                return;
            }

            // The getCompleteAllocatedBuffer routine may have allocated memory.
            // So make sure we store its location and size in Q item.
            item->event     = dataBuf;
            item->bufBytes  = bufSize;
            item->dataBytes = nBytes;
            item->srcId     = dataId;

            // Place onto queue
            if (queue->push(item)) {
                currentQItem = (currentQItem + 1) % VECTOR_SIZE;
                // Need to manually track this for boost::lockfree:queue
                queueSize++;
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
                restartTime.tv_sec  = now.tv_sec;
                restartTime.tv_nsec = now.tv_nsec;

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
     * Non-blocking retrieval of event sent from one of the expected data sources.
     * Once an item is taken off the internal queue by this method, room is
     * created for receiving threads to place another item onto it. If
     * long term access to the return event is desired, the caller
     * must copy it.
     * <p>
     * Note: it's possible, but unlikely, that the user calls this method
     * successively more times than the max number of simultaneously available
     * events (1023) while continuing to access the event returned by the first
     * call. This might result in the receiving thread overwriting that first event
     * while it's being used.
     * </p>
     *
     * @param event      pointer filled with pointer to event data.
     * @param bytes      pointer filled with event size in bytes.
     * @param eventNum   pointer filled with event number.
     * @param srcId      pointer filled with id of data source.
     *
     * @return true if event retrieved from queue, else false. If false, the queue
     *         may be empty or the source id may not be valid for this consumer.
     */
    bool EjfatConsumer::getEvent(char **event, size_t *bytes, uint64_t* eventNum, uint16_t *srcId) {

        qItem *item;

        // Dequeue event
        if (!queue->pop(item)) return false;

        // Need to manually track this for boost::lockfree:queue
        queueSize--;

        if (bytes    != nullptr) *bytes    = item->bufBytes;
        if (srcId    != nullptr) *srcId    = item->srcId;
        if (eventNum != nullptr) *eventNum = item->tick;
        if (event != nullptr && *event != nullptr) *event = item->event;

        return true;
    }


}


