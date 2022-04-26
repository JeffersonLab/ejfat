#ifndef ERSAP_EJFAT_PACKETIZE_ENGINE_HPP_
#define ERSAP_EJFAT_PACKETIZE_ENGINE_HPP_

#include <string>

namespace ersap {
namespace ejfat {

class EjfatPacketizeEngine
{
public:

    EjfatPacketizeEngine();

    void process(char *buffer, uint32_t bufLen, uint64_t tick);

    void process(char *buffer, uint32_t bufLen,
                 std::string & host, const std::string & interface,
                 int mtu, uint16_t port, uint64_t tick,
                 int protocol, int entropy, int version, uint16_t dataId,
                 uint32_t delay, bool debug);

    void parseConfigFile();

private:

    std::string host;
    std::string interface;

    int mtu;
    int version;
    int protocol;
    int entropy;

    uint32_t delay; // milliseconds

    uint16_t port;
    uint16_t dataId;

    bool debug;

};

} // end namespace ejfat
} // end namespace ersap

#endif
