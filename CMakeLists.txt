# To use this, do the following the first time:
#   % mkdir build
#   % cd build
#   % cmake .. -DCMAKE_BUILD_TYPE=release
#   % make (install)
#
# To only build C libraries and executables, replace the 3rd cammand above with
#   % cmake .. -DCMAKE_BUILD_TYPE=release -DC_ONLY=1
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

# This call must be placed BEFORE "project" command
cmake_minimum_required(VERSION 3.10)


project(ejfat VERSION 0.1)


set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_DEBUG_POSTFIX -dbg)

set(MAKE_EXAMPLES 1)


# Place libs & binaries in build/lib and bin (this is not for installation)
set(LIBRARY_OUTPUT_PATH    ${CMAKE_BINARY_DIR}/lib)
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)


# Set architecture-related string for installation consistent with scons file
set(ARCH ${CMAKE_HOST_SYSTEM_NAME}-${CMAKE_HOST_SYSTEM_PROCESSOR})
message(STATUS "ARCH = " ${ARCH})


# Install into $INSTALL_DIR directory
if (DEFINED INSTALL_DIR)
    message(STATUS "INSTALL_DIR = " ${INSTALL_DIR})
    set(INSTALL_DIR_DEFINED 1)
    set(CMAKE_INSTALL_PREFIX ${INSTALL_DIR}/${ARCH})
    set(CMAKE_INSTALL_INCLUDEDIR ${INSTALL_DIR}/common/include)
    message(STATUS "CMAKE_INSTALL_PREFIX reset to " ${CMAKE_INSTALL_PREFIX})
else()
    message(STATUS "Set the \"CODA\" env var or specify -DCODA_INSTALL=... on the command line or no installation possible!")
endif()


set(EXEC_FILES
        simulation/udp_data_rcv.c
        simulation/udp_data_send.c
        simulation/udp_rcv_order.cc
        simulation/udp_send_order.cc
        simulation/udp_rcv.c
        simulation/udp_send.c
        emulation/lb_emu.cc
        emulation/lb_rcv.cc
        emulation/pktzr_md.cc
        )

set(C_HEADER_FILES
        )

set(C_LIB_FILES
        )

set(CPP_HEADER_FILES
        simulation/ejfat_packetizer.hpp
        simulation/ejfat_assemble.hpp
        )

set(CPP_LIB_FILES
        )


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
if (DEFINED MAKE_EXAMPLES)
    foreach(fileName ${EXEC_FILES})
        # Get file name with no directory or extension as executable name
        get_filename_component(execName ${fileName} NAME_WE)
        # Create executable from file
        add_executable(${execName} ${fileName})
        # Put debug extension on if applicable
        set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
        # Needs these libs
        target_link_libraries(${execName})

        # Only install if installation directory has been defined
        if (DEFINED INSTALL_DIR_DEFINED)
            # Install into bin dir
            install(TARGETS ${execName} RUNTIME DESTINATION bin)
        endif()
    endforeach(fileName)
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
