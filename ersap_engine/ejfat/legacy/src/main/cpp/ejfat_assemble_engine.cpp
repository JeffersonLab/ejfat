#include "ejfat_assemble_engine.hpp"
#include "ejfat_assemble_ersap.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <stdio.h>
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


    EjfatAssembleEngine::EjfatAssembleEngine()
    {
        // Look for a local config file (assembler.yaml)
        port = 7777;
        interface = "127.0.0.1";

        destPort = 8888;
        destIP = "127.0.0.1";

        debug = false;
        zmq = false;

        ctx = nullptr;
        sock = nullptr;
    }

    EjfatAssembleEngine::~EjfatAssembleEngine()
    {
        if (zmq) {
            zmq_close(sock);
        }
    }


    /**
      * Create a zmq push socket to send data buffers to ERSAP back end.
      * @param host IP address of destination (PULL) zmq socket.
      * @param port TCP port of destination (PULL) zmq socket.
      * @return returns zero if successful, else it returns -1 and sets errno.
      */
    int EjfatAssembleEngine::createZmqSocket()
    {
        ctx  = zmq_ctx_new();
        sock = zmq_socket(ctx, ZMQ_PUSH);
        char dest[25];
        sprintf(dest, "tcp://%s:%hu", destIP.c_str(), destPort);
        int err = zmq_connect(sock, dest);
        return err;
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


    void EjfatAssembleEngine::parseConfigFile()
    {
        std::ifstream file("./assembler.yaml");
        if (!file) {
            std::cout << "unable to open ./assembler.yaml file, use default values";
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
                port = (int)strtol(val.c_str(), (char **)nullptr, 10);
                if ((port == 0) && (errno == EINVAL || errno == ERANGE)) {
                    port = 7777;
                }
            }
            if (key == "destPort") {
                destPort = (int)strtol(val.c_str(), (char **)nullptr, 10);
                if ((destPort == 0) && (errno == EINVAL || errno == ERANGE)) {
                    destPort = 8888;
                }
            }
            else if (key == "interface") {
                interface = val;
            }
            else if (key == "destIP") {
                destIP = val;
            }
            else if (key == "debug") {
                if (val == "true" || val == "on") {
                    debug = true;
                }
            }
            else if (key == "zmq") {
                if (val == "true" || val == "on") {
                    zmq = true;
                }
            }
        }

        if (zmq) {
            int err = createZmqSocket();
            if (err < 0) {
                perror("Problem creating zmq socket:");
                exit (-1);
            }
        }
    }



    void EjfatAssembleEngine::process(char **userBuf, size_t *userBufLen,
                                       uint16_t port, const char *listeningAddr,
                                       bool noCopy)
    {
        std::cout << "EJFAT assembling ..." << std::endl;
        //static int getBuffer(char** userBuf, int32_t *userBufLen, unsigned short port, char *listeningAddr, bool noCopy) {

        int err = getBuffer(userBuf, userBufLen, port, listeningAddr, noCopy, debug);
        if (err < 0) {
            fprintf(stderr, "Error assembling packets, err = %d\n", err);
            exit (-1);
        }

        if (zmq) {
            int err = zmq_send(sock, *userBuf, *userBufLen, 0);
            if (err < 0) {
                perror("Problem sending data on zmq socket:");
                exit (-1);
            }
        }
    }
} // end namespace ejfat
} // end namespace ersap
