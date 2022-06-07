#include "ejfat_packetize_engine.hpp"
#include "ejfat_packetize.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <vector>
#include <iterator>
#include <string>
#include <cctype>
#include <errno.h>

#ifdef __APPLE__
#include <inttypes.h>
#endif

namespace ersap {
namespace ejfat {


    EjfatPacketizeEngine::EjfatPacketizeEngine() {
        // Default values
        port = 19522;
        mtu  = 1024;
        version = 2;
        dataId = 1;
        protocol = 1;
        entropy = 0;
        delay = 0;
        delayPrescale = 1;
        host = "127.0.0.1";
        interface = "eth0";
        debug = false;
        useIPv6 = false;
    }


    static std::vector<std::string> split(const std::string &s, char delim)
    {
        std::stringstream ss(s);
        std::string item;
        std::vector<std::string> elems;
        while (std::getline(ss, item, delim)) {
            elems.push_back(item);
        }
        return elems;
    }

    static std::string trim(const std::string &s)
    {
        auto start = s.begin();
        while (start != s.end() && std::isspace(*start)) {
            start++;
        }

        auto end = s.end();
        do {
            end--;
        } while (std::distance(start, end) > 0 && std::isspace(*end));

        return std::string(start, end + 1);
    }


    void EjfatPacketizeEngine::parseConfigFile()
    {
        std::ifstream file("./packetizer.yaml");
        if (!file) {
            std::cout << "unable to open ./packetizer.yaml file, use default values";
            return;
        }

        std::string line;
        while (getline(file, line)) {
            // Split at ":"
            std::vector<std::string> strs = split(line, ':');
            // Strip off white space
            const std::string key = trim(strs[0]);
            const std::string val = trim(strs[1]);

            if (key == "port") {
                port = (uint16_t)strtol(val.c_str(), (char **)nullptr, 10);
                if ((port == 0) && (errno == EINVAL || errno == ERANGE)) {
                    port = 19522;
                }
            }
            else if (key == "mtu") {
                mtu = (int)strtol(val.c_str(), (char **)nullptr, 10);
                if ((mtu == 0) && (errno == EINVAL || errno == ERANGE)) {
                    mtu = 1024;
                }
            }
            else if (key == "version") {
                version = (int)strtol(val.c_str(), (char **)nullptr, 10);
                if ((version == 0) && (errno == EINVAL || errno == ERANGE)) {
                    version = 1;
                }
            }
            else if (key == "delay") {
                delay = (uint32_t)strtol(val.c_str(), (char **)nullptr, 10);
                if ((delay == 0) && (errno == EINVAL || errno == ERANGE)) {
                    delay = 0;
                }
            }
            else if (key == "delayPrescale") {
                delayPrescale = (uint32_t)strtol(val.c_str(), (char **)nullptr, 10);
                if (delayPrescale == 0) {
                    delayPrescale = 1;
                }
            }
            else if (key == "protocol") {
                protocol = (int)strtol(val.c_str(), (char **)nullptr, 10);
                if ((protocol == 0) && (errno == EINVAL || errno == ERANGE)) {
                    protocol = 1;
                }
            }
            else if (key == "entropy") {
                entropy = (int)strtol(val.c_str(), (char **)nullptr, 10);
                if (errno == EINVAL || errno == ERANGE) {
                    entropy = 0;
                }
            }
            else if (key == "dataId") {
                dataId = (uint16_t)strtol(val.c_str(), (char **)nullptr, 10);
                if ((dataId == 0) && (errno == EINVAL || errno == ERANGE)) {
                    dataId = 1;
                }
            }
            else if (key == "debug") {
                if (val == "true" || val == "on") {
                    debug = true;
                }
            }
            else if (key == "useIPv6") {
                if (val == "true" || val == "on") {
                    useIPv6 = true;
                }
            }
            else if (key == "host") {
                host = val;
            }
            else if (key == "interface") {
                interface = val;
            }
        }
    }


    void EjfatPacketizeEngine::process(char *buffer, uint32_t bufLen, uint64_t tick)
    {
        //        std::cout << "EJFAT processing..." << std::endl;

        int err = ::ejfat::sendBuffer(buffer, bufLen, host, interface,
                                      mtu, port, tick, protocol, entropy,
                                      version, dataId, delay, delayPrescale, debug, true, useIPv6);
        if (err < 0) {
            fprintf(stderr, "\nError in ejfat_packetize_engine.process(): %s\n", strerror(errno));
            exit (-1);
        }
    }


    void EjfatPacketizeEngine::process(char *buffer, uint32_t bufLen,
                                       std::string & host, const std::string & interface,
                                       int mtu, uint16_t port, uint64_t tick,
                                       int protocol, int entropy, int version, uint16_t dataId,
                                       uint32_t delay, bool debug, bool useIPv6)
    {
//        std::cout << "EJFAT processing..." << std::endl;

        int err = ::ejfat::sendBuffer(buffer, bufLen, host, interface,
                                      mtu, port, tick, protocol, entropy,
                                      version, dataId, delay, delayPrescale, debug, true, useIPv6);
        if (err < 0) {
            fprintf(stderr, "\nError in ejfat_packetize_engine.process(): %s\n", strerror(errno));
            exit (-1);
        }
    }



    /**
     * Packetize buffers and send to given destinations.
     *
     * @param buffers  array of buffers
     * @param bufLens  array of buffer lengths in bytes
     * @param entropys array of entropy values. Since dataId must match entropy, just use entropy for data Id.
     * @param bufCount number of array elements
     * @param tick
     */
    void EjfatPacketizeEngine::process(char **buffers, uint32_t *bufLens, int *entropys, int bufCount, uint64_t tick)
    {
        //        std::cout << "EJFAT processing..." << std::endl;
        for (int i=0; i < bufCount; i++) {
            int err = ::ejfat::sendBuffer(buffers[i], bufLens[i], host, interface,
                                          mtu, port, tick, protocol, entropys[i],
                                          version, entropys[i], delay, delayPrescale, debug, true, useIPv6);
            if (err < 0) {
                fprintf(stderr, "\nError in ejfat_packetize_engine.process(): %s\n", strerror(errno));
                exit(-1);
            }
        }
    }


    /**
     * Packetize buffers and send to given destinations.
     *
     * @param buffers  array of buffers
     * @param bufLens  array of buffer lengths in bytes
     * @param entropys array of entropy values. Since dataId must match entropy, just use entropy for data Id.
     * @param bufCount number of array elements
     * @param host
     * @param interface
     * @param mtu
     * @param port
     * @param tick
     * @param protocol
     * @param version this must = 2.
     * @param delay microseconds between each packet
     * @param delayPrescale delay every Nth packet
     * @param debug
     * @param useIPv6
     */
    void EjfatPacketizeEngine::process(char **buffers, uint32_t *bufLens, int *entropys, int bufCount,
                                       std::string & host, const std::string & interface,
                                       int mtu, uint16_t port, uint64_t tick,
                                       int protocol, int version,
                                       uint32_t delay, uint32_t delayPrescale, bool debug, bool useIPv6)
    {
        //        std::cout << "EJFAT processing..." << std::endl;
        // for now set by hand
        version = 2;

        for (int i=0; i < bufCount; i++) {
            int err = ::ejfat::sendBuffer(buffers[i], bufLens[i], host, interface,
                                          mtu, port, tick, protocol, entropys[i],
                                          version, entropys[i], delay, delayPrescale, debug, true, useIPv6);
            if (err < 0) {
                fprintf(stderr, "\nError in ejfat_packetize_engine.process(): %s\n", strerror(errno));
                exit(-1);
            }
        }
    }


} // end namespace ejfat
} // end namespace ersap
