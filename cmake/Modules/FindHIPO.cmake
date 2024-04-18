# Main Hipo lib and the accompanying lz4 lib.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBHIPO QUIET libhipo4)

set(HIPO_VERSION ${PC_LIBHIPO_VERSION})

find_path(HIPO_INCLUDE_DIR reader.h
        PATHS ${INSTALL_DIR}/include $ENV{HIPO_HOME}/hipo4)
message(STATUS "Looking in dir = " $ENV{HIPO_HOME})
message(STATUS "HIPO_INCLUDE_DIR = " ${HIPO_INCLUDE_DIR})

find_path(HIPO_LZ4_INCLUDE_DIR lz4.h
        PATHS ${INSTALL_DIR}/include $ENV{HIPO_HOME}/lz4/lib)
message(STATUS "HIPO_LZ4_INCLUDE_DIR = " ${HIPO_LZ4_INCLUDE_DIR})

find_library(HIPO_LIBRARY
        NAMES hipo4
        PATHS ${INSTALL_DIR}/lib $ENV{HIPO_HOME}/slib)
message(STATUS "HIPO_LIBRRY = " ${HIPO_LIBRARY})

find_library(HIPO_LZ4_LIBRARY
        NAMES lz4
        PATHS ${INSTALL_DIR}/lib $ENV{HIPO_HOME}/slib)
message(STATUS "HIPO_LZ4_LIBRRY = " ${HIPO_LZ4_LIBRARY})

if(HIPO_LIBRARY)
    set(HIPO_FOUND ON)
endif()

set ( HIPO_LIBRARIES  ${HIPO_LIBRARY} ${HIPO_LZ4_LIBRARY} )
set ( HIPO_INCLUDE_DIRS  ${HIPO_INCLUDE_DIR} ${HIPO_LZ4_INCLUDE_DIR} )
message(STATUS "HIPO_LIBRARIES = " ${HIPO_LIBRARIES})
message(STATUS "HIPO_INCLUDE_DIRS = " ${HIPO_INCLUDE_DIRS})

if(NOT TARGET libhipo4)
    add_library(libhipo4 UNKNOWN IMPORTED)
    add_library(liblz4 UNKNOWN IMPORTED)

    set_target_properties(libhipo4 PROPERTIES
            IMPORTED_LOCATION ${HIPO_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${HIPO_INCLUDE_DIR})

    set_target_properties(liblz4 PROPERTIES
            IMPORTED_LOCATION ${HIPO_LZ4_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${HIPO_LZ4_INCLUDE_DIR})
endif()

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set HIPO_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( HIPO DEFAULT_MSG HIPO_LIBRARIES HIPO_INCLUDE_DIRS )