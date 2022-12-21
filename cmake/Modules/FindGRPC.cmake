# Note to user:
# 99.999% of folks are only interested in ET_LIBRARY.
# The only users who would find ET_REMOTE_LIBRARY useful are those
# who are running on an operating system that can only use sockets
# (not memory mapped file), for example vxWorks.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBGRPC QUIET libgrpc)

set(GRPC_VERSION ${PC_LIBGRPC_VERSION})

find_path(GRPC_INCLUDE_DIR grpc/grpc.h
        PATHS $ENV{GRPC_INSTALL_DIR}/include)

find_library(GRPC_LIBRARY
        NAMES grpc
        PATHS $ENV{GRPC_INSTALL_DIR}/lib)

find_library(GRPC++_LIBRARY
        NAMES grpc++
        PATHS $ENV{GRPC_INSTALL_DIR}/lib)

find_library(GRPC++_REFLECTION_LIBRARY
        NAMES grpc++_reflection
        PATHS $ENV{GRPC_INSTALL_DIR}/lib)

find_library(PROTOBUF_LIBRARY
        NAMES protobuf
        PATHS $ENV{GRPC_INSTALL_DIR}/lib)


if(GRPC_LIBRARY)
    set(GRPC_FOUND ON)
endif()

set ( GRPC_LIBRARIES ${GRPC_LIBRARY} ${GRPC++_LIBRARY} ${GRPC++_REFLECTION_LIBRARY} ${PROTOBUF_LIBRARY})
set ( GRPC_INCLUDE_DIRS  ${GRPC_INCLUDE_DIR} )

if(NOT TARGET libgrpc)
    add_library(libgrpc UNKNOWN IMPORTED)
    add_library(libgrpc++ UNKNOWN IMPORTED)
    add_library(libgrpc++_reflection UNKNOWN IMPORTED)
    add_library(libprotobuf UNKNOWN IMPORTED)

    set_target_properties(libgrpc PROPERTIES
            IMPORTED_LOCATION ${GRPC_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${GRPC_INCLUDE_DIRS})

    set_target_properties(libgrpc++ PROPERTIES
            IMPORTED_LOCATION ${GRPC++_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${GRPC_INCLUDE_DIRS})

    set_target_properties(libgrpc++_reflection PROPERTIES
            IMPORTED_LOCATION ${GRPC++_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${GRPC_INCLUDE_DIRS})

    set_target_properties(libprotobuf PROPERTIES
            IMPORTED_LOCATION ${GRPC++_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${GRPC_INCLUDE_DIRS})

endif()

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set GRPC_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( GRPC DEFAULT_MSG GRPC_LIBRARIES GRPC_INCLUDE_DIRS )