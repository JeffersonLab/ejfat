#include "ejfat_assemble_et_service.hpp"
#include "ejfat_assemble_ersap_et.hpp"

#include <ejfat_assemble_et_engine.hpp>

#include <ersap/stdlib/json_utils.hpp>
#include <ersap/engine_data.hpp>

#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <sys/time.h>
#include <ctime>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>



#if __APPLE__
    #define bswap_16(value) ((((value) & 0xff) << 8) | ((value) >> 8))

    #define bswap_32(value) \
    (((uint32_t)bswap_16((uint16_t)((value) & 0xffff)) << 16) | \
    (uint32_t)bswap_16((uint16_t)((value) >> 16)))

    #define bswap_64(value) \
    (((uint64_t)bswap_32((uint32_t)((value) & 0xffffffff)) << 32) | \
    (uint64_t)bswap_32((uint32_t)((value) >> 32)))
#else
    #include <byteswap.h>
#endif

#ifdef __APPLE__
#include <cctype>
#endif


extern "C"
std::unique_ptr<ersap::Engine> create_engine()
{
    return std::make_unique<ersap::ejfat::EjfatAssembleEtService>();
}


namespace ersap {
namespace ejfat {

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


    EjfatAssembleEtService::~EjfatAssembleEtService()
    {
        et_fifo_close(fid);
        et_close(id);
    };


    ersap::EngineData EjfatAssembleEtService::configure(ersap::EngineData& input)
    {

        // Ersap provides a simple JSON parser to read configuration data
        // and configure the service.
        auto config = ersap::stdlib::parse_json(input);


        // Defaults
        debug = false;
        port  = 7777;

        if (ersap::stdlib::has_key(config, "debug")) {
            debug = ersap::stdlib::get_bool(config, "debug");
        }

        // throws exception if doesn't exist
        etName = ersap::stdlib::get_string(config, "etName");

        if (ersap::stdlib::has_key(config, "port")) {
            port = ersap::stdlib::get_int(config, "port");
        }

        // Source ids
        std::string idList = ersap::stdlib::get_string(config, "ids");
        if (idList.empty()) {
            std::cerr << "Must define list of ids in config file!" << std::endl;
            exit(-1);
        }

        // get a vector of source ids (originally listed in comma-separated string)
        std::vector<std::string> vec = split(idList, ',');

        // Tranform this into an array of ints
        for (int i=0; i < vec.size(); i++) {
            ids[i] = atoi(vec[i].c_str());
        }
        idCount = vec.size();


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
            std::cerr << "bind socket error" << std::endl;
            exit(-1);
        }

        // Open ET system
        et_open_config_init(&openconfig);
        if (et_open(&id, etName.c_str(), openconfig) != ET_OK) {
            std::cerr << "et_open problems" << std::endl;
            exit(1);
        }
        et_open_config_destroy(openconfig);

        // Create FIFO object with which to use ET system
        int status = et_fifo_openProducer(id, &fid, ids, idCount);
        if (status != ET_OK) {
            std::cerr << "et_fifo_open problems" << std::endl;
            exit(1);
        }

        // Example for when the service has state that is configured by
        // the orchestrator. The "state" object should be a std::shared_ptr
        // accessed atomically.
        //
        // (This service is actually stateless, so detector_ could just simply be
        // initialized in the service constructor).
        std::atomic_store(&engine_, std::make_shared<EjfatAssembleEtEngine>());
        return {};
    }


    ersap::EngineData EjfatAssembleEtService::execute(ersap::EngineData& input)
    {
        // This generates data in an ET system, so no meaningful input/output here

        // This always loads the shared_pointer into a new shared_ptr
        std::atomic_load(&engine_)->process(sock, fid, debug);

        return input;
    }

    ersap::EngineData EjfatAssembleEtService::execute_group(const std::vector<ersap::EngineData>&)
    {
        return {};
    }

    std::vector<ersap::EngineDataType> EjfatAssembleEtService::input_data_types() const
    {
        return { ersap::type::JSON, ersap::type::BYTES };
    }


    std::vector<ersap::EngineDataType> EjfatAssembleEtService::output_data_types() const
    {
        return { ersap::type::JSON, ersap::type::BYTES };
    }


    std::set<std::string> EjfatAssembleEtService::states() const
    {
        return std::set<std::string>{};
    }


    std::string EjfatAssembleEtService::name() const
    {
        return "EjfatAssembleService";
    }


    std::string EjfatAssembleEtService::author() const
    {
        return "Carl Timmer";
    }


    std::string EjfatAssembleEtService::description() const
    {
        return "EJFAT service to UDP assemble data and sent to FPGA-based load balancer";
    }


    std::string EjfatAssembleEtService::version() const
    {
        return "0.1";
    }

}
}
