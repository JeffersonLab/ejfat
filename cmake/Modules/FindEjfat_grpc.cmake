# Note to user:
# 99.999% of folks are only interested in ET_LIBRARY.
# The only users who would find ET_REMOTE_LIBRARY useful are those
# who are running on an operating system that can only use sockets
# (not memory mapped file), for example vxWorks.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBGRPC QUIET ejfat_grpc)

set(GRPC_VERSION 1.0)

find_path(Ejfat_grpc_INCLUDE_DIR lb_cplane.h
        PATHS ${INSTALL_DIR}/include ${EJFAT_ERSAP_INSTALL_DIR}/include $ENV{GRPC_INSTALL_DIR}/include)

find_library(Ejfat_grpc_LIBRARY
        NAMES ejfat_grpc
        PATHS ${INSTALL_DIR}/lib ${EJFAT_ERSAP_INSTALL_DIR}/lib $ENV{GRPC_INSTALL_DIR}/lib)

if(Ejfat_grpc_LIBRARY)
    set(Ejfat_grpc_FOUND ON)
endif()

set(Ejfat_grpc_LIBRARIES ${Ejfat_grpc_LIBRARY})
set(Ejfat_grpc_INCLUDE_DIRS ${Ejfat_grpc_INCLUDE_DIR})

message(STATUS "Ejfat_grpc_LIBRARIES = " ${Ejfat_grpc_LIBRARIES})
message(STATUS "Ejfat_grpc_INCLUDE_DIRS = " ${Ejfat_grpc_INCLUDE_DIRS})

if(NOT TARGET ejfat_grpc)
    add_library(ejfat_grpc UNKNOWN IMPORTED)

    set_target_properties(ejfat_grpc PROPERTIES
            IMPORTED_LOCATION ${Ejfat_grpc_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${Ejfat_grpc_INCLUDE_DIRS})
endif()

include (FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set GRPC_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args (Ejfat_grpc DEFAULT_MSG Ejfat_grpc_LIBRARIES Ejfat_grpc_INCLUDE_DIRS )