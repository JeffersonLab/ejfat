# To use this, do the following the first time:
#   % mkdir build
#   % cd build
#   % cmake .. -DBUILD_SIMPLE=1 -DBUILD_CLAS=1 -DBUILD_OLD=1 -DBUILD_BACKEND=1
#   % make (install)
#
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
        message(FATAL_ERROR "Define EJFAT_ERSAP_INSTALL_DIR or specify -DINSTALL_DIR=<...> for installation & lib/header location")
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


# Michael's old files used with static LB, now obsolete, not compiled by default,
# no dependencies
set(EMU_EXEC_FILES
        emulation/lb_emu.cc
        emulation/lb_rcv.cc
        emulation/lb_rcv-roc.cc
        emulation/pktzr_md.cc
        emulation/lb_send.cc
        )

# Carl's old files used with static LB, now obsolete, not compiled by default,
# no dependencies
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
        simulation/packetBlasteeFastDirect.cc
        )

# Regular backends: boost, grpc, ejfat_grpc, protobuf
set(BACKEND_EXEC_FILES
        simulation/packetBlasteeFastMP.cc
        simulation/packetBlasteeFullNewMP.cc
        )

# To be used with other utilities files (and therefore disruptor) and ET for use in backends
set(BACKEND_SRC_FILES
        simulation/EtFifoEntryItem.cpp
        )

# Backend with extras: ET, boost, grpc, ejfa_grpc, protobuf, Disruptor, Prometheus, asio, crow dependencies. Basically everything.
set(BACKEND_EXEC_FILES_2
#set(ERSAP_EXEC_FILES
        simulation/packetBlasteeEtMT.cc
        simulation/packetBlasteeEtMTPRT.cc
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

# Files for sending clas 12 data: hipo lib
set(CLAS_EXEC_FILES
        clas_source/clasBlaster.cc
        clas_source/clasBlasterNTP.cc
        )

# List of all test files
set(TEST_FILES
        )

# List of all util directory header files
set(UTIL_HEADER_FILES
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

# List of all header files
set(CPP_HEADER_FILES
        simulation/ejfat.hpp
        simulation/ejfat_packetize.hpp
        simulation/ejfat_assemble_ersap.hpp
        simulation/EtFifoEntryItem.h
        ${UTIL_HEADER_FILES}
        )

# For doxygen purposes, list of all simple API files
set(SIMPLE_API_FILES
        simpleAPI/EjfatException.h
        simpleAPI/EjfatConsumer.cpp
        simpleAPI/EjfatConsumer.h
        simpleAPI/EjfatProducer.cpp
        simpleAPI/EjfatProducer.h
        )


# Make the doxygen documentation
find_package( Doxygen )

if ( DOXYGEN_FOUND )
    set( DOXYGEN_OUTPUT_DIRECTORY ../docs/doxygen )
    set( DOXYGEN_COLLABORATION_GRAPH YES )
    set( DOXYGEN_EXTRACT_ALL YES )
    set( DOXYGEN_CLASS_DIAGRAMS YES )
    set( DOXYGEN_HIDE_UNDOC_RELATIONS NO )
    set( DOXYGEN_HAVE_DOT YES )
    set( DOXYGEN_CLASS_GRAPH YES )
    set( DOXYGEN_CALL_GRAPH YES )
    set( DOXYGEN_CALLER_GRAPH YES )
    set( DOXYGEN_COLLABORATION_GRAPH YES )
    set( DOXYGEN_BUILTIN_STL_SUPPORT YES )
#    set( DOXYGEN_EXTRACT_PRIVATE YES )
#    set( DOXYGEN_EXTRACT_PACKAGE YES )
    set( DOXYGEN_EXTRACT_STATIC YES )
#    set( DOXYGEN_EXTRACT_LOCALMETHODS YES )
    set( DOXYGEN_UML_LOOK YES )
    set( DOXYGEN_UML_LIMIT_NUM_FIELDS 50 )
    set( DOXYGEN_TEMPLATE_RELATIONS YES )
    set( DOXYGEN_DOT_GRAPH_MAX_NODES 100 )
    set( DOXYGEN_MAX_DOT_GRAPH_DEPTH 0 )
    set( DOXYGEN_DOT_TRANSPARENT YES )

    doxygen_add_docs( doxygen  ${SIMPLE_API_FILES} ${UTIL_FILES} ${CPP_HEADER_FILES})
else()
    message( "Doxygen need to be installed to generate the doxygen documentation" )
endif()


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
    # Need boost for Simple API
    if (NOT DEFINED Boost_FOUND)
        find_package(Boost REQUIRED system thread chrono)
    endif()
    if (NOT Boost_FOUND)
        message(FATAL_ERROR "Boost cannot be found, cmake will exit." )
    endif()

    # Find Protobuf installation
    # Looks for protobuf-config.cmake file installed by Protobuf's cmake installation.
    if (NOT DEFINED Protobuf_FOUND)
        find_package(Protobuf CONFIG REQUIRED)
        message(STATUS "Using protobuf ${Protobuf_VERSION}")
        set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
    endif()
    if (NOT Protobuf_FOUND)
        message(FATAL_ERROR "Protobuf cannot be found, cmake will exit." )
    endif()

    # Find gRPC installation
    # Looks for gRPCConfig.cmake file installed by gRPC's cmake installation.
    if (NOT DEFINED gRPC_FOUND)
        find_package(gRPC)
    endif()
    if (NOT gRPC_FOUND)
        # Grpc cannot be found, let's try again by using env var
        if (NOT DEFINED ENV{GRPC_INSTALL_DIR})
            if (NOT DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR})
                message(FATAL_ERROR "Define GRPC_INSTALL_DIR or EJFAT_ERSAP_INSTALL_DIR so gRPC can be found")
            else()
                set(GRPC_INSTALL_DIR $ENV{EJFAT_ERSAP_INSTALL_DIR})
            endif()
        else()
            set(GRPC_INSTALL_DIR $ENV{GRPC_INSTALL_DIR})
        endif()

        find_package(gRPC REQUIRED CONFIG PATHS ${GRPC_INSTALL_DR}/lib/cmake/grpc)
    endif()
    if (NOT gRPC_FOUND)
        message(FATAL_ERROR "grpc cannot be found, cmake will exit." )
    endif()

    message(STATUS "Using gRPC ${gRPC_VERSION}")
    set(_REFLECTION gRPC::grpc++_reflection)
    set(_GRPC_GRPCPP gRPC::grpc++)
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

    # Find where ejfat_grpc lib and includes are installed
    if (NOT DEFINED Ejfat_grpc_FOUND)
        find_package(Ejfat_grpc)
    endif()
    if (NOT Ejfat_grpc_FOUND)
        message(STATUS "ejfat_grpc lib cannot be found, compile without it")
    endif()

    # Go to the cmake file in simpleAPI directory
    add_subdirectory(simpleAPI)
endif()


# Building regular EJFAT backends is optional and set it to OFF by default
option(BUILD_BACKEND "Build EJFAT backend related code" OFF)

if (BUILD_BACKEND)
    # Need boost for Disruptor
    if (NOT DEFINED Boost_FOUND)
        find_package(Boost REQUIRED system thread chrono)
    endif()
    if (NOT Boost_FOUND)
        message(FATAL_ERROR "Boost cannot be found, cmake will exit." )
    endif()


    # Find where Disruptor libs and includes are installed for ejfat use
    if (NOT DEFINED DISRUPTOR_FOUND)
        find_package(DISRUPTOR)
    endif()
    if (NOT DISRUPTOR_FOUND)
        message(FATAL_ERROR "Disruptor cannot be found, cmake will exit." )
    endif()


    # Find Protobuf installation
    # Looks for protobuf-config.cmake file installed by Protobuf's cmake installation.
    if (NOT DEFINED Protobuf_FOUND)
        find_package(Protobuf CONFIG REQUIRED)
        message(STATUS "Using protobuf ${Protobuf_VERSION}")
        set(_PROTOBUF_LIBPROTOBUF protobuf::libprotobuf)
    endif()
    if (NOT Protobuf_FOUND)
        message(FATAL_ERROR "Protobuf cannot be found, cmake will exit." )
    endif()


    # Find gRPC installation
    # Looks for gRPCConfig.cmake file installed by gRPC's cmake installation.
    if (NOT DEFINED gRPC_FOUND)
        find_package(gRPC CONFIG REQUIRED)
    endif()
    if (NOT gRPC_FOUND)
        # Grpc cannot be found, let's try again by using env var
        if (NOT DEFINED ENV{GRPC_INSTALL_DIR})
            if (NOT DEFINED ENV{EJFAT_ERSAP_INSTALL_DIR})
                message(FATAL_ERROR "Define GRPC_INSTALL_DIR or EJFAT_ERSAP_INSTALL_DIR so gRPC can be found")
            else()
                set(GRPC_INSTALL_DIR $ENV{EJFAT_ERSAP_INSTALL_DIR})
            endif()
        else()
            set(GRPC_INSTALL_DIR $ENV{GRPC_INSTALL_DIR})
        endif()

        find_package(gRPC REQUIRED CONFIG PATHS ${GRPC_INSTALL_DR}/lib/cmake/grpc)
    endif()
    if (NOT gRPC_FOUND)
        message(FATAL_ERROR "grpc cannot be found, cmake will exit." )
    endif()

    message(STATUS "Using gRPC ${gRPC_VERSION}")
    set(_REFLECTION gRPC::grpc++_reflection)
    set(_GRPC_GRPCPP gRPC::grpc++)
    set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)


    # Find where ejfat_grpc lib and includes are installed
    if (NOT DEFINED Ejfat_grpc_FOUND)
        find_package(Ejfat_grpc)
    endif()
    if (NOT Ejfat_grpc_FOUND)
        message(STATUS "ejfat_grpc lib cannot be found, compile without it")
    endif()


    #----------------------------------------------------------
    # For executables in BACKEND_EXEC_FILES_2 to be compiled,
    # the following additional packages are required.
    #----------------------------------------------------------


    # Need ET for ERSAP CODE
    find_package(ET)
    if (NOT ET_FOUND)
        message(FATAL_ERROR "Et cannot be found, cmake will exit")
    endif()


    # for clasBlasteeEtNTPRT
    find_package(ASIO REQUIRED)
    find_package(CROW REQUIRED)
    find_package(prometheus-cpp REQUIRED CONFIG)

endif()



# CLAS12 source build is optional and set it to OFF by default
option(BUILD_CLAS "Build CLAS 12 source code" OFF)

# if you tell cmake to have code use USE_CLAS preprocessor macro
if (BUILD_CLAS)
    # Find where CLAS libs and includes are installed for ejfat use.
    # If nothing is found in INSTALL_DIR, then $ENV{HIPO_HOME} will be searched
    find_package(HIPO)
    if (NOT HIPO_FOUND)
        message(FATAL_ERROR "Hipo cannot be found, exit cmake")
    endif()
endif()


if (BUILD_TEST)
    set(EXEC_FILES ${EXEC_FILES} ${TEST_FILES})
endif()


    # No outside dependency programs
    foreach(fileName ${EXEC_FILES})
        # Get file name with no directory or extension as executable name
        get_filename_component(execName ${fileName} NAME_WE)
        # Create executable from file
        message(STATUS "Create normal executable " ${execName})
        add_executable(${execName} ${fileName})
        # Put debug extension on if applicable
        set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
        # Location of include files
        target_include_directories(${execName} PUBLIC  ${CMAKE_SOURCE_DIR}/simulation
                                                       ${CMAKE_SOURCE_DIR}/util)
        # Needs these libs
        target_link_libraries(${execName} pthread)

        # Only install if installation directory has been defined
        if (INSTALL_DIR_DEFINED)
            # Install into bin dir
            install(TARGETS ${execName} RUNTIME DESTINATION bin)
        endif()
    endforeach(fileName)


    if (BUILD_CLAS)
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
            if (INSTALL_DIR_DEFINED)
                # Install into bin dir
                install(TARGETS ${execName} RUNTIME DESTINATION bin)
            endif()
        endforeach(fileName)
    endif()


    if (BUILD_BACKEND)
        message(STATUS "TRY BACKEND CODE:")

        # This files need util files added to executable
        foreach(fileName ${BACKEND_EXEC_FILES})
            # Get file name with no directory or extension as executable name
            get_filename_component(execName ${fileName} NAME_WE)
            # Create executable from file
            message(STATUS "Create backend-related executable " ${execName})
            add_executable(${execName} ${fileName} ${UTIL_FILES})
            # Put debug extension on if applicable
            set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
            # Location of include files
            target_include_directories(${execName} PUBLIC
                    ${CMAKE_SOURCE_DIR}/simulation
                    ${CMAKE_SOURCE_DIR}/util
                    ${Boost_INCLUDE_DIRS}
                    ${Ejfat_grpc_INCLUDE_DIRS}
                    ${gRPC_INCLUDE_DIRS}
                    ${DISRUPTOR_INCLUDE_DIRS}
                    )
            # Needs these libs
            target_link_libraries(${execName} pthread
                    ${Boost_LIBRARIES}
                    ${_REFLECTION}
                    ${_GRPC_GRPCPP}
                    ${Ejfat_grpc_LIBRARIES}
                    ${DISRUPTOR_LIBRARIES}
                    )

            # Only install if installation directory has been defined
            if (INSTALL_DIR_DEFINED)
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
        target_link_libraries(${UTIL_LIB_NAME} pthread ${DISRUPTOR_LIBRARIES})

        add_library(${UTIL_LIB_NAME_STATIC} STATIC ${UTIL_FILES})
        target_include_directories(${UTIL_LIB_NAME_STATIC} PUBLIC
                ${CMAKE_SOURCE_DIR}/simulation
                ${CMAKE_SOURCE_DIR}/util
                ${Boost_INCLUDE_DIRS}
                ${DISRUPTOR_INCLUDE_DIRS})
        target_link_libraries(${UTIL_LIB_NAME_STATIC} pthread ${DISRUPTOR_LIBRARIES})

        if (DEFINED INSTALL_DIR_DEFINED)
            # Install library into lib dir
            install(TARGETS ${UTIL_LIB_NAME} LIBRARY DESTINATION lib)
            install(TARGETS ${UTIL_LIB_NAME_STATIC} LIBRARY DESTINATION lib)
            # Install headers into include dir
        endif()


#        add_executable(packetBlasteeEtMTPRT simulation/packetBlasteeEtMTPRT.cc ${UTIL_FILES} ${GRPC_SRC_FILES})
#
#        target_include_directories(packetBlasteeEtMTPRT PUBLIC ${CMAKE_SOURCE_DIR}/simulation
#                ${ASIO_INCLUDE_DIRS}
#                ${CROW_INCLUDE_DIRS}
#                ${CMAKE_SOURCE_DIR}/util
#                ${GRPC_INCLUDE_DIRS}
#                ${ET_INCLUDE_DIRS}
#                ${Boost_INCLUDE_DIRS}
#                ${DISRUPTOR_INCLUDE_DIRS})
#
#        target_link_libraries(packetBlasteeEtMTPRT PRIVATE prometheus-cpp::core prometheus-cpp::pull pthread ${Boost_LIBRARIES} ${DISRUPTOR_LIBRARY} ${GRPC_LIBRARIES} ${ET_LIBRARY})
#
#        if (INSTALL_DIR_DEFINED)
#            # Install into bin dir
#            install(TARGETS packetBlasteeEtMTPRT RUNTIME DESTINATION bin)
#        endif()
#
#        # -------- Prometheus stuff ----------------------------------------


        # This files need util files added to executable
        foreach(fileName ${BACKEND_EXEC_FILES_2})
            # Get file name with no directory or extension as executable name
            get_filename_component(execName ${fileName} NAME_WE)
            # Create executable from file
            message(STATUS "Create grpc-related executable " ${execName})
            # UTIL_FILES and BACKEND_SRC_FILES are only really needed for packetBlasteeEtMT
            add_executable(${execName} ${fileName} ${UTIL_FILES} ${BACKEND_SRC_FILES})
            # Put debug extension on if applicable
            set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
            # Location of include files
            target_include_directories(${execName} PUBLIC
                    ${CMAKE_SOURCE_DIR}/simulation
                    ${ASIO_INCLUDE_DIRS}
                    ${CROW_INCLUDE_DIRS}
                    ${CMAKE_SOURCE_DIR}/util
                    ${Ejfat_grpc_INCLUDE_DIRS}
                    ${gRPC_INCLUDE_DIRS}
                    ${ET_INCLUDE_DIRS}
                    ${Boost_INCLUDE_DIRS}
                    ${DISRUPTOR_INCLUDE_DIRS})

            # Needs these libs
            target_link_libraries(${execName} PRIVATE pthread
                    prometheus-cpp::core prometheus-cpp::pull
                    ${Boost_LIBRARIES}
                    ${_REFLECTION}
                    ${_GRPC_GRPCPP}
                    ${Ejfat_grpc_LIBRARIES}
                    ${DISRUPTOR_LIBRARIES}
                    ${ET_LIBRARY})

            # Only install if installation directory has been defined
            if (INSTALL_DIR_DEFINED)
                # Install into bin dir
                install(TARGETS ${execName} RUNTIME DESTINATION bin)
            endif()
        endforeach(fileName)
    endif()



# Only install if installation directory has been defined
if (INSTALL_DIR_DEFINED)
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
