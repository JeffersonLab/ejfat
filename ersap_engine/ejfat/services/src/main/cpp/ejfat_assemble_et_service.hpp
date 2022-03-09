#ifndef SCIA_RESAMPLING_SERVICE_HPP
#define SCIA_RESAMPLING_SERVICE_HPP

#include <ersap/engine.hpp>

#include <atomic>
#include <memory>

#include "et.h"
#include "et_fifo.h"

namespace ersap {
namespace ejfat {

class EjfatAssembleEtEngine;

class EjfatAssembleEtService : public ersap::Engine
{
public:
    EjfatAssembleEtService() = default;

    EjfatAssembleEtService(const EjfatAssembleEtService&) = delete;
    EjfatAssembleEtService& operator=(const EjfatAssembleEtService&) = delete;

    EjfatAssembleEtService(EjfatAssembleEtService&&) = default;
    EjfatAssembleEtService& operator=(EjfatAssembleEtService&&) = default;

    ~EjfatAssembleEtService();

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

    static const int maxSourceIds = 1024;

    std::shared_ptr<EjfatAssembleEtEngine> engine_{};

    bool debug;

    int port;
    int sock;

    // ET system file name
    std::string etName;
    et_sys_id id;
    et_openconfig openconfig;

    // ET FIFO stuff
    et_fifo_id fid;

    // Data source ids
    int ids[maxSourceIds];
    int idCount;

};

} // end namespace jana
} // end namespace ersap

#endif
