# Note to user:
# 99.999% of folks are only interested in ET_LIBRARY.
# The only users who would find ET_REMOTE_LIBRARY useful are those
# who are running on an operating system that can only use sockets
# (not memory mapped file), for example vxWorks.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)


set(CROW_VERSION ${PC_LIBCROW_VERSION})

find_path(CROW_INCLUDE_DIR crow.h
        PATHS $ENV{CROW_INSTALL_DIR}/include)


if(CROW_INCLUDE_DIR)
    set(CROW_FOUND ON)
endif()

set ( CROW_INCLUDE_DIRS  ${CROW_INCLUDE_DIR} )


include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set CROW_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( CROW DEFAULT_MSG CROW_INCLUDE_DIRS )