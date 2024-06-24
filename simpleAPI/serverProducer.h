//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100

//
// Created by timmer on 6/05/24.
//

#ifndef EJFAT_SERVER_PRODUCER_H
#define EJFAT_SERVER_PRODUCER_H

#include <cstdint>
#include <cstdlib>
#include <cinttypes>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <atomic>


#include "ejfat.hpp"
#include "simpleEjfat.h"
#include "ejfat_packetize.hpp"
#include "EjfatException.h"

#include <boost/lockfree/spsc_queue.hpp>


// Example callback function
static void* callbackFuncExample(void *arg) {
    std::cout << "Callback function called" << std::endl;
    return arg;
}



namespace ejfat {


    /**
     * This class defines an Ejfat data or event sender.
     *
     * <p>
     * One 1 thread which sends events to either the simple server or directly to a consumer.
     * </p>
     *
     * <p>
     * Another thread is started to print out statistics.
     * </p>
     *
     * <p>
     * Last thread takes events off an internal queue
     * and sends them to the server to implement non-blocking send.
     * </p>
     *
     * @date 06/05/2024
     * @author timmer
     */
    class serverProducer {


    private:

        /** If true, print out debugging info to console. */
        bool debug = false;

        /** Id of this data source. */
        uint16_t id = 0;

        /** Starting event number for LB header in a UDP packet. */
        uint64_t tick = 0;

        /** Delay in microseconds between each event sent. */
        int delay = 0;

        /** The delay prescale (1,2, ... N). A delay is taken for every Nth event. */
        int delayPrescale = 1;

        /** Helps implement the delay prescale. */
        int delayCounter = 1;

        /** Version of Ejfat. */
        int version = 2;

        /** Version of the Ejfat protocol. */
        int protocol = 1;

        /**
         * Entropy of this sender. The number to add to the destination port
         * for a given destination host. Used on receiving end to read different sources
         * on different UDP ports for multithreading and ease of programming purposes.
         */
        int entropy = 0;

        /** Last time a sync message sent to CP in nanosec since epoch. */
        uint64_t lastSyncTimeNanos;

        /** Number of events sent since last sync message sent to CP. */
        uint64_t eventsSinceLastSync;

        /** Buffer in which to create and store the sync message to the CP. */
        char syncBuf[28];

        //------------------------------------------------------------------
        // Statistics stuff
        //------------------------------------------------------------------

        volatile uint64_t totalBytes=0, totalPackets=0, totalEvents=0;

        //------------------------------------------------------------------
        // Network stuff
        //------------------------------------------------------------------

        /** Size in bytes of UDP socket's send buffer. */
        int udpSendBufSize = 25000000;

        /** Maximum Transmission Unit (max byte size of each UDP packet. */
        int mtu = 9000;

        /** Max number of data bytes to store in a single UDP packet (depends on MTU). */
        int maxUdpPayload = 9000 - 20 - 8 - HEADER_BYTES;


        /** IP address (dotted-decimal form) to send data to.
         * If direct = true, this is the data IP addr of the consumer. */
        std::string serverAddr;
        /** UDP port to send data to.
         * If direct = true, this is the data port of the consumer. */
        uint16_t serverPort = 19500;


        /** If true, use IP version 6 for LB data address, else use version 4. */
        bool ipv6Data = false;


        /** If true, bypass LB and send data directly to consumer (serverAddr, serverPort). No sync necessary. */
        bool direct = false;
        /** If true, call connect() for both sync and data sockets. */
        bool connectSocket = false;

        /** UDP socket for sending data to simple server or direct to consumer. */
        int serverSocket;


        /** Structure for socket connection, IPv4. */
        struct sockaddr_in  sendAddrStruct;
        /** Structure for socket connection, IPv6. */
        struct sockaddr_in6 sendAddrStruct6;


        //------------------------------------------------------------------
        // Thread stuff
        //------------------------------------------------------------------

        /** Vector used to store the numbers of the cores to run this sender on. */
        std::vector<int> cores;

        /** Thread to do statistics. */
        std::thread statThread;
        /** Thread to take things from internal queue (if any) and send. */
        std::thread sendThread;

        /** Has the statistics thread been started? */
        volatile bool statThdStarted = false;
        /** Has the send thread been started? */
        volatile bool sendThdStarted = false;

        /** Flag used to stop threads. */
        std::atomic_bool endThreads;

        //------------------------------------------------------------------
        // Queue stuff
        //------------------------------------------------------------------
        // Why would you want a bounded queue? If you put data buffers on it faster than it can be
        // sent, you have a memory leak. If there's any kind of delay specified, then
        // you'll need to throttle putting items onto the Q.
        //
        // Question: How do you know if what you put on the Q has been sent and its pointer made
        // available for possible reuse?
        // Answer: Use the addToSendQueue() method and specify a callback and arg to be run when
        // it's been sent.


        /** Structure to hold each send-queue item. */
        typedef struct queueItem_t {
            uint32_t bytes;
            uint64_t tick;
            char     *event;
            void     *cbArg;
            void* (*callback)(void *);
        } qItem;


        /** Max size of internal queue holding events to be sent. */
        static const size_t QSIZE = 2047;

        /**
         * Size of array containing elements that can be placed on the queue.
         * We want the array to be bigger than the Q since if the Q is full, we still want
         * access to at least one unused array element which the caller can fill and
         * wait for it to be placed on the Q.
         */
        static const size_t ARRAYSIZE = QSIZE + 1;

        /** Fast, lock-free, wait-free queue for single producer and single consumer. */
        boost::lockfree::spsc_queue<qItem*, boost::lockfree::capacity<QSIZE>> queue;

        /** Array of qItems available to be stored on queue. Allocate ahead of time. */
        qItem qItemArray[ARRAYSIZE];

        /** Track which element of qItemArray is currently being placed onto Q (0 - 2047). */
        int currentQItem = 0;



    public:

        serverProducer(const std::string& serverAddress,
                      uint16_t serverPort = 19500,
                      bool direct = false, bool debug = false,
                      uint16_t id = 0, int entropy = 0,
                      int delay = 0, int delayPrescale = 1,
                      bool connect = false, int mtu = 9000,
                      const std::vector<int>& cores = {},
                      int version = 2, int protocol = 1);


        // No copy constructor
        serverProducer(const serverProducer & item) = delete;

        // Destructor to shutdown threads
        ~serverProducer();

        // No assignment operator
        serverProducer & operator=(const serverProducer & other) = delete;



        // Blocking call. Event number automatically set.
        // Any core affinity needs to be done by caller.
        void sendEvent(char *event, size_t bytes);

        // Blocking call specifying event number.
        void sendEvent(char *event, size_t bytes, uint64_t eventNumber);


        // Non-blocking call to place event on internal queue.
        // Event number automatically set. Returns false if queue full.
        bool addToSendQueue(char *event, size_t bytes,
                            void* (*callback)(void *) = nullptr, void *cbArg = nullptr);

        // Non-blocking call specifying event number.
        bool addToSendQueue(char *event, size_t bytes, uint64_t eventNumber,
                            void* (*callback)(void *) = nullptr, void *cbArg = nullptr);


    private:

        void statisticsThreadFunc(void *arg);
        void startupStatisticsThread();

        void sendThreadFunc(void *arg);
        void startupSendThread();

        void createServerSocket();
        void createSyncSocket();
        void createDataSocket();

        void createSocketsAndStartThreads();

    };



}


#endif // EJFAT_SERVER_PRODUCER_H
