#include "ejfat_packetize_service.hpp"

#include <ejfat_packetize_engine.hpp>

#include <ersap/stdlib/json_utils.hpp>
#include <ersap/engine_data.hpp>

#include <iostream>
#include <sys/time.h>
#include <ctime>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>
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
    return std::make_unique<ersap::ejfat::EjfatPacketizeService>();
}


namespace ersap {
namespace ejfat {

time_t start;

ersap::EngineData EjfatPacketizeService::configure(ersap::EngineData& input)
{
    // Ersap provides a simple JSON parser to read configuration data
    // and configure the service.
    auto config = ersap::stdlib::parse_json(input);

    start = time(nullptr);

    // Defaults


    host = "127.0.0.1";
    interface = "eth0";
    mtu = 1024;
    port = 19522;
    ver = 2;
    dataId = 1;
    protocol = 1;
    entropy = 0;
    delay = 0;
    debug = false;
    useIPv6 = false;

    // Values from config file
    if (ersap::stdlib::has_key(config, "host")) {
        host = ersap::stdlib::get_string(config, "host");
    }
    if (ersap::stdlib::has_key(config, "interface")) {
        interface = ersap::stdlib::get_string(config, "interface");
    }
    if (ersap::stdlib::has_key(config, "debug")) {
        debug = ersap::stdlib::get_bool(config, "debug");
    }
    if (ersap::stdlib::has_key(config, "useIPv6")) {
        useIPv6 = ersap::stdlib::get_bool(config, "useIPv6");
    }
    if (ersap::stdlib::has_key(config, "mtu")) {
        mtu = ersap::stdlib::get_int(config, "mtu");
    }
    if (ersap::stdlib::has_key(config, "port")) {
        port = (uint16_t) ersap::stdlib::get_int(config, "port");
    }
    if (ersap::stdlib::has_key(config, "version")) {
        ver = ersap::stdlib::get_int(config, "version");
    }
    if (ersap::stdlib::has_key(config, "delay")) {
        delay = ersap::stdlib::get_int(config, "delay");
    }
    if (ersap::stdlib::has_key(config, "dataId")) {
        dataId = (uint16_t) ersap::stdlib::get_int(config, "dataId");
    }
    if (ersap::stdlib::has_key(config, "protocol")) {
        protocol = ersap::stdlib::get_int(config, "protocol");
    }
    if (ersap::stdlib::has_key(config, "entropy")) {
        entropy = ersap::stdlib::get_int(config, "entropy");
    }

    // Example for when the service has state that is configured by
    // the orchestrator. The "state" object should be a std::shared_ptr
    // accessed atomically.
    //
    // (This service is actually stateless, so detector_ could just simply be
    // initialized in the service constructor).
    std::atomic_store(&engine_, std::make_shared<EjfatPacketizeEngine>());
    return {};
}

ersap::EngineData EjfatPacketizeService::execute(ersap::EngineData& input)
{

    // Pull out needed items from data
    auto & vec = data_cast<std::vector<uint8_t>>(input);
    uint64_t tick   = ntohl(*reinterpret_cast<const uint32_t*>(&vec[0]));
    uint32_t bufLen = ntohl(*reinterpret_cast<const uint32_t*>(&vec[4]));
    char *buffer    = reinterpret_cast<char*>(&vec[8]);

    // This always loads the shared_pointer into a new shared_ptr
    std::atomic_load(&engine_)->process(buffer, bufLen, host, interface,
                                        mtu, port, tick, protocol, entropy, ver,
                                        dataId, delay, debug, useIPv6);
    return input;
}

ersap::EngineData EjfatPacketizeService::execute_group(const std::vector<ersap::EngineData>& input)
{
//    // Pull out needed items from data.
//    // Remove const
//    auto & inputVec = const_cast<std::vector<ersap::EngineData>&> (input);
//
//    uint64_t   tick;
//
//    int        bufCount = input.size();
//    int        entropys[bufCount];
//    uint32_t   bufLens[bufCount];
//    char*      buffers[bufCount];
//
//    for (int i=0; i < bufCount; i++) {
//        auto & vec = data_cast<std::vector<uint8_t>>(inputVec[i]);
//
//        tick       = ntohl(*reinterpret_cast<const uint32_t*>(&vec[0]));
//        bufLens[i] = ntohl(*reinterpret_cast<const uint32_t*>(&vec[4]));
//        buffers[i] = reinterpret_cast<char*>(&vec[8]);
//        entropys[i] = i;
//    }
//
//        // This always loads the shared_pointer into a new shared_ptr
//    std::atomic_load(&engine_)->process(buffers, bufLens, entropys, bufCount,
//                                        host, interface,
//                                        mtu, port, tick, protocol, ver,
//                                        delay, debug, useIPv6);
    return {};
}

std::vector<ersap::EngineDataType> EjfatPacketizeService::input_data_types() const
{
    return { ersap::type::JSON, ersap::type::BYTES };
}


std::vector<ersap::EngineDataType> EjfatPacketizeService::output_data_types() const
{
    return { ersap::type::BYTES };
}


std::set<std::string> EjfatPacketizeService::states() const
{
    return std::set<std::string>{};
}


std::string EjfatPacketizeService::name() const
{
    return "EjfatPacketizeService";
}


std::string EjfatPacketizeService::author() const
{
    return "Carl Timmer";
}


std::string EjfatPacketizeService::description() const
{
    return "EJFAT service to UDP packetize data and sent to FPGA-based load balancer";
}


std::string EjfatPacketizeService::version() const
{
    return "0.1";
}

}
}
