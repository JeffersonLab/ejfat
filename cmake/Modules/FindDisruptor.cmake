# Note to user:
# 99.999% of folks are only interested in ET_LIBRARY.
# The only users who would find ET_REMOTE_LIBRARY useful are those
# who are running on an operating system that can only use sockets
# (not memory mapped file), for example vxWorks.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBDISRUPTOR QUIET libDisruptor)

set(ET_VERSION ${PC_LIBDISRUPTOR_VERSION})

find_path(DISRUPTOR_INCLUDE_DIR Disruptor/Disruptor.h
        PATHS $ENV{CODA}/common/include $ENV{DISRUPTOR_CPP_HOME})

find_library(DISRUPTOR_LIBRARY
             NAMES Disruptor
             PATHS $ENV{CODA}/*/lib $ENV{DISRUPTOR_CPP_HOME}/build/Disruptor
             NO_DEFAULT_PATH
            )

if(DISRUPTOR_LIBRARY)
    set(DISRUPTOR_FOUND ON)
endif()

set ( DISRUPTOR_LIBRARIES  ${DISRUPTOR_LIBRARY} )
# backup one directory as header files are of form: Disruptor/<header file name>
set ( DISRUPTOR_INCLUDE_DIR  ${DISRUPTOR_INCLUDE_DIR} )
set ( DISRUPTOR_INCLUDE_DIRS  ${DISRUPTOR_INCLUDE_DIR} )

if(NOT TARGET libDisruptor)
    add_library(libDisruptor UNKNOWN IMPORTED)

    set_target_properties(libDisruptor PROPERTIES
            IMPORTED_LOCATION ${DISRUPTOR_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${DISRUPTOR_INCLUDE_DIRS})

endif()

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set Disruptor_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( DISRUPTOR DEFAULT_MSG DISRUPTOR_LIBRARIES DISRUPTOR_INCLUDE_DIRS )




