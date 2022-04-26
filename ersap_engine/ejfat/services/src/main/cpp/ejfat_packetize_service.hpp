#ifndef SCIA_RESAMPLING_SERVICE_HPP
#define SCIA_RESAMPLING_SERVICE_HPP

#include <ersap/engine.hpp>

#include <atomic>
#include <memory>

namespace ersap {
namespace ejfat {

class EjfatPacketizeEngine;


class EjfatPacketizeService : public ersap::Engine
{
public:
    EjfatPacketizeService() = default;

    EjfatPacketizeService(const EjfatPacketizeService&) = delete;
    EjfatPacketizeService& operator=(const EjfatPacketizeService&) = delete;

    EjfatPacketizeService(EjfatPacketizeService&&) = default;
    EjfatPacketizeService& operator=(EjfatPacketizeService&&) = default;

    ~EjfatPacketizeService() override = default;

public:
    ersap::EngineData configure(ersap::EngineData&) override;

    ersap::EngineData execute(ersap::EngineData&) override;

    ersap::EngineData execute_group(const std::vector<ersap::EngineData>&) override;

public:
    std::vector<ersap::EngineDataType> input_data_types() const override;

    std::vector<ersap::EngineDataType> output_data_types() const override;

    std::set<std::string> states() const override;

public:
    std::string name() const override;

    std::string author() const override;

    std::string description() const override;

    std::string version() const override;

private:
    std::shared_ptr<EjfatPacketizeEngine> engine_{};

    std::string host;
    std::string interface;

    int mtu;
    int ver;
    int protocol;
    int entropy;

    uint32_t delay; // millisec

    uint16_t port;
    uint16_t dataId;

    bool debug;
};

} // end namespace jana
} // end namespace ersap

#endif
