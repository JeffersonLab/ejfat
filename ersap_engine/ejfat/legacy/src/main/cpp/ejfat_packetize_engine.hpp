#ifndef ERSAP_EJFAT_PACKETIZE_ENGINE_HPP_
#define ERSAP_EJFAT_PACKETIZE_ENGINE_HPP_

#include <string>

namespace ersap {
namespace ejfat {

class EjfatPacketizeEngine
{
public:

    EjfatPacketizeEngine();

    // Send a single buffer
    void process(char *buffer, uint32_t bufLen, uint64_t tick);

    void process(char *buffer, uint32_t bufLen,
                 std::string & host, const std::string & interface,
                 int mtu, uint16_t port, uint64_t tick,
                 int protocol, int entropy, int version, uint16_t dataId,
                 uint32_t delay, bool debug, bool useIPv6);

    // Send an array of buffers
    void process(char **buffers, uint32_t *bufLens, int *entropys, int bufCount, uint64_t tick);

    void process(char **buffers, uint32_t *bufLens, int *entropys, int bufCount,
                 std::string & host, const std::string & interface,
                 int mtu, uint16_t port, uint64_t tick,
                 int protocol, int version,
                 uint32_t delay, uint32_t delayPrescale, bool debug, bool useIPv6);

    void parseConfigFile();

private:

    std::string host;
    std::string interface;

    int mtu;
    int version;
    int protocol;
    int entropy;

    uint32_t delay; // microseconds
    uint32_t delayPrescale;

    uint16_t port;
    uint16_t dataId;

    bool debug;
    bool useIPv6;

};

} // end namespace ejfat
} // end namespace ersap

#endif
