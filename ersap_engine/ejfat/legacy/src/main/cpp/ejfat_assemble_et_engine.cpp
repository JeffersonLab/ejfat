#include "ejfat_assemble_et_engine.hpp"
#include "ejfat_assemble_ersap_et.hpp"

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

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>


#ifdef __APPLE__
#include <inttypes.h>
#endif


namespace ersap {
namespace ejfat {


    EjfatAssembleEtEngine::EjfatAssembleEtEngine()
    {
        // Look for a local config file (assembler_et.yaml)
        port  = 7777;
        debug = false;

        // Parse config, get ET name and all source ids here
        parseConfigFile();

        // Create UDP socket
        sock = socket(AF_INET, SOCK_DGRAM, 0);

        // Configure settings in address struct
        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

        // Bind socket with address struct
        int err = bind(sock, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
        if (err != 0) {
            // TODO: handle error properly
            if (debug) fprintf(stderr, "bind socket error\n");
        }

        // Open ET system
        et_open_config_init(&openconfig);
        if (et_open(&id, etName.c_str(), openconfig) != ET_OK) {
            if (debug) fprintf(stderr, "et_open problems\n");
            exit(1);
        }
        et_open_config_destroy(openconfig);

        // Create FIFO object with which to use ET system
        int status = et_fifo_openProducer(id, &fid, ids, idCount);
        if (status != ET_OK) {
            if (debug) fprintf(stderr, "et_fifo_open problems\n");
            exit(1);
        }
    }

    EjfatAssembleEtEngine::~EjfatAssembleEtEngine()
    {
        et_fifo_close(fid);
        et_close(id);
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


    void EjfatAssembleEtEngine::parseConfigFile()
    {
        std::ifstream file("./assembler_et.yaml");
        if (!file) {
            std::cout << "unable to open ./assembler_et.yaml file, use default values";
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
            else if (key == "etName") {
                etName = val;
            }
            else if (key == "ids") {
                // get a vector of source ids (originally listed in comma-separated string)
                std::vector<std::string> vec = split(val, ',');

                // Tranform this into an array of ints
                for (int i=0; i < vec.size(); i++) {
                    ids[i] = atoi(vec[i].c_str());
                }
                idCount = vec.size();
            }
            else if (key == "debug") {
                if (val == "true" || val == "on") {
                    debug = true;
                }
            }
        }

    }


    void EjfatAssembleEtEngine::process()
    {
        if (debug) std::cout << "EJFAT assembling ..." << std::endl;

        int err = getBuffers(sock, fid, debug);
        if (err < 0) {
            fprintf(stderr, "Error assembling packets, err = %d\n", err);
            exit (-1);
        }
    }


    void EjfatAssembleEtEngine::process(int _sock, et_fifo_id _fid, int _debug)
    {
        if (debug) std::cout << "EJFAT assembling ..." << std::endl;

        int err = getBuffers(_sock, _fid, _debug);
        if (err < 0) {
            fprintf(stderr, "Error assembling packets, err = %d\n", err);
            exit (-1);
        }
    }

} // end namespace ejfat
} // end namespace ersap
