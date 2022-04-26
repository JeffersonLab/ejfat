#ifndef ERSAP_EJFAT_ASSEMBLE_ENGINE_HPP_
#define ERSAP_EJFAT_ASSEMBLE_ENGINE_HPP_

#include <string>
#include <zmq.h>

namespace ersap {
namespace ejfat {

class EjfatAssembleEngine
{
public:

     EjfatAssembleEngine();
    ~EjfatAssembleEngine();

    void process(char **userBuf, size_t *userBufLen,
                 uint16_t port, const char *listeningAddr,
                 bool noCopy);

    void parseConfigFile();

    int createZmqSocket();

private:

    std::string interface;
    uint16_t port;
    bool debug;
    bool zmq;  // if true, send data over zmq
    bool useIPv6;

    // Zmq context, socket, dest addr & port
    void *ctx;
    void *sock;
    std::string destIP;
    uint16_t destPort;
};

} // end namespace ejfat
} // end namespace ersap

#endif
