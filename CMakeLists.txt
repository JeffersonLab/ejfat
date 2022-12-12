# To use this, do the following the first time:
#   % mkdir build
#   % cd build
#   % cmake ..
#   % make (install)
#
# To build code that uses the ET system,
#   % cmake .. -DBUILD_ET=1
#
# To build code that uses the ZMQ,
#   % cmake .. -DBUILD_ZMQ=1
#
# To build ERSAP engine/service code, define env var ERSAP_HOME and
#   % cmake .. -DBUILD_ERSAP=1
#
# These flag may be combined
#
# After doing this once can do the following to recompile
#  % cd <top level evio dir>
#  % cmake --build build (--target install)
#
#
# The installation will default to the CODA env variable if it's defined
# This can be overridden on command line (in build dir) with:
#   % cmake -DCODA_INSTALL=<my_dir> ..
#   % make install

# This file came from evio-6.0 project

cmake_minimum_required(VERSION 3.10)
project(ejfat VERSION 0.1)


# define where we put FindZeroMQ.cmake file so zmq can be found
#set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX -dbg)

# Make executables
set(MAKE_EXEC 1)

# Test code build is optional and set it to OFF by default
option(BUILD_TEST "Build test code" OFF)


# uncomment to put in debug symbols or do on command line with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
#set(CMAKE_BUILD_TYPE Debug)

# Place libs & binaries in build/lib and bin (this is not for installation)
set(LIBRARY_OUTPUT_PATH    ${CMAKE_BINARY_DIR}/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)


# Set architecture-related string for installation consistent with scons file
set(ARCH ${CMAKE_HOST_SYSTEM_NAME}-${CMAKE_HOST_SYSTEM_PROCESSOR})
message(STATUS "ARCH = " ${ARCH})

# CMAKE_INSTALL_PREFIX defaults to /usr/local on unix

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type selected, default to Release")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release MinSizeRel RelWithDebInfo)
endif()

if (BUILD_ERSAP AND DEFINED ENV{ERSAP_HOME})
    # Provide useful default paths when $ERSAP_HOME is defined
    message(STATUS "Installing into ERSAP_HOME = " $ENV{ERSAP_HOME})
    set(CMAKE_INSTALL_PREFIX "$ENV{ERSAP_HOME}" CACHE PATH "" FORCE)
    set(CMAKE_INSTALL_INCLUDEDIR ${ERSAP_HOME}/include)
    # look in here when calling find_library, etc.
    list(APPEND CMAKE_PREFIX_PATH "$ENV{ERSAP_HOME}")
elseif (DEFINED INSTALL_DIR)
    message(STATUS "Installing into " ${INSTALL_DIR})
    set(INSTALL_DIR_DEFINED 1)
    set(CMAKE_INSTALL_PREFIX ${INSTALL_DIR}/${ARCH})
    set(CMAKE_INSTALL_INCLUDEDIR ${INSTALL_DIR}/common/include)
    message(STATUS "CMAKE_INSTALL_PREFIX reset to " ${CMAKE_INSTALL_PREFIX})
else()
    message(STATUS "Either set env var ERSAP_HOME or specify -DINSTALL_DIR=<...> on the command line or no installation possible!")
endif()

if (NOT DEFINED CMAKE_INSTALL_PLUGINDIR)
    set(CMAKE_INSTALL_PLUGINDIR plugins CACHE PATH "Ersap plugins directory (plugins)")
endif()
mark_as_advanced(CMAKE_INSTALL_PLUGINDIR)


#add_compile_options(-Wall -Wextra)
add_compile_options(-O3)


list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules)


set(EMU_EXEC_FILES
        emulation/lb_emu.cc
        emulation/lb_rcv.cc
        emulation/lb_rcv-roc.cc
        emulation/pktzr_md.cc
        )

set(ET_EXEC_FILES
        simulation/udp_rcv_et.cc
        simulation/ersap_et_consumer.cc
        ersap_engine/ejfat/legacy/src/main/cpp/ejfat_assemble_et_engine.cpp
        )

set(DIS_EXEC_FILES
        simulation/scatter.cc
        simulation/gather.cc
        simulation/testBufSupply.cc
        simulation/packetBlasteeBuf.cc
        simulation/packetBlasteeFast.cc
        simulation/packetBlasterFat.cc
        simulation/packetBlasteeFat.cc
        simulation/packetBlasteeTcpMT.cc
        )

set(UTIL_FILES
        util/ByteOrder.cpp
        util/ByteBuffer.cpp
        util/BufferSupplyItem.cpp
        util/BufferSupply.cpp
        )

set(SIM_EXEC_FILES
        simulation/udp_rcv_order.cc
        simulation/udp_send_order.cc
        simulation/packetBlastee.cc
        simulation/packetBlasteeTcp.cc
        simulation/packetBlaster.cc
        simulation/packetBlasterSmall.cc
        simulation/packetBlastee2.cc
        simulation/packetBlaster2.cc
        simulation/packetBlasterFat2.cc
        )

set(ZMQ_EXEC_FILES
        simulation/packetBlasteeZmq.cc
        simulation/bufferReceiverZmq.cc
        )

set(CLAS_EXEC_FILES
        clas_source/clasBlaster.cc
        )

set(TEST_FILES
        )

set(CPP_HEADER_FILES
        simulation/ejfat_packetize.hpp
        simulation/ejfat_assemble_ersap.hpp
        simulation/ejfat_assemble_ersap_et.hpp
        util/ByteOrder.h
        util/ByteBuffer.h
        util/BufferSupplyItem.h
        util/BufferSupply.h
        )


# Files using Disruptor code also need util files, so keep them separate from other executables,
# ie don't include DIS_EXEC_FILES here
set(EXEC_FILES ${EMU_EXEC_FILES} ${SIM_EXEC_FILES})



# Disruptor build is optional and set it to OFF by default
option(BUILD_DIS "Build Disruptor-related code" OFF)

if (BUILD_DIS)
    # Finding ET libs and includes involve looking at CODA install directory
    if (DEFINED ENV{CODA})
        message(STATUS "CODA = " $ENV{CODA})
    else()
        message(FATAL_ERROR "Set the \"CODA\" env var so that Disruptor libs and includes can be found!")
    endif()

    find_package(DISRUPTOR)
    if (DISRUPTOR_FOUND)
        message(STATUS "Disruptor include dir = "  ${DISRUPTOR_INCLUDE_DIRS})

        # Need boost libs for disruptor
        find_package(Boost REQUIRED system thread chrono)

        if (Boost_FOUND)
            message(STATUS "Boost Found: libs = " ${Boost_LIBRARIES} ", include dirs = " ${Boost_INCLUDE_DIRS})
            link_directories(${Boost_LIBRARY_DIRS})
        else()
            message(FATAL_ERROR "Boost cannot be found, cmake will exit." )
        endif()

    else()
        message(STATUS "Disruptor cannot be found, compile without it")
    endif()
else()
    message(STATUS "Compile without Disruptor")
endif()


# ET build is optional and set it to OFF by default
option(BUILD_ET "Build ET-related code" OFF)

if (BUILD_ET)
    # Finding ET libs and includes involve looking at CODA install directory
    if (DEFINED ENV{CODA})
        message(STATUS "CODA = " $ENV{CODA})
    else()
        message(FATAL_ERROR "Set the \"CODA\" env var so that ET libs and includes can be found!")
    endif()

    find_package(ET)
    if (ET_FOUND)
        set(EXEC_FILES ${EXEC_FILES} ${ET_EXEC_FILES})
    else()
        message(STATUS "Et cannot be found, compile without it")
    endif()
else()
    message(STATUS "Compile without zeromq")
endif()


# Ersap build is optional and set it to OFF by default
option(BUILD_ERSAP "Build ERSAP engine and service" OFF)

if (BUILD_ERSAP AND DEFINED ENV{ERSAP_HOME})
    find_package(Ersap REQUIRED)

    add_subdirectory(ersap_engine/ejfat/legacy/src/main/cpp)
    add_subdirectory(ersap_engine/ejfat/services/src/main/cpp)
endif()


# Zeromq build is optional and set it to OFF by default
option(BUILD_ZMQ "Build ZMQ-related code" OFF)

# if you tell cmake to have code use USE_ZMQ preprocessor macro
if (BUILD_ZMQ)
    find_package(ZeroMQ)
    if (ZeroMQ_FOUND)
        #add_compile_definitions(BUILD_ZMQ)
        set(EXEC_FILES ${EXEC_FILES} ${ZMQ_EXEC_FILES})
    else()
        message(STATUS "Zeromq cannot be found, compile without it")
    endif()
else()
    message(STATUS "Compile without zeromq")
endif()


# CLAS12 source build is optional and set it to OFF by default
option(BUILD_CLAS "Build CLAS 12 source code" OFF)

# if you tell cmake to have code use USE_CLAS preprocessor macro
if (BUILD_CLAS)
    find_package(HIPO)
    if (HIPO_FOUND)
        set(EXEC_FILES ${EXEC_FILES} ${CLAS_EXEC_FILES})
    else()
        message(STATUS "Hipo cannot be found, compile without it")
    endif()
else()
    message(STATUS "Compile without hipo")
endif()


if (BUILD_TEST)
    set(EXEC_FILES ${EXEC_FILES} ${TEST_FILES})
endif()


#include(FindPackageHandleStandardArgs)
#
#find_package(Doxygen)
#
#if( DOXYGEN_FOUND )
#    message(STATUS "Doxygen found")
#
#    # Change these values from cmake defaults in the Doxyfile
#    set(DOXYGEN_GENERATE_TREEVIEW YES)
#    set(DOXYGEN_REFERENCED_BY_RELATION YES)
#    set(DOXYGEN_EXCLUDE_PATTERNS */.* */CMakeFiles/* */_CPack_Packages/* CMakeLists.txt CMakeCache.txt)
#    #set(DOXYGEN_EXCLUDE src/libsrc++/pugixml.cpp src/libsrc++/pugixml.hpp)
#    set(DOXYGEN_FILE_PATTERNS *.c *.cpp *.h)
#    #set(DOXYGEN_WARN_LOGFILE doxyerrors.log)
#    set(DOXYGEN_SORT_MEMBERS_CTORS_1ST YES)
#    set(DOXYGEN_SHOW_GROUPED_MEMB_INC YES)
#    set(DOXYGEN_INLINE_IHERITED_MEMB YES)
#    set(DOXYGEN_JAVADOC_AUTOBRIEF YES)
#
#    # Add target of "docCC"
#    #set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/doc/doxygen/CC)
#    #doxygen_add_docs(docCC src/libsrc++)
#
#    # Change these values from cmake defaults in the Doxyfile
#    set(DOXYGEN_OPTIMIZE_OUTPUT_FOR_C YES)
#    set(DOXYGEN_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/doc/doxygen/C)
#
#    # Add target of "docC"
#    doxygen_add_docs(docC src/libsrc src/execsrc)
#
#elseif(NOT DEFINED C_ONLY)
#    message(FATAL_ERROR "Doxygen NOT found, cmake will exit." )
#endif()


# Shared evio C library
#add_library(evio SHARED ${C_LIB_FILES})
#set_target_properties(evio PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
#include_directories(evio PUBLIC src/libsrc /usr/local/include)


#if (NOT DEFINED C_ONLY)
#    # Shared evio C++ library
#    add_library(eviocc SHARED ${CPP_LIB_FILES})
#    set_target_properties(eviocc PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
#    target_link_libraries(eviocc ${LZ4_LIBRARY} ${Boost_LIBRARIES} ${DISRUPTOR_LIBRARY})
#    include_directories(eviocc PUBLIC src/libsrc++ /usr/local/include
#            ${Boost_INCLUDE_DIRS} ${LZ4_INCLUDE_DIRS} ${DISRUPTOR_INCLUDE_DIR})
#
#    if (DEFINED MAKE_EXAMPLES)
#        foreach(fileName ${TEST})
#            # Get file name with no directory or extension as executable name
#            get_filename_component(execName ${fileName} NAME_WE)
#            # Create executable from file
#            add_executable(${execName} ${fileName})
#            # Put debug extension on if applicable
#            set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
#            # Needs these libs
#            target_link_libraries(${execName} eviocc pthread ${Boost_LIBRARIES} ${LZ4_LIBRARY} expat dl z m )
#
#            # Only install if installation directory has been defined
#            if (DEFINED INSTALL_DIR_DEFINED)
#                # Install into bin/testC dir
#                install(TARGETS ${execName} RUNTIME DESTINATION bin/test)
#            endif()
#        endforeach(fileName)
#    endif()
#else()
#    message(STATUS "NOT compiling C++ evio library!!!" )
#endif()


# Test programs
if (DEFINED MAKE_EXEC)
    foreach(fileName ${EXEC_FILES})
        # Get file name with no directory or extension as executable name
        get_filename_component(execName ${fileName} NAME_WE)
        # Create executable from file
        message(STATUS "Create normal executable " ${execName})
        add_executable(${execName} ${fileName})
        # Put debug extension on if applicable
        set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
        # Location of include files
        target_include_directories(${execName} PUBLIC   ${HIPO_INCLUDE_DIRS}
                                                        ${ZeroMQ_INCLUDE_DIRS}
                                                        ${ET_INCLUDE_DIRS}
                                                        ${CMAKE_SOURCE_DIR}/simulation
                                                        ${CMAKE_SOURCE_DIR}/util)
        # Needs these libs
        target_link_libraries(${execName} pthread ${ZeroMQ_LIBRARIES} ${ET_LIBRARY} ${HIPO_LIBRARIES})

        # Only install if installation directory has been defined
        if (DEFINED INSTALL_DIR_DEFINED)
            # Install into bin dir
            install(TARGETS ${execName} RUNTIME DESTINATION bin)
        endif()
    endforeach(fileName)

    if (DISRUPTOR_FOUND)
        message(STATUS "TRY DISRUPTOR CODE:")

        # This files need util files added to executable
        foreach(fileName ${DIS_EXEC_FILES})
            # Get file name with no directory or extension as executable name
            get_filename_component(execName ${fileName} NAME_WE)
            # Create executable from file
            message(STATUS "Create disruptor-related executable " ${execName})
            add_executable(${execName} ${fileName} ${UTIL_FILES})
            # Put debug extension on if applicable
            set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
            # Location of include files
            target_include_directories(${execName} PUBLIC ${CMAKE_SOURCE_DIR}/simulation
                    ${CMAKE_SOURCE_DIR}/util
                    ${ZeroMQ_INCLUDE_DIRS}
                    ${Boost_INCLUDE_DIRS}
                    ${DISRUPTOR_INCLUDE_DIRS}
                    ${ET_INCLUDE_DIRS})
            # Needs these libs
            target_link_libraries(${execName} pthread ${Boost_LIBRARIES} ${ZeroMQ_LIBRARIES} ${DISRUPTOR_LIBRARY} ${ET_LIBRARY})

            # Only install if installation directory has been defined
            if (DEFINED INSTALL_DIR_DEFINED)
                # Install into bin dir
                install(TARGETS ${execName} RUNTIME DESTINATION bin)
            endif()
        endforeach(fileName)
    endif()

endif()


# Only install if installation directory has been defined
if (DEFINED INSTALL_DIR_DEFINED)
    install(FILES ${CPP_HEADER_FILES} DESTINATION include)
    install(FILES ${CPP_HEADER_FILES} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()


## Only install if installation directory has been defined
#if (DEFINED INSTALL_DIR_DEFINED)
#    # Installation defaulting to ${CMAKE_INSTALL_PREFIX}/lib or include
#    install(TARGETS evio LIBRARY DESTINATION lib)
#    install(FILES ${C_HEADER_FILES} DESTINATION include)
#
#    if (NOT DEFINED C_ONLY)
#        install(TARGETS eviocc LIBRARY DESTINATION lib)
#        install(FILES ${CPP_HEADER_FILES} DESTINATION include)
#        install(FILES ${CPP_HEADER_FILES} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
#    endif()
#endif()


## Uninstall target
#if(NOT TARGET uninstall)
#    configure_file(
#            "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
#            "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake" IMMEDIATE @ONLY)
#
#    add_custom_target(uninstall
#            COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
#endif()
#
