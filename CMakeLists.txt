# To use this, do the following the first time:
#   % mkdir build
#   % cd build
#   % cmake ..
#   % make (install)
#
#
# To build ERSAP engine/service code
# define where we put FindZeroMQ.cmake file so zmq can be found
#   % cmake .. -DBUILD_ERSAP=1 -DBUILD_ET=1
#
# To build evenything in simulation and util directories
#   % cmake .. -DBUILD_DIS=1 -DBUILD_ET=1 -DBUILD_GRPC=1
#
# These and other flags may be combined to do a complete build
#  % cmake .. -DBUILD_ET=1 -DBUILD_ERSAP=1 -DBUILD_DIS=1 -DBUILD_GRPC=1 -DBUILD_CLAS=1
#
# After doing this once can do the following to recompile
#  % cd <top level dir>
#  % cmake --build build (--target install)
#
#
# The installation will default to the cmake flag INSTALL_DIR.
# If that's not defined, it will use the env var EJFAT_ERSAP_INSTALL_DIR:
#   % cmake -DINSTALL_dir=<my_dir> ..

# This file came from ejfat project

cmake_minimum_required(VERSION 3.10)
project(ejfat VERSION 0.1)


# define where we put Find<*>.cmake files so various packages can be found
# set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DEBUG_POSTFIX -dbg)

# -------- Prometheus staff

include(FetchContent)

FetchContent_Declare(
  crow
  GIT_REPOSITORY https://github.com/ipkn/crow.git
  GIT_TAG        master # or a specific commit/branch/tag
)

FetchContent_GetProperties(crow)
if(NOT crow_POPULATED)
  FetchContent_Populate(crow)
  set(CROW_INCLUDE_DIR ${crow_SOURCE_DIR}/include)
  include_directories(${CROW_INCLUDE_DIR})
endif()

# Assuming prometheus-cpp is available and find_package can locate it
find_package(prometheus-cpp REQUIRED)

#add_executable(prometheus_exporter simulation/packetBlasteeEtMTPRT.cc)
add_executable(prometheus_exporter simulation/packetBlasteeEtMTPRT.cc ${UTIL_FILES} ${GRPC_SRC_FILES})

#target_link_libraries(prometheus_exporter PRIVATE prometheus-cpp::core prometheus-cpp::pull)
target_link_libraries(prometheus_exporter PRIVATE prometheus-cpp::core prometheus-cpp::pull pthread ${Boost_LIBRARIES} ${DISRUPTOR_LIBRARY} ${GRPC_LIBRARIES} ${ET_LIBRARY})

target_include_directories(prometheus_exporter PUBLIC ${CMAKE_SOURCE_DIR}/simulation
        ${CMAKE_SOURCE_DIR}/util
        ${GRPC_INCLUDE_DIRS}
        ${ET_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}
        ${DISRUPTOR_INCLUDE_DIRS})

# -------- Prometheus staff

# Make executables
set(MAKE_EXEC 1)

# Test code build is optional and set it to OFF by default
option(BUILD_TEST "Build test code" OFF)


# uncomment to put in debug symbols or do on command line with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
#set(CMAKE_BUILD_TYPE Debug)

#add_compile_options(-Wall)
add_compile_options(-O3)


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

if (NOT DEFINED INSTALL_DIR)
    if (NOT DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR})
        message(FATAL_ERROR "Define EJFAT_ERSAP_INSTALL_DIR or specify -DINSTALL_DIR=<...> for installation")
    else()
        set(INSTALL_DIR $ENV{EJFAT_ERSAP_INSTALL_DIR})
    endif()
endif()

message(STATUS "Installing into " ${INSTALL_DIR})
set(INSTALL_DIR_DEFINED 1)
set(CMAKE_INSTALL_PREFIX ${INSTALL_DIR})
set(CMAKE_INSTALL_INCLUDEDIR ${INSTALL_DIR}/include)
message(STATUS "CMAKE_INSTALL_PREFIX set to " ${CMAKE_INSTALL_PREFIX})

if (NOT DEFINED CMAKE_INSTALL_PLUGINDIR)
    set(CMAKE_INSTALL_PLUGINDIR plugins CACHE PATH "Ersap plugins directory (plugins)")
endif()
mark_as_advanced(CMAKE_INSTALL_PLUGINDIR)


list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules)


# Michael's old files used with static LB, now obsolete, not compiled by default
set(EMU_EXEC_FILES
        emulation/lb_emu.cc
        emulation/lb_rcv.cc
        emulation/lb_rcv-roc.cc
        emulation/pktzr_md.cc
        emulation/lb_send.cc
        )

# Carl's old files used with static LB, now obsolete, not compiled by default
set(STATIC_EXEC_FILES
        staticLB/udp_rcv_order.cc
        staticLB/udp_send_order.cc
        staticLB/packetAnalyzer.cc
        staticLB/packetBlastee.cc
        staticLB/packetBlasterSmall.cc
        staticLB/packetBlasterOld.cc
        )

# Executables with no dependencies
set(NODEP_EXEC_FILES
        simulation/packetBlaster.cc
        )

set(GRPC_EXEC_FILES
        simulation/packetBlasteeEtMT.cc
        simulation/packetBlasteeFastMP.cc
        simulation/packetBlasteeFullNewMP.cc
        )

set(GRPC_SRC_FILES
        simulation/EtFifoEntryItem.cpp
        )

# ET system dependency only
set(ET_EXEC_FILES
        simulation/ersap_et_consumer.cc
        )

# Utility files all using Disruptor, fast ring-buffer based object supplies
set(UTIL_FILES
        util/ByteOrder.cpp
        util/ByteBuffer.cpp
        util/BufferSupplyItem.cpp
        util/BufferSupply.cpp
        util/BufferItem.cpp
        util/PacketsItem.cpp
        util/PacketsItemN.cpp
        util/SupplyItem.cpp
        util/PacketStoreItem.cpp
        util/PacketRefItem.cpp
        )


set(CLAS_EXEC_FILES
        clas_source/clasBlaster.cc
        )

# List of all test files
set(TEST_FILES
        )

# List of all header files
set(CPP_HEADER_FILES
        simulation/ejfat.hpp
        simulation/ejfat_packetize.hpp
        simulation/ejfat_assemble_ersap.hpp
        simulation/EtFifoEntryItem.h
        util/ByteOrder.h
        util/ByteBuffer.h
        util/BufferSupplyItem.h
        util/BufferSupply.h
        util/SupplierN.h
        util/Supplier.h
        util/SupplyItem.h
        util/BufferItem.h
        util/PacketsItem.h
        util/PacketsItemN.h
        util/PacketStoreItem.h
        util/PacketRefItem.h
        util/ejfatUtil.h
        )


# List the files with no dependencies, made by default
set(EXEC_FILES ${NODEP_EXEC_FILES})


# OLD code build is optional, set it to OFF by default
option(BUILD_OLD "Build old static-LB code" OFF)

if (BUILD_OLD)
    list(APPEND EXEC_FILES ${EMU_EXEC_FILES} ${STATIC_EXEC_FILES})
endif()



# Simple API build is optional and set it to OFF by default.
option(BUILD_SIMPLE "Build simple api library and executables" OFF)

if (BUILD_SIMPLE)
    # Need boost libs for disruptor
    find_package(Boost REQUIRED system thread chrono)

    if (Boost_FOUND)
        message(STATUS "Boost Found: libs = " ${Boost_LIBRARIES} ", include dirs = " ${Boost_INCLUDE_DIRS})
        link_directories(${Boost_LIBRARY_DIRS})
    else()
        message(FATAL_ERROR "Boost cannot be found, cmake will exit." )
    endif()

    # Find where GRPC and ERSAP backend libs and includes are installed for ejfat use
    if (NOT(DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR} OR DEFINED ENV{GRPC_INSTALL_DIR}))
        message(FATAL_ERROR "Set EJFAT_ERSAP_INSTALL_DIR or GRPC_INSTALL_DIR env var so GRPC and ERSAP backend libs and includes can be found")
    endif()

    find_package(GRPC)
    if (NOT GRPC_FOUND)
        message(STATUS "GRPC cannot be found, compile without it")
    else()
        message(STATUS "GRPC_LIBRARIES = " ${GRPC_LIBRARIES})
    endif()


    add_subdirectory(simpleAPI)
endif()



# Disruptor build is optional and set it to OFF by default
option(BUILD_DIS "Build Disruptor-related code" OFF)

if (BUILD_DIS)
    # Find where Disruptor libs and includes are installed for ejfat use
    if (NOT(DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR} OR DEFINED ENV{DISRUPTOR_CPP_HOME}))
        message(FATAL_ERROR "Set EJFAT_ERSAP_INSTALL_DIR or DISRUPTOR_CPP_HOME env var so Disruptor libs and includes can be found")
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
    # Find where ET libs and includes are installed for ejfat use
    if (NOT DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR})
        message(FATAL_ERROR "Set EJFAT_ERSAP_INSTALL_DIR env var so ET libs and includes can be found")
    endif()

    find_package(ET)
    if (ET_FOUND)
        set(EXEC_FILES ${EXEC_FILES} ${ET_EXEC_FILES})
    else()
        message(STATUS "Et cannot be found, compile without it")
    endif()
else()
    message(STATUS "Compile without ET")
endif()


# GRPC build is optional and set it to OFF by default
option(BUILD_GRPC "Build grpc-related code" OFF)

if (BUILD_GRPC)
    # Find where GRPC and ERSAP backend libs and includes are installed for ejfat use
    if (NOT(DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR} OR DEFINED ENV{GRPC_INSTALL_DIR}))
        message(FATAL_ERROR "Set EJFAT_ERSAP_INSTALL_DIR or GRPC_INSTALL_DIR env var so GRPC and ERSAP backend libs and includes can be found")
    endif()

    find_package(GRPC)
    if (NOT GRPC_FOUND)
        message(STATUS "GRPC cannot be found, compile without it")
    else()
        message(STATUS "GRPC_LIBRARIES = " ${GRPC_LIBRARIES})
    endif()
else()
    message(STATUS "Compile without GRPC")
endif()


# Ersap build is optional and set it to OFF by default.
# Requires zmq lib.
option(BUILD_ERSAP "Build ERSAP engine and service" OFF)

if (BUILD_ERSAP)
    find_package(Ersap REQUIRED)

    add_subdirectory(ersap_engine/ejfat/legacy/src/main/cpp)
    add_subdirectory(ersap_engine/ejfat/services/src/main/cpp)
endif()



# CLAS12 source build is optional and set it to OFF by default
option(BUILD_CLAS "Build CLAS 12 source code" OFF)

# if you tell cmake to have code use USE_CLAS preprocessor macro
if (BUILD_CLAS)
    # Find where GRPC and ERSAP backend libs and includes are installed for ejfat use
    if (NOT(DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR} OR DEFINED ENV{HIPO_HOME}))
        message(FATAL_ERROR "Set EJFAT_ERSAP_INSTALL_DIR or HIPO_HOME env var so HIPO backend libs and includes can be found")
    endif()

    find_package(HIPO)
    if (NOT HIPO_FOUND)
        message(STATUS "Hipo cannot be found, compile without it")
    endif()
else()
    message(STATUS "Compile without hipo")
endif()


if (BUILD_TEST)
    set(EXEC_FILES ${EXEC_FILES} ${TEST_FILES})
endif()


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
        target_include_directories(${execName} PUBLIC   ${ET_INCLUDE_DIRS}
                                                        ${CMAKE_SOURCE_DIR}/simulation
                                                        ${CMAKE_SOURCE_DIR}/util)
        # Needs these libs
        target_link_libraries(${execName} pthread ${ZeroMQ_LIBRARIES} ${ET_LIBRARY})

        # Only install if installation directory has been defined
        if (DEFINED INSTALL_DIR_DEFINED)
            # Install into bin dir
            install(TARGETS ${execName} RUNTIME DESTINATION bin)
        endif()
    endforeach(fileName)


    if (BUILD_CLAS AND HIPO_FOUND)
        foreach(fileName ${CLAS_EXEC_FILES})
            # Get file name with no directory or extension as executable name
            get_filename_component(execName ${fileName} NAME_WE)
            # Create executable from file
            message(STATUS "Create normal executable " ${execName})
            add_executable(${execName} ${fileName})
            # Put debug extension on if applicable
            set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
            # Location of include files
            target_include_directories(${execName} PUBLIC   ${HIPO_INCLUDE_DIRS}
                    ${CMAKE_SOURCE_DIR}/simulation)
            # Needs these libs
            target_link_libraries(${execName} pthread ${HIPO_LIBRARIES})

            # Only install if installation directory has been defined
            if (DEFINED INSTALL_DIR_DEFINED)
                # Install into bin dir
                install(TARGETS ${execName} RUNTIME DESTINATION bin)
            endif()
        endforeach(fileName)
    endif()


    if (BUILD_DIS AND DISRUPTOR_FOUND)
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
                    ${Boost_INCLUDE_DIRS}
                    ${DISRUPTOR_INCLUDE_DIRS}
                    ${ET_INCLUDE_DIRS})
            # Needs these libs
            target_link_libraries(${execName} pthread ${Boost_LIBRARIES} ${DISRUPTOR_LIBRARY} ${ET_LIBRARY})

            # Only install if installation directory has been defined
            if (DEFINED INSTALL_DIR_DEFINED)
                # Install into bin dir
                install(TARGETS ${execName} RUNTIME DESTINATION bin)
            endif()
        endforeach(fileName)

        # Also create a utility library out of the utility files
        set (UTIL_LIB_NAME ejfat_util)
        set (UTIL_LIB_NAME_STATIC ejfat_util_st)

        add_library(${UTIL_LIB_NAME} SHARED ${UTIL_FILES})
        target_include_directories(${UTIL_LIB_NAME} PUBLIC
                ${CMAKE_SOURCE_DIR}/simulation
                ${CMAKE_SOURCE_DIR}/util
                ${Boost_INCLUDE_DIRS}
                ${DISRUPTOR_INCLUDE_DIRS})

        add_library(${UTIL_LIB_NAME_STATIC} STATIC ${UTIL_FILES})
        target_include_directories(${UTIL_LIB_NAME_STATIC} PUBLIC
                ${CMAKE_SOURCE_DIR}/simulation
                ${CMAKE_SOURCE_DIR}/util
                ${Boost_INCLUDE_DIRS}
                ${DISRUPTOR_INCLUDE_DIRS})

        if (DEFINED INSTALL_DIR_DEFINED)
            # Install library into lib dir
            install(TARGETS ${UTIL_LIB_NAME} LIBRARY DESTINATION lib)
            install(TARGETS ${UTIL_LIB_NAME_STATIC} LIBRARY DESTINATION lib)
            # Install headers into include dir
        endif()

    endif()

    if (BUILD_GRPC AND GRPC_FOUND)
        message(STATUS "TRY GRPC CODE:")

        # This files need util files added to executable
        foreach(fileName ${GRPC_EXEC_FILES})
            # Get file name with no directory or extension as executable name
            get_filename_component(execName ${fileName} NAME_WE)
            # Create executable from file
            message(STATUS "Create grpc-related executable " ${execName})
            # UTIL_FILES and GRPC_SRC_FILES are only really needed for packetBlasteeEtMT


            add_executable(${execName} ${fileName} ${UTIL_FILES} ${GRPC_SRC_FILES})

            # Put debug extension on if applicable
            set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
            # Location of include files
            target_include_directories(${execName} PUBLIC ${CMAKE_SOURCE_DIR}/simulation
                    ${CMAKE_SOURCE_DIR}/util
                    ${GRPC_INCLUDE_DIRS}
                    ${ET_INCLUDE_DIRS}
                    ${Boost_INCLUDE_DIRS}
                    ${DISRUPTOR_INCLUDE_DIRS})

            # Needs these libs
            target_link_libraries(${execName} pthread ${Boost_LIBRARIES} ${DISRUPTOR_LIBRARY} ${GRPC_LIBRARIES} ${ET_LIBRARY})

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



# Uninstall target
if(NOT TARGET uninstall)
    configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake" IMMEDIATE @ONLY)

    add_custom_target(uninstall
            COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
