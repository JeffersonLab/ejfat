# Note to user:
# 99.999% of folks are only interested in ET_LIBRARY.
# The only users who would find ET_REMOTE_LIBRARY useful are those
# who are running on an operating system that can only use sockets
# (not memory mapped file), for example vxWorks.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBET QUIET libet)

set(ET_VERSION ${PC_LIBET_VERSION})

find_path(ET_INCLUDE_DIR et.h
        PATHS $ENV{CODA}/common/include)

find_library(ET_LIBRARY
        NAMES et
        PATHS $ENV{CODA}/*/lib)

find_library(ET_REMOTE_LIBRARY
        NAMES et_remote
        PATHS $ENV{CODA}/*/lib)

if(ET_LIBRARY)
    set(ET_FOUND ON)
endif()

set ( ET_LIBRARIES  ${ET_LIBRARY} )
set ( ET_INCLUDE_DIRS  ${ET_INCLUDE_DIR} )

if(NOT TARGET libet)
    add_library(libet UNKNOWN IMPORTED)
    add_library(libet_remote UNKNOWN IMPORTED)

    set_target_properties(libet PROPERTIES
            IMPORTED_LOCATION ${ET_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${ET_INCLUDE_DIRS})

    set_target_properties(libet_remote PROPERTIES
            IMPORTED_LOCATION ${ET_REMOTE_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${ET_INCLUDE_DIRS})
endif()

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set ET_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( ET DEFAULT_MSG ET_LIBRARIES ET_INCLUDE_DIRS )