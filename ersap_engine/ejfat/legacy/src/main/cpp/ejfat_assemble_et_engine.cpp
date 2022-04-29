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
#include <string.h>
#include <cctype>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <limits>
#include <unistd.h>
#include <getopt.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>


#ifdef __APPLE__
#include <inttypes.h>
#endif


namespace ersap {
namespace ejfat {


    EjfatAssembleEtEngine::EjfatAssembleEtEngine(uint16_t port_, const std::string & etName_,
                                                 const std::string & listeningAddr,
                                                 int* ids_, int idCount_, bool debug_, bool useIPv6_) :
                                                 port(port_), etName(etName_),
                                                 idCount(idCount_), debug(debug_), useIPv6(useIPv6_)
    {
        // Save inputs
        if (idCount_ > 1024) {
            throw std::runtime_error("idCount must not be > 1024");
        }

        for (int i=0; i < idCount; i++) {
            ids[i] = ids_[i];
        }

        if (useIPv6) {
            struct sockaddr_in6 serverAddr6{};

            // Create IPv6 UDP socket
            if ((sock = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                exit(1);
            }

            // Try to increase recv buf size to 25 MB
            socklen_t size = sizeof(int);
            int recvBufBytes = 25000000;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0; // clear it
            getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

            // Configure settings in address struct
            memset(&serverAddr6, 0, sizeof(serverAddr6));
            serverAddr6.sin6_family = AF_INET6;
            serverAddr6.sin6_port = htons(port);
            if (!listeningAddr.empty()) {
                inet_pton(AF_INET6, listeningAddr.c_str(), &serverAddr6.sin6_addr);
            }
            else {
                serverAddr6.sin6_addr = in6addr_any;
            }

            // Bind socket with address struct
            int err = bind(sock, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                exit(1);
            }

        } else {
            struct sockaddr_in serverAddr{};

            // Create UDP socket
            if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                exit(1);
            }

            // Try to increase recv buf size to 25 MB
            socklen_t size = sizeof(int);
            int recvBufBytes = 25000000;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0; // clear it
            getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

            // Configure settings in address struct
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(port);
if (debug) fprintf(stderr, "listening on port %hu\n", port);
            if (!listeningAddr.empty()) {
                serverAddr.sin_addr.s_addr = inet_addr(listeningAddr.c_str());
if (debug) fprintf(stderr, "listening on address %s\n", listeningAddr.c_str());
            }
            else {
                serverAddr.sin_addr.s_addr = INADDR_ANY;
if (debug) fprintf(stderr, "listening on address INADDR_ANY\n");
            }
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            // Bind socket with address struct
            int err = bind(sock, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                exit(1);
            }
        }


        // Open ET system
        int status;
        et_open_config_init(&openconfig);
        if ((status = et_open(&id, etName.c_str(), openconfig)) != ET_OK) {
            if (debug) et_perror(status);
            exit(1);
        }
        et_open_config_destroy(openconfig);

        // Create FIFO object with which to use ET system
        status = et_fifo_openProducer(id, &fid, ids, idCount);
        if (status != ET_OK) {
            if (debug) et_perror(status);
            exit(1);
        }
    }

    EjfatAssembleEtEngine::EjfatAssembleEtEngine()
    {
        // Look for a local config file (assembler_et.yaml)
        port  = 17750;
        debug = false;
        useIPv6 = false;

        // Parse config, get ET name and all source ids here
        parseConfigFile();

        if (useIPv6) {
            struct sockaddr_in6 serverAddr6{};

            // Create IPv6 UDP socket
            if ((sock = socket(AF_INET6, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv6 client socket");
                exit(1);
            }

            // Try to increase recv buf size to 25 MB
            socklen_t size = sizeof(int);
            int recvBufBytes = 25000000;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0; // clear it
            getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

            // Configure settings in address struct
            memset(&serverAddr6, 0, sizeof(serverAddr6));
            serverAddr6.sin6_family = AF_INET6;
            serverAddr6.sin6_port = htons(port);
            serverAddr6.sin6_addr = in6addr_any;

            // Bind socket with address struct
            int err = bind(sock, (struct sockaddr *) &serverAddr6, sizeof(serverAddr6));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                exit(1);
            }

        } else {
            struct sockaddr_in serverAddr{};

            // Create UDP socket
            if ((sock = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
                perror("creating IPv4 client socket");
                exit(1);
            }

            // Try to increase recv buf size to 25 MB
            socklen_t size = sizeof(int);
            int recvBufBytes = 25000000;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, sizeof(recvBufBytes));
            recvBufBytes = 0; // clear it
            getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recvBufBytes, &size);
            if (debug) fprintf(stderr, "UDP socket recv buffer = %d bytes\n", recvBufBytes);

            // Configure settings in address struct
            memset(&serverAddr, 0, sizeof(serverAddr));
            serverAddr.sin_family = AF_INET;
            serverAddr.sin_port = htons(port);
            serverAddr.sin_addr.s_addr = INADDR_ANY;
            memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

            // Bind socket with address struct
            int err = bind(sock, (struct sockaddr *) &serverAddr, sizeof(serverAddr));
            if (err != 0) {
                if (debug) fprintf(stderr, "bind socket error\n");
                exit(1);
            }
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
            else if (key == "useIPv6") {
                if (val == "true" || val == "on") {
                    useIPv6 = true;
                }
            }
        }

    }


    void EjfatAssembleEtEngine::process()
    {
        if (debug) std::cout << "EJFAT assembling ..." << std::endl;

        int err = ::ejfat::getBuffers(sock, fid, debug);
        if (err < 0) {
            fprintf(stderr, "Error assembling packets, err = %d\n", err);
            exit (-1);
        }
    }


    void EjfatAssembleEtEngine::process(int _sock, et_fifo_id _fid, int _debug)
    {
        if (debug) std::cout << "EJFAT assembling ..." << std::endl;

        int err = ::ejfat::getBuffers(_sock, _fid, _debug);
        if (err < 0) {
            fprintf(stderr, "Error assembling packets, err = %d\n", err);
            exit (-1);
        }
    }



    /**
     * Print out help.
     * @param programName name to use for this program.
     */
    static void printHelp(char *programName) {
        fprintf(stderr,
                "\nusage: %s\n%s\n%s\n%s\n%s\n%s\n\n",
                programName,
                "        -f <ET file>",
                "        [-h] [-v] [-ip6 (use IP v6)]",
                "        [-a <listening IP address (defaults to INADDR_ANY)>]",
                "        [-p <listening UDP port (17750 default)>]",
                "        [-ids <comma-separated list of data source ids, no whitespace>");

        fprintf(stderr, "        This is an EJFAT UDP packet receiver which separates input from\n");
        fprintf(stderr, "        multiple known data sources. It places data into an ET system which\n");
        fprintf(stderr, "        is addressed thru an API which treats it as a FIFO. Data buffers,\n");
        fprintf(stderr, "        one for each source, all of the same tick,a are part of 1 fifo entry.\n");
        fprintf(stderr, "        Out-of-order packetsare accounted for.\n");
    }


    /**
     * Parse all command line options.
     *
     * @param argc        arg count from main().
     * @param argv        arg list from main().
     * @param bufSize     filled with buffer size.
     * @param port        filled with UDP port to listen on.
     * @param debug       filled with debug flag.
     * @param useIPv6     filled with use IP version 6 flag.
     * @param fileName    filled with output file name.
     * @param listenAddr  filled with IP address to listen on.
     * @param fast        filled with true if reading with recvfrom and minimizing data copy.
     * @param recvmsg     filled with true if reading with recvmsg.
     */
    static void parseArgs(int argc, char **argv, uint16_t* port, bool *debug, bool *useIPv6,
                          char *etName, char *listenAddr, int *ids, int *idCount) {

        int c, i_tmp;
        bool help = false;
        bool etDefined = false;

        /* 4 multiple character command-line options */
        static struct option long_options[] =
                {{"ids",   1, NULL, 1},
                 {"ip6",   0, NULL, 2},
                 {0,       0, 0,    0}
                };


        while ((c = getopt_long_only(argc, argv, "vhp:a:f:", long_options, 0)) != EOF) {

            if (c == -1)
                break;

            switch (c) {

                case 'f':
                    if (strlen(optarg) >= 255) {
                        fprintf(stderr, "ET file name is too long\n");
                        exit(-1);
                    }
                    strcpy(etName, optarg);
                    etDefined = true;
                    break;

                case 1:
                {   // Parse comma-separated list of data source ids
                    // Returns first token

                    int id_count = 0;

                    char *token = strtok(optarg, ",");
                    i_tmp = atoi(token);
                    if (i_tmp < 0) {
                        printf("Invalid argument to -ids, each id must be >= 0\n");
                        exit(-1);
                    }
                    ids[0] = i_tmp;
                    id_count = 1;

                    // Keep printing tokens while one of the
                    // delimiters present in str[].
                    while (token != NULL) {
                        token = strtok(NULL, ",");
                        if (token == NULL) break;
                        i_tmp = atoi(token);
                        if (i_tmp < 0) {
                            printf("Invalid argument to -ids, each id must be >= 0\n");
                            exit(-1);
                        }

                        if (id_count == 32) {
                            printf("Invalid argument to -ids, too many ids, max of 32\n");
                            exit(-1);
                        }
                        ids[id_count++] = i_tmp;
                    }

                    *idCount = id_count;
                }

                break;

                case 'p':
                    // PORT
                    i_tmp = (int) strtol(optarg, nullptr, 0);
                    if (i_tmp > 1023 && i_tmp < 65535) {
                        *port = i_tmp;
                    }
                    else {
                        fprintf(stderr, "Invalid argument to -p, 1023 < port < 65536\n");
                        exit(-1);
                    }
                    break;

                case 'a':
                    // LISTENING IP ADDRESS
                    if (strlen(optarg) > 15 || strlen(optarg) < 7) {
                        fprintf(stderr, "dot-decimal listening IP address is bad\n");
                        exit(-1);
                    }
                    strcpy(listenAddr, optarg);
                    break;

                case 'v':
                    // VERBOSE
                    *debug = true;
                    break;

                case 2:
                    // IP version 6 flag
                    *useIPv6 = true;
                    break;

                case 'h':
                    help = true;
                    break;

                default:
                    printHelp(argv[0]);
                    exit(2);
            }

        }

        if (help || !etDefined) {
            printHelp(argv[0]);
            exit(2);
        }
    }



} // end namespace ejfat
} // end namespace ersap


int main(int argc, char **argv) {

    // Set this to max expected data size
    uint16_t port = 17750;
    bool debug = false;
    bool useIPv6 = false;

    int ids[32];
    // Set default id
    int idCount = 1;
    ids[0] = 1;

    char etName[255], listeningAddr[16];
    memset(etName, 0, 255);
    memset(listeningAddr, 0, 16);

    ersap::ejfat::parseArgs(argc, argv, &port, &debug, &useIPv6, etName, listeningAddr, ids, &idCount);

    ersap::ejfat::EjfatAssembleEtEngine engine(port, etName, listeningAddr, ids, idCount, debug, useIPv6);
    engine.process();

    return 0;
}
