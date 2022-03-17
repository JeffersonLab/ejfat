#ifndef ERSAP_EJFAT_ASSEMBLE_ENGINE_HPP_
#define ERSAP_EJFAT_ASSEMBLE_ENGINE_HPP_

#include <string>
#include <et.h>
#include <et_fifo.h>

namespace ersap {
namespace ejfat {

class EjfatAssembleEtEngine
{
public:

    EjfatAssembleEtEngine();
    EjfatAssembleEtEngine(uint16_t port, const std::string & etName,
                          const std::string & listeningAddr,
                          int* ids, int idCount, bool debug);

    ~EjfatAssembleEtEngine();

    void process();
    void process(int sock, et_fifo_id fid, int debug);

    void parseConfigFile();

private:

    static const int maxSourceIds = 1024;

    // UDP listening socket
    int sock;
    uint16_t port;

    bool debug;

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

} // end namespace ejfat
} // end namespace ersap

#endif
