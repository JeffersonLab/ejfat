
set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)


find_path(CROW_INCLUDE_DIR crow.h
        PATHS $ENV{CROW_INSTALL_DIR}/include)


if(CROW_INCLUDE_DIR)
    set(CROW_FOUND ON)
endif()

set ( CROW_INCLUDE_DIRS  ${CROW_INCLUDE_DIR} )

message(STATUS "CROW_INSTALL_DIR: " $ENV{CROW_INSTALL_DIR})
message(STATUS "CROW_INCLUDE_DIR: " ${CROW_INCLUDE_DIR})


include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set CROW_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( CROW DEFAULT_MSG CROW_INCLUDE_DIRS )