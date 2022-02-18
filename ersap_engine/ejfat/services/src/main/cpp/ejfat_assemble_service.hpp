#ifndef SCIA_RESAMPLING_SERVICE_HPP
#define SCIA_RESAMPLING_SERVICE_HPP

#include <ersap/engine.hpp>

#include <atomic>
#include <memory>

namespace ersap {
namespace ejfat {

class EjfatAssembleEngine;

class EjfatAssembleService : public ersap::Engine
{
public:
    EjfatAssembleService() = default;

    EjfatAssembleService(const EjfatAssembleService&) = delete;
    EjfatAssembleService& operator=(const EjfatAssembleService&) = delete;

    EjfatAssembleService(EjfatAssembleService&&) = default;
    EjfatAssembleService& operator=(EjfatAssembleService&&) = default;

    ~EjfatAssembleService() override = default;

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
    std::shared_ptr<EjfatAssembleEngine> engine_{};

    std::string interface;
    int port;
};

} // end namespace jana
} // end namespace ersap

#endif
