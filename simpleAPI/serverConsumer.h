//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 6/03/24.
//

#ifndef EJFAT_SERVER_CONSUMER_H
#define EJFAT_SERVER_CONSUMER_H

#include <cstdint>
#include <cstdlib>
#include <cinttypes>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unordered_map>
#include <atomic>



#include "ejfat.hpp"
#include "ejfat_assemble_ersap.hpp"
#include "EjfatException.h"
#include "simpleEjfat.h"

#include <boost/lockfree/queue.hpp>

namespace ejfat {


    /**
     * This class defines an Ejfat data or event consumer.
     *
     * <p>
     * There can only be one 1 thread which reads from a single socket in
     * order to receive events from a single source.
     *
     * To implement this, a dedicated reading thread is run for each data source.
     * Events are placed on internal queues - one for each source. All this is
     * transparent to the user.
     * </p>
     *
     * <p>
     * Another thread is started to print out statistics.
     * </p>
     *
     * <p>
     * The last thread communicates with a "simple server" which, in turn, talks
     * to its associated control plane. Thus this consumer does not talk gRPC
     * which is handled by the server. The CP is instructed to pass data to this
     * consumer.
     * </p>
     *
     * @date 06/04/2024
     * @author timmer
     */
    class serverConsumer {


    private:


        /** If true, print out debugging info to console. */
        bool debug;

        /** Ids of data sources sending to this consumer. */
        std::vector<int> ids;

        /**
         * Starting core # to run receiving threads on.
         * If < 0, don't use thread affinity.
         */
        int startingCore = -1;

        /** Number of cores per receiving thread. */
        int coreCount = 1;

        /**
         * Initial weight of this consumer, compared to other consumers,
         * which gives the CP a hint as to the initial distribution of events.
         * The absolute value of the weight is not meaningful, only its value
         * in relation to other consumers.
         */
        float weight = 1.F;

        /** Factor for setting min # of Cp slot assignments. */
        float minFactor = 0.F;

        /** Factor for setting max # of Cp slot assignments. */
        float maxFactor = 0.F;

        //------------------------------------------------------------------
        // Network stuff
        //------------------------------------------------------------------

        /** If true, receive data directly from a sender,
        *  bypassing the LB and not talking to the CP or simple-server. */
        bool direct;
        /** If true, call connect() on socket to server. */
        bool connectSocket = false;

        /** Address of simple server to talk to. */
        std::string serverAddr;

        /** Port of simple server. */
        uint16_t serverPort = 18300;

        /** Local IP address (dotted-decimal form) to recv data on. */
        std::string dataAddr;

        /** Starting local UDP port to recv data on. Increase for each incoming data source. */
        uint16_t dataPort = 17750;

        /** UDP sockets for receiving data from various sources. */
        std::unordered_map<int, int> dataSockets;

        /** Socket for talking to simple server. */
        int serverSocket;

        /** Structure for server connection, IPv4. */
        struct sockaddr_in  serverAddrStruct;
        /** Structure for server connection, IPv6. */
        struct sockaddr_in6 serverAddrStruct6;


        /**
         * IP address for UDP sockets to listen on.
         * If not set, sockets will bind to INADDR_ANY.
         * Currently unused.
         */
        std::string listeningAddr;

        /** If true, use IP version 6 for data address, else use version 4. */
        bool ipv6DataAddr = false;

        /** If true, use IP version 6 for server address, else use version 4. */
        bool ipv6ServerAddr = false;

        /** Size in bytes of UDP data socket's recv buffer. */
        int udpRecvBufSize = 25000000;

        //------------------------------------------------------------------
        // PID loop stuff
        //------------------------------------------------------------------

        /**
         * PID proportional constant set through trial and error. The PID loop is
         * used to produce an error signal to the CP which allows the CP to distribute
         * events in a balanced manner.
         */
        float Kp = 0.;

        /**
         * PID integral constant set through trial and error used to produce an error
         * signal to the CP.
         */
        float Ki = 0.;

        /**
         * PID derivative constant set through trial and error used to produce an error
         * signal to the CP.
         */
        float Kd = 0.; // 1000x normal

        /** Set point of PID loop, goal of fifo level-setting. */
        float setPoint = 0.;

        /** Number of fill values to average when reporting to grpc. */
        int fcount = 1000;

        /** Time period in millisec for reporting to CP. */
        int reportTime = 1000;

        /** Stat sampling time in microsec. */
        uint32_t sampleTime = 1000;

        /** Set this manually to always say we're ready for data. */
        bool isReady = true;

        //------------------------------------------------------------------
        // Statistics stuff
        //------------------------------------------------------------------

        // Statistics
        volatile uint64_t totalBytes=0, totalPackets=0, totalEvents=0;

        volatile struct timespec restartTime;
        //std::unordered_map<int, packetRecvStats> allStats;
        std::unordered_map<int, std::shared_ptr<packetRecvStats>> allStats;

        bool jointStats = false;


        /**
         * If a source dies and restarts, get a handle on when it actually restarted
         * so the stats/rates can be reset properly.
         */

        //------------------------------------------------------------------
        // Thread stuff
        //------------------------------------------------------------------

        /** Structure to pass to each receiving thread. */
        typedef struct recvThdArg_t {
            int srcId;
            int core;
            int coreCount;
        } recvThdArg;


        /** Thread to do statistics. */
        std::thread statThread;

        /** Thread for talking to the EJFAT control plane thru the simple server. */
        std::thread serverThread;

        /** 1 recv thread for each incoming data source. Map key = src id. */
        std::unordered_map<int, std::thread> recvThreads;

        /** Has the statistics thread been started? */
        volatile bool statThdStarted = false;

        /** Have all the receiving threads been started? */
        volatile bool recvThdsStarted = false;

        /** Have all the send threads been started? */
        volatile bool serverThdStarted = false;

        /** Flag used to stop threads. */
        std::atomic_bool endThreads;

        //------------------------------------------------------------------
        // Queue stuff
        //------------------------------------------------------------------

        /** Class defining each internal queue item. */
        class qItem {

          public:
            /** Id of the source of this event. */
            uint16_t srcId;
            /** Tick or event number of event contained. */
            uint64_t tick;
            /** Size in bytes of the complete buffer pointed to by event. */
            size_t   bufBytes;
            /** Size in bytes of the data contained in event (dataBytes <= bufBytes). */
            size_t   dataBytes;
            /** Pointer to the start of the event data. */
            char*    event;

            qItem() {
                srcId=0;tick = 0;bufBytes=0;dataBytes=0;event=nullptr;
            }
        };


        /** Max size of internal queue for holding events (qItem) received from all data sources. */
        static const size_t QSIZE = 1023;

        /**
         * Size of vector containing elements, from a single source, that can be placed on the queue.
         * We want the vector to be fixed in size and bigger than the Q.
         * If the Q is full, we still want access to at least one unused vector
         * element which the caller can fill and wait for it to be placed on the Q.
         */
        static const size_t VECTOR_SIZE = QSIZE + 1;

        /**
         * Fast, lock-free, queue for multiple producers and consumers.
         * One queue shared by all data sources.
         * Each qItem essentially wraps an event and comes from a vector of qItems.
         */
        std::shared_ptr<boost::lockfree::queue<qItem*, boost::lockfree::capacity<QSIZE>>> queue;

        /**
         * Each vector of qItems contains the items available to be written into by a
         * single receiving thread. Once written into, item is placed in the single queue.
         * One vector for each data source. Map key = src id.
         */
        std::unordered_map<int, std::vector<qItem>> qItemVectors;

        /**
         * Track which element of a qItem vector, from a single source,
         * is currently being placed onto Q (0 - (VECTOR_SIZE-1)).
         * One index for each data source. Map key = src id.
         */
        std::unordered_map<int, int> currentQItems;

        /** The boost::lockfree::queue does NOT track its size (boo!) so do it manually. */
        std::atomic_int queueSize;




    public:

        // Data direct from sender, bypass LB, don't talk to CP
        explicit serverConsumer(uint16_t dataPort = 17750,
                               const std::vector<int> &ids = {0},
                               bool debug = false, bool jointStats = false,
                               int startingCore = -1, int coreCount = 1);

        // Multiple data sources using connection to simple server.
        serverConsumer(const std::string &serverAddr, const std::string &dataAddr,
                      uint16_t serverPort = 18300, uint16_t dataPort = 17750,
                      const std::vector<int> &ids = {0},
                      bool debug = false, bool jointStats = false, bool connect = false,
                      int startingCore = -1, int coreCount = 1,
                      float minFactor=0.F, float maxFactor=0.F,
                      float Kp=520., float Ki=5., float Kd=0.,
                      float setPt=0., float weight=1.);

        // No copy constructor
        serverConsumer(const serverConsumer & item) = delete;

        // Destructor to shutdown threads
        ~serverConsumer();

        // No assignment operator
        serverConsumer & operator=(const serverConsumer & other) = delete;

        // Non-blocking call to get events
        bool getEvent(char **event, size_t *bytes, uint64_t* eventNum, uint16_t *srcId);


    private:

        bool registerWithSimpleServer();
        bool deregisterWithSimpleServer();
        bool updateSimpleServer(float fill, float pidErr, bool isReady);

        void createServerSocket();
        void createDataSockets();

        void statisticsThreadFunc();
        void startupStatisticsThread();

        void startupServerThread();
        void serverThreadFunc();

        void recvThreadFunc(recvThdArg *arg);
        void startupRecvThreads();

        void createSocketsAndStartThreads();
    };



}


#endif // EJFAT_SERVER_CONSUMER_H
