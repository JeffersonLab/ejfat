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
#include <atomic>
#include <unordered_map>
#include <array>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>


#include "ejfat.hpp"
#include "ejfat_assemble_ersap.hpp"
#include "EjfatException.h"
#include "lb_cplane.h"

#include <boost/lockfree/spsc_queue.hpp>

namespace ejfat {


    /**
     * This class defines an Ejfat data or event consumer.
     *
     * @date 04/02/2024
     * @author timmer
     */
    class EjfatConsumer {


    private:


        /** Structure to hold each internal queue item. */
        typedef struct queueItem_t {
            /** Tick or event number of event contained. */
            uint64_t tick;
            /** Size in bytes of the complete buffer pointed to by event. */
            size_t   bufBytes;
            /** Size in bytes of the data contained in event (dataBytes <= bufBytes). */
            size_t   dataBytes;
            /** Pointer to the start of the event data. */
            char*    event;
        } qItem;


        /** Structure to pass to each receiving thread. */
        typedef struct recvThdArg_t {

            // shared ptr of stats
            std::shared_ptr<packetRecvStats> stats;

            int srcId;
            int core;
            int coreCount;

        } recvThdArg;




        // Statistics
        volatile uint64_t totalBytes=0, totalPackets=0, totalEvents=0;

        std::string cpName;

        /** IP address (dotted-decimal form) to talk to CP with. */
        std::string cpAddr;
        /** UDP port to talk to CP with. */
        uint16_t cpPort = 18347;

        /** Local IP address (dotted-decimal form) to recv data on. */
        std::string dataAddr;

        /** Starting local UDP port to recv data on. Increase for each incoming data source. */
        uint16_t dataPort = 17750;


        /**
         * IP address for UDP sockets to listen on.
         * If not set, sockets will bind to INADDR_ANY.
         */
        std::string listeningAddr;


        std::string instanceToken;
        std::string lbId;

        float Kp = 0., Ki = 0., Kd = 0.; // 1000x normal
        float weight = 1.;




        /** If true, use IP version 6 for data address, else use version 4. */
        bool ipv6DataAddr = false;


        /** UDP sockets for receiving data from various sources. */
        std::unordered_map<int, int> dataSockets;


        /** If true, print out debugging info to console. */
        bool debug = false;

        /** Ids of data sources sending to this consumer. */
        std::vector<int> ids;

        /** Size in bytes of UDP data socket's recv buffer. */
        int udpRecvBufSize = 25000000;

        /** Buffer in which to create and store the sync message to the CP. */
        char syncBuf[28];

        /** Starting core # to run receiving threads on. */
        int startingCore = -1;

        /** Number of cores per receiving thread. */
        int coreCount = 1;


        /** Thread to do statistics. */
        std::thread statThread;





        /** 1 recv thread for each incoming data source. Map key = src id. */
        std::unordered_map<int, std::thread> recvThreads;

        /** Has the statistics thread been started? */
        volatile bool statThdStarted = false;

        /** Hava all the send thread been started? */
        volatile bool recvThdsStarted = false;

        /** Flag used to stop threads. */
        std::atomic_bool endThreads;

        //------------------------------------------------------------------
        // Queue stuff
        //------------------------------------------------------------------

        /** Max size of each internal queue for holding events received for each data source. */
        static const size_t QSIZE = 511;

        /**
         * Size of array containing elements that can be placed on the queue.
         * We want the array to be bigger than the Q since if the Q is full, we still want
         * access to at least one unused array element which the caller can fill and
         * wait for it to be placed on the Q.
         */
        static const size_t ARRAYSIZE = QSIZE + 1;


        /**
         * Fast, lock-free, wait-free queue for single producer and single consumer.
         * One queue for each data source. Map key = src id. Consuming must be from
         * a single thread only (producer already is single threaded).
         */
        std::unordered_map<int, boost::lockfree::spsc_queue<qItem*, boost::lockfree::capacity<QSIZE>>> queues;

        /** Each array of qItems is available to be stored on a single queue.
         * One array for each data source. Map key = src id. */
        std::unordered_map<int, qItem[ARRAYSIZE]> qItemArrays;

        /** Track which element of a qItem array is currently being placed onto Q (0 - 511).
         * One index for each data source. Map key = src id. */
        std::unordered_map<int, int> currentQItems;

        std::unordered_map<int, packetRecvStats> allStats;

        struct timespec restartTime;


    public:


//        // Single data source
//        EjfatConsumer(const std::string &dataAddr, const std::string &cpAddr, int srcId=0,
//                      const std::string& envVar = "EJFAT_URI",
//                      const std::string& fileName = "/tmp/ejfat_uri",
//                      float Kp=0., float Ki=0., float Kd=0., float weight=1.);

        // Multiple data sources
        EjfatConsumer(const std::string &dataAddr, const std::string &cpAddr,
                      const std::vector<int> &ids = {0},
                      const std::string& envVar = "EJFAT_URI",
                      const std::string& fileName = "/tmp/ejfat_uri",
                      uint16_t dataPort = 17750, uint16_t cpPort = 18347,
                      int startingCore = -1, int coreCount = 1,
                      float Kp=0., float Ki=0., float Kd=0., float weight=1.);


        // No copy constructor
        EjfatConsumer(const EjfatConsumer & item) = delete;

        // Destructor to shutdown threads
        ~EjfatConsumer();

        // No assignment operator
        EjfatConsumer & operator=(const EjfatConsumer & other) = delete;



// There can only be one 1 thread which reads from the socket.
// Thus either we have a single blocking call or use a queue.
// The single blocking call does NOT work in general (unless you
// want to spin up a thread just to read, then place it into your
// own queue). But let's just build this in to start with.
// That way we can control all the fifo fill level, PID stuff
// and the user never needs to see it.


        // Non-blocking call
        bool getEvent(char **event, size_t *bytes, uint64_t* eventNum, int srcId);

        //TODO: I think we end up with the same issue, but in reverse.
        // The problem is that the queue doesn't know when the caller
        // is done with the event so its memory can be reused.
        // Perhaps each successive getEvent will free the previous buffer.
        // Do we have an additional freeEvent() call??
        // Can we increase the internal buffer size if it's too small??

        //TODO: return an event id # which is then used in freeEvent(evId, srcId); or is char *event enough ??

        void freeEvent(char *event, int srcId);

    private:

        bool setFromURI(ejfatURI & uri);

        void statisticsThreadFunc(void *arg);
        void startupStatisticsThread();

        void recvThreadFunc(void *arg);
        void startupRecvThreads();

        void createDataSockets();

        void createSocketsAndStartThreads();

    };



}


#endif //EJFAT_EJFATCONSUMER_H
