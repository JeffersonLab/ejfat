//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100



/**
 * @file
 * Contains definitions and routines common to both packetizing
 * and reassembling. Mainly stuff to handle the placing of info
 * into a URI when reserving an LB and the parsing of the URI.
 */
#ifndef EJFAT_H
#define EJFAT_H



#include <iostream>
#include <fstream>
#include <string>
#include <regex>

#define btoa(x) ((x)?"true":"false")

namespace ejfat {


    /** Structure to hold info parsed from an ejfat URI (and a little extra). */
    typedef struct ejfatURI_t {

        /** Is there a valid instance token? */
        bool haveInstanceToken;
        /** Is there a valid data addr & port? */
        bool haveData;
        /** Is there a valid sync addr & port? */
        bool haveSync;
        /** Is data addr IPv6? */
        bool useIPv6Data;
        /** Is sync addr IPv6? Not used, include for future expansion. */
        bool useIPv6Sync;

        /** UDP port to send events (data) to. */
        uint16_t dataPort;
        /** UDP port for event sender to send sync messages to. */
        uint16_t syncPort;
        /** TCP port for grpc communications with CP. */
        uint16_t cpPort;

        /** String identifier of an LB instance, set by the CP on an LB reservation. */
        std::string lbId;
        /** Admin token for the CP being used. */
        std::string adminToken;
        /** Instance token set by the CP on an LB reservation. */
        std::string instanceToken;
        /** IPv4 address to send events (data) to. */
        std::string dataAddrV4;
        /** IPv6 address to send events (data) to. */
        std::string dataAddrV6;
        /** IPv4 address to send sync messages to. */
        std::string syncAddrV4;
        /** IPv6 address to send sync messages to. Not used, for future expansion. */
        std::string syncAddrV6;
        /** IPv4 address for grpc communication with CP. */
        std::string cpAddrV4;

        /** String given by user, during registration, to label an LB instance. */
        std::string lbName;

    } ejfatURI;



    /**
     * Method to clear a ejfatURI structure.
     * @param uri reference to ejfatURI struct to clear.
     */
    static void clearUri(ejfatURI &uri) {
        uri.haveInstanceToken = false;
        uri.haveData          = false;
        uri.haveSync          = false;
        uri.useIPv6Data       = false;
        uri.useIPv6Sync       = false;

        uri.dataPort = 0;
        uri.syncPort = 0;
        uri.cpPort   = 0;

        uri.lbId.clear();
        uri.adminToken.clear();
        uri.instanceToken.clear();
        uri.dataAddrV4.clear();
        uri.dataAddrV6.clear();
        uri.syncAddrV4.clear();
        uri.syncAddrV6.clear();
        uri.cpAddrV4.clear();

        uri.lbName.clear();
    }



    /**
     * Method to print out a ejfatURI structure.
     * @param out output stream.
     * @param uri reference to ejfatURI struct to print out.
     */
    static void printUri(std::ostream& out, ejfatURI &uri) {

        out << "Have data info:      " << btoa(uri.haveData) << std::endl;
        out << "Have sync info:      " << btoa(uri.haveSync) << std::endl;
        out << "Have instance Token: " << btoa(uri.haveInstanceToken) << std::endl;


        if (!uri.adminToken.empty()) {
            out << "Admin token:         " << uri.adminToken << std::endl;
        }

        if (!uri.instanceToken.empty()) {
            out << "Instance token:      " << uri.instanceToken << std::endl;
        }

        if (!uri.lbId.empty()) {
            out << "LB id:               " << uri.lbId << std::endl;
        }

        if (!uri.lbName.empty()) {
            out << "LB name:             " << uri.lbName << std::endl;
        }

        if (!uri.dataAddrV4.empty()) {
            out << "Data host:           " << uri.dataAddrV4 << std::endl;
            out << "Data port:           " << uri.dataPort << std::endl;
        }

        if (!uri.dataAddrV6.empty()) {
            out << "Data host V6:        " << uri.dataAddrV6 << std::endl;
            out << "Data port:           " << uri.dataPort << std::endl;
        }

        if (!uri.syncAddrV4.empty()) {
            out << "Sync host:           " << uri.syncAddrV4 << std::endl;
            out << "Sync port:           " << uri.syncPort << std::endl;
        }

        if (!uri.syncAddrV6.empty()) {
            out << "Sync host V6:        " << uri.syncAddrV6 << std::endl;
            out << "Sync port:           " << uri.syncPort << std::endl;
        }
    }



    /**
     * Function to determine if a string is an IPv4 address.
     * @param address string containing address to examine.
     */
    static bool isIPv4(const std::string& str) {
        std::regex ipv4_regex("^((25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\\.){3}(25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)$");
        return std::regex_match(str, ipv4_regex);
    }

    /**
     * Function to determine if a string is an IPv6 address.
     * @param address string containing address to examine.
     */
    static bool isIPv6(const std::string& str) {
        std::regex ipv6_regex("^([0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}$");
        return std::regex_match(str, ipv6_regex);
    }


    /**
     * Method to parse a URI with ejfat connection info of the format:
     * ejfat://[<token>@]<cp_host>:<cp_port>/lb/<lb_id>[?data=<data_host>:<data_port>][&sync=<sync_host>:<sync_port>].
     * Token is optional, data_host and data_port are optional, sync_host, and sync_port are optional.
     * The order of data= and sync= must be kept, data first, sync second.
     * If data= is not there, &sync must become ?sync.
     * Distinction is made between ipV6 and ipV4 addresses.
     *
     * @param uri URI to parse.
     * @param uriInfo ref to ejfatURI struct to fill with parsed values.
     * @return true if parse sucessful, else false;
     */
    static bool parseURI(const std::string uri, ejfatURI &uriInfo) {

        // Start with a blank slate
        clearUri(uriInfo);

        // URI must match this regex pattern
        std::regex pattern(R"regex(ejfat://(?:([^@]+)@)?([^:]+):(\d+)/lb/([^?]+)(?:\?(?:(?:data=(.*?):(\d+)){1}(?:&sync=(.*?):(\d+))?|(?:sync=(.*?):(\d+)){1}))?)regex");

        std::smatch match;
        if (std::regex_match(uri, match, pattern)) {
            // we're here if uri is in the proper format ...

            // optional token
            std::string token = match[1];

            if (!token.empty()) {
                uriInfo.instanceToken = token;
                uriInfo.haveInstanceToken = true;
            }
            else {
                uriInfo.haveInstanceToken = false;
            }

            uriInfo.cpAddrV4    = match[2];
            uriInfo.cpPort      = std::stoi(match[3]);
            uriInfo.lbId        = match[4];

            // in this case only syncAddr and syncPort defined
            if (!match[9].str().empty()) {
                uriInfo.haveSync = true;
                uriInfo.haveData = false;

                // decide if this is IPv4 or IPv6 or neither
                if (isIPv6(match[9])) {
                    uriInfo.syncAddrV6  = match[9];
                    uriInfo.useIPv6Sync = true;
                }
                else if (isIPv4(match[9])) {
                    uriInfo.syncAddrV4 = match[9];
                }
                else {
                    // invalid IP addr
                    uriInfo.haveSync = false;
                }

                try {
                    // look at the sync port
                    int port = std::stoi(match[10]);
                    if (port < 1024 || port > 65535) {
                        // port is out of range
                        uriInfo.haveSync = false;
                    }
                    else {
                        uriInfo.syncPort = port;
                    }

                } catch (const std::exception& e) {
                    // string is not a valid integer
                    uriInfo.haveSync = false;
                }
            }
            else {
                // if dataAddr and dataPort defined
                if (!match[5].str().empty()) {
                    uriInfo.haveData = true;

                    if (isIPv6(match[5])) {
                        uriInfo.dataAddrV6  = match[5];
                        uriInfo.useIPv6Data = true;
                    }
                    else if (isIPv4(match[5])) {
                        uriInfo.dataAddrV4 = match[5];
                    }
                    else {
                        uriInfo.haveData = false;
                    }

                    try {
                        // look at the data port
                        int port = std::stoi(match[6]);
                        if (port < 1024 || port > 65535) {
                            // port is out of range
                            uriInfo.haveData = false;
                        }
                        else {
                            uriInfo.dataPort = port;
                        }

                    } catch (const std::exception& e) {
                        // string is not a valid integer
                        uriInfo.haveData = false;
                    }

                }
                else {
                    uriInfo.haveData = false;
                }

                // if syncAddr and syncPort defined
                if (!match[7].str().empty()) {
                    uriInfo.haveSync = true;

                    // decide if this is IPv4 or IPv6 or neither
                    if (isIPv6(match[7])) {
                        uriInfo.syncAddrV6  = match[7];
                        uriInfo.useIPv6Sync = true;
                    }
                    else if (isIPv4(match[7])) {
                        uriInfo.syncAddrV4 = match[7];
                    }
                    else {
                        uriInfo.haveSync = false;
                    }

                    try {
                        // look at the sync port
                        int port = std::stoi(match[8]);
                        if (port < 1024 || port > 65535) {
                            // port is out of range
                            uriInfo.haveSync = false;
                        }
                        else {
                            uriInfo.syncPort = port;
                        }

                    } catch (const std::exception& e) {
                        // string is not a valid integer
                        uriInfo.haveSync = false;
                    }
                }
                else {
                    uriInfo.haveSync = false;
                }
            }
            return true;
        }

        return false;
    }


    /**
     * Method to get the URI produced when reserving a load balancer.
     * This can be accomplished by running lbreserve.
     *
     * @param envVar    name of environmental variable containing URI (default EJFAT_URI).
     * @param fileName  name of environmental variable containing URI (default /tmp/ejfat_uri).
     * @return string containing URI if successful, else blank string.
     */
    static std::string getURI(const std::string& envVar = "EJFAT_URI",
                              const std::string& fileName = "/tmp/ejfat_uri") {

        // First see if the EJFAT_URI environmental variable is defined, if so, parse it
        const char* uriStr = std::getenv(envVar.c_str());
        if (uriStr != nullptr) {
            return std::string(uriStr);
        }

        // If no luck with env var, look into file (should contain only one line)
        if (!fileName.empty()) {
            std::ifstream file(fileName);
            if (file.is_open()) {
                std::string uriLine;
                if (std::getline(file, uriLine)) {
                    file.close();
                    return std::string(uriLine);
                }

                file.close();
            }
        }

        return std::string("");
    }




}

#endif // EJFAT_H
