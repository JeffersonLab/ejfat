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

#ifndef EJFAT_EJFATSERVER_H
#define EJFAT_EJFATSERVER_H

#include <cstdint>
#include <cstdlib>
#include <cinttypes>
#include <unistd.h>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <set>
#include <fstream>
#include <unordered_map>
#include <array>
#include <atomic>
#include <stdexcept>
#include <map>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>


#include "simpleEjfat.h"
#include "ejfat.hpp"
#include "ejfat_packetize.hpp"
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
    class EjfatServer {


    private:


        /** If true, print out debugging info to console. */
        bool debug;

        /** Ids of data sources sending to this consumer. */
        std::set<int> ids;

        /** Last tick sent from each of the listed data sources. Key = src. */
        std::map<int, uint64_t> ticks;

        /** Number of events sent since last sync msg sent. Key = src. */
        std::map<int, uint64_t> bufsSent;

        /**
         * Starting core # to run sending thread on.
         * If < 0, don't use thread affinity.
         */
        int startingCore = -1;

        /** Number of cores for sending thread. */
        int coreCount = 1;

        /** Version of Ejfat checked against incoming data. */
        int version;

        /** Version of the Ejfat protocol checked against incoming data. */
        int protocol;

        //------------------------------------------------------------------
        // Network stuff
        //------------------------------------------------------------------

        bool connectSockets;

        // IP addresses

        /** IP address of CP. */
        std::string cpAddr;
        /** IP address of LB. */
        std::string dataAddrLB;
        /** IP addr to send sync msgs to. Should be the same as cpAddr. */
        std::string syncAddr;

        // Ports

        /** Local UDP port to recv data on. */
        uint16_t dataPortIn = 19500;
        /** LB data port. */
        uint16_t dataPortLB = 19522;
        /** Sync msg port. */
        uint16_t syncPort   = 19523;
        /** CP msg port. */
        uint16_t cpPort     = 18347;
        /** Port for receiving consumer messages. */
        uint16_t consumerPort = 18300;

        // Sockets

        /** UDP socket for receiving data from all sources. */
        int dataSocketIn;
        /** UDP socket for sending data to LB. */
        int dataSocketLB;
        /** UDP socket for sending syncs to CP. */
        int syncSocket;
        /** UDP socket for receiving msgs from consumers. */
        int consumerSocket;

        /**
         * IP address for UDP sockets to listen on.
         * If not set, sockets will bind to INADDR_ANY.
         * Currently unused.
         */
        std::string listeningAddr;

        bool useIPv6 = false;
        /** If true, use IP v6 for incoming data address, else use v4. */
        bool ipv6DataAddrIn = false;
        /** If true, use IP v6 for LB data address, else use v4. */
        bool ipv6DataAddrLB = false;
        /** If true, use IP v6 for sync msg address, else use v4. */
        bool ipv6SyncAddr   = false;
        /** If true, use IP v6 for receiving consumer msgs address, else use v4. */
        bool ipv6ConsumerAddr = false;

        /** Size in bytes of UDP data socket's recv buffers. */
        int udpRecvBufSize = 25000000;
        /** Size in bytes of UDP data socket's send buffers. */
        int udpSendBufSize = 25000000;


        /** Structure for data socket connection, IPv4. */
        struct sockaddr_in  sendAddrStruct;
        /** Structure for data socket connection, IPv6. */
        struct sockaddr_in6 sendAddrStruct6;


        /** Structure for sync connection, IPv4. */
        struct sockaddr_in  syncAddrStruct;
        /** Structure for sync connection, IPv6. */
        struct sockaddr_in6 syncAddrStruct6;


        //------------------------------------------------------------------
        // Control Plane stuff
        //------------------------------------------------------------------

        // CP's port and IP addr listed above

        /** Name of this consumer as given to CP, generated internally, */
        std::string myName;

        /** Token with which to register with the CP. */
        std::string instanceToken;

        /** Id of the LB reserved for use by this consumer. */
        std::string lbId;

        /** Map of objects used to interact with the CP to register, update, and deregister.
         * Key is the string "ipAddr:port", Value is shared pointer of client object. */
        std::map<std::string, std::shared_ptr<LbControlPlaneClient>> LbClients;

        //------------------------------------------------------------------
        // Thread stuff
        //------------------------------------------------------------------

        /** Structure to pass to each sending thread. */
        typedef struct sendThdArg_t {
            int core;
            int coreCount;
        } sendThdArg;


        /** Thread for talking to the EJFAT control plane. */
        std::thread consumerThread;

        /** Thread for handling all incoming data and sending to LB. */
        std::thread sendThread;

        /** Thread for sync msg sending to CP. */
        std::thread syncThread;


        /** Has the consumer thread been started? */
        volatile bool consumerThdStarted = false;

        /** Has the send thread been started? */
        volatile bool sendThdStarted = false;

        /** Has the sync thread been started? */
        volatile bool syncThdStarted = false;


        /** Flag used to stop threads. */
        std::atomic_bool endThreads;



    public:

        // Multiple data sources
        EjfatServer(const std::string& uri = "",
                    const std::string& fileName = "/tmp/ejfat_uri",
                    uint16_t dataPort = 19500, uint16_t consumerPort = 18300,
                    bool useIpv6 = false, bool connect = true, bool debug = false,
                    int startingCore = -1, int coreCount = 1,
                    int version = 2, int protocol = 1);


        // No copy constructor
        EjfatServer(const EjfatServer & item) = delete;

        // Destructor to shutdown threads
        ~EjfatServer();

        // No assignment operator
        EjfatServer & operator=(const EjfatServer & other) = delete;


    private:

        bool setFromURI(ejfatURI & uri);

        void consumerThreadFunc();
        void startupConsumerThread();

        void sendThreadFunc(sendThdArg *arg);
        void startupSendThread();

        void sendSyncFunc();
        void startupSyncThread();

        void createDataInputSocket();
        void createDataOutputSocket();
        void createSyncSocket();
        void createConsumerSocket();

        void createSocketsAndStartThreads();
    };



}


#endif // EJFAT_EJFATSERVER_H
