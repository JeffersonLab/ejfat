//
// Copyright 2024, Jefferson Science Associates, LLC.
// Subject to the terms in the LICENSE file found in the top-level directory.
//
// EPSCI Group
// Thomas Jefferson National Accelerator Facility
// 12000, Jefferson Ave, Newport News, VA 23606
// (757)-269-7100


#ifndef EJFAT_EJFATEXCEPTION_H
#define EJFAT_EJFATEXCEPTION_H


#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <exception>


namespace ejfat {

    /**
     * Exception class for Ejfat software package.
     * @date 03/18/2024
     * @author timmer
     */
    class EjfatException : public std::runtime_error {

    public:

        explicit EjfatException(const std::string & msg)   noexcept : std::runtime_error(msg) {}
        explicit EjfatException(const std::exception & ex) noexcept : std::runtime_error(ex.what()) {}

        EjfatException(const std::string & msg, const char *file, int line) noexcept : std::runtime_error(msg) {
            std::ostringstream o;
            o << file << ":" << line << ":" << msg;
            std::cerr << o.str() << std::endl;
        }

    };

#define throwEjfatLine(arg) throw EjfatException(arg, __FILE__, __LINE__);

}

#endif //EJFAT_EJFATEXCEPTION_H
