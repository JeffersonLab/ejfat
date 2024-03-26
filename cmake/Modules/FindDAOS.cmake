# For the daos include files, we need to `sudo yum install daos-devel` first.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBDAOS QUIET libdaos)

if(PC_LIBDAOS_FOUND)
    # DAOS library found using pkg-config
    set(DAOS_FOUND TRUE)
    set(DAOS_VERSION ${PC_LIBDAOS_VERSION})
    set(DAOS_LIBRARIES ${PC_LIBDAOS_LIBRARIES})
else()
    find_path(DAOS_INCLUDE_DIR
        NAMES daos.h
        PATHS /usr/include
    )

    find_library(DAOS_LIBRARY
        NAMES libdaos.so.2  # explicitly name the daos lib based on your installation
        PATHS /usr/lib64)
endif()

if(DAOS_LIBRARY)
    set(DAOS_FOUND TRUE)
endif()

set(DAOS_LIBRARIES ${DAOS_LIBRARY})
set(DAOS_INCLUDE_DIRS  ${DAOS_INCLUDE_DIR})
message(STATUS "DAOS_LIBRARIES = " ${DAOS_LIBRARIES})
message(STATUS "DAOS_INCLUDE_DIRS = " ${DAOS_INCLUDE_DIRS})

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set ET_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( DAOS DEFAULT_MSG DAOS_LIBRARIES )
