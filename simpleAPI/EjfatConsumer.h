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

#ifndef EJFAT_EJFATCONSUMER_H
#define EJFAT_EJFATCONSUMER_H

#include <cstdint>
#include <cstdlib>
#include <cinttypes>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <array>
#include <atomic>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>


#include "ejfat.hpp"
#include "ejfat_assemble_ersap.hpp"
#include "EjfatException.h"
#include "lb_cplane.h"

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
     * Another thread is started to print out statistics. And finally, there is a
     * thread which does the communication with the EJFAT control plane.
     * </p>
     *
     * @date 04/02/2024
     * @author timmer
     */
    class EjfatConsumer {


    private:


        /** If true, print out debugging info to console. */
        bool debug;

        /** If true, receive data directly from a sender,
         *  bypassing the LB and not talking to the CP. */
        bool direct;

        /** Ids of data sources sending to this consumer. */
        std::vector<int> ids;

        /**
         * Starting core # to run receiving threads on.
         * If < 0, don't use thread affinity.
         */
        int startingCore = -1;

        /** Number of cores per receiving thread. */
        int coreCount = 1;


        //------------------------------------------------------------------
        // Network stuff
        //------------------------------------------------------------------

        /** Local IP address (dotted-decimal form) to recv data on. */
        std::string dataAddr;

        /** Starting local UDP port to recv data on. Increase for each incoming data source. */
        uint16_t dataPort = 17750;

        /** UDP sockets for receiving data from various sources. */
        std::unordered_map<int, int> dataSockets;

        /**
         * IP address for UDP sockets to listen on.
         * If not set, sockets will bind to INADDR_ANY.
         * Currently unused.
         */
        std::string listeningAddr;

        /** If true, use IP version 6 for data address, else use version 4. */
        bool ipv6DataAddr = false;

        /** Size in bytes of UDP data socket's recv buffer. */
        int udpRecvBufSize = 25000000;

        //------------------------------------------------------------------
        // Control Plane stuff
        //------------------------------------------------------------------

        /** IP address (dotted-decimal form) to talk to CP with. */
        std::string cpAddr;

        /** UDP port to talk to CP with. */
        uint16_t cpPort = 18347;

        /** Name of this consumer as given to CP, generated internally, */
        std::string myName;

        /** Token with which to register with the CP. */
        std::string instanceToken;

        /** Id of the LB reserved for use by this consumer. */
        std::string lbId;

        /**
         * PID proportional constant set through trial and error. The PID loop is
         * used to produce an error signal to the CP which allows the CP to distribute
         * events in a balanced manner.
         */
        float Kp = 0.F;

        /**
         * PID integral constant set through trial and error used to produce an error
         * signal to the CP.
         */
        float Ki = 0.F;

        /**
         * PID derivative constant set through trial and error used to produce an error
         * signal to the CP.
         */
        float Kd = 0.F; // 1000x normal

        /**
         * Initial weight of this consumer, compared to other consumers,
         * which gives the CP a hint as to the initial distribution of events.
         * The absolute value of the weight is not meaningful, only its value
         * in relation to other consumers.
         */
        float weight = 1.F;

        /** Set point of PID loop, goal of fifo level-setting. */
        float setPoint = 0.F;

        /** Factor for setting min # of CP slot assignments. */
        float minFactor = 0.F;

        /** Factor for setting max # of CP slot assignments. */
        float maxFactor = 0.F;

        /** Number of fill values to average when reporting to grpc. */
        int fcount = 1000;

        /** Time period in millisec for reporting to CP. */
        int reportTime = 1000;

        /** Stat sampling time in microsec. */
        uint32_t sampleTime = 1000;

        /**
         * Object used to interact with the CP to register, update, and deregister.
         * Using shared_ptr makes it easier to declare object here and created it later.
         */
        std::shared_ptr<LbControlPlaneClient> LbClient;

        //------------------------------------------------------------------
        // Statistics stuff
        //------------------------------------------------------------------

        // Statistics
        volatile uint64_t totalBytes=0, totalPackets=0, totalEvents=0;

        volatile struct timespec restartTime;
        std::unordered_map<int, packetRecvStats> allStats;

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

        /** Thread for talking to the EJFAt control plane. */
        std::thread grpcThread;

        /** 1 recv thread for each incoming data source. Map key = src id. */
        std::unordered_map<int, std::thread> recvThreads;

        /** Has the statistics thread been started? */
        volatile bool statThdStarted = false;

        /** Hava all the send thread been started? */
        volatile bool recvThdsStarted = false;

        /** Hava all the send thread been started? */
        volatile bool grpcThdStarted = false;

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
         * <p>
         * Fast, lock-free, queue for multiple producers and consumers.
         * One queue shared by all data sources.
         * Each qItem essentially wraps an event and comes from a vector of qItems.
         * </p>
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
        explicit EjfatConsumer(uint16_t dataPort = 17750,
                               const std::vector<int> &ids = {0},
                               bool debug = false, bool jointStats = false,
                               int startingCore = -1, int coreCount = 1);

        // Multiple data sources
        EjfatConsumer(const std::string &dataAddr, uint16_t dataPort = 17750,
                      const std::vector<int> &ids = {0},
                      const std::string& uri = "",
                      const std::string& fileName = "/tmp/ejfat_uri",
                      bool debug = false, bool jointStats = false,
                      int startingCore = -1, int coreCount = 1,
                      float minFactor = 0., float maxFactor = 0.,
                      float Kp=520., float Ki=5., float Kd=0.,
                      float setPt=0., float weight=1.);


        // No copy constructor
        EjfatConsumer(const EjfatConsumer & item) = delete;

        // Destructor to shutdown threads
        ~EjfatConsumer();

        // No assignment operator
        EjfatConsumer & operator=(const EjfatConsumer & other) = delete;

        // Non-blocking call to get events
        bool getEvent(char **event, size_t *bytes, uint64_t* eventNum, uint16_t *srcId);


    private:

        bool setFromURI(ejfatURI & uri);

        void statisticsThreadFunc();
        void startupStatisticsThread();

        void startupGrpcThread();
        void grpcThreadFunc();

        void recvThreadFunc(recvThdArg *arg);
        void startupRecvThreads();

        void createDataSockets();
        void createSocketsAndStartThreads();
    };



}


#endif //EJFAT_EJFATCONSUMER_H
