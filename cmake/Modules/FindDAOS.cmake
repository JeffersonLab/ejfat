# For the daos include files, we need to `sudo yum install daos-devel` first.

set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)
pkg_check_modules(PC_LIBDAOS QUIET libdaos)

if(PC_LIBDAOS_FOUND)
    # DAOS library found using pkg-config
    set(DAOS_VERSION ${PC_LIBDAOS_VERSION})
    set(DAOS_LIBRARY ${PC_LIBDAOS_LIBRARIES})
    set(DAOS_INCLUDE_DIR ${PC_LIBDAOS_INCLUDE_DIRS})
else()
    # Fallback: Find DAOS headers and libraries manually
    find_path(DAOS_INCLUDE_DIR
        NAMES daos.h
        PATHS /usr/include
    )

    find_path(DFS_INCLUDE_DIR
        NAMES daos_fs.h daos_fs_sys.h
        PATHS /usr/include
    )

    find_library(DAOS_LIBRARY
        NAMES libdaos.so
        PATHS /usr/lib64
    )

    find_library(DFS_LIBRARY
        NAMES libdfs.so
        PATHS /usr/lib64
    )

    if (DAOS_INCLUDE_DIR AND DFS_INCLUDE_DIR AND DAOS_LIBRARY AND DFS_LIBRARY)
        set(DAOS_INCLUDE_DIRS ${DAOS_INCLUDE_DIR} ${DFS_INCLUDE_DIR})
        set(DAOS_LIBRARIES ${DAOS_LIBRARY} ${DFS_LIBRARY})
    else()
        message(FATAL_ERROR "DAOS libraries or headers not found")
    endif()
endif()

if(DAOS_LIBRARIES)
    set(DAOS_FOUND TRUE)
endif()

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set ET_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( DAOS DEFAULT_MSG DAOS_LIBRARIES )
