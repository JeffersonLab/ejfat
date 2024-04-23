set(PKG_CONFIG_USE_CMAKE_PREFIX_PATH ON)
find_package(PkgConfig)



find_path(ASIO_INCLUDE_DIR asio.hpp
        PATHS $ENV{ASIO_INSTALL_DIR}/include)


if(ASIO_INCLUDE_DIR)
    set(ASIO_FOUND ON)
endif()

set ( ASIO_INCLUDE_DIRS  ${ASIO_INCLUDE_DIR} )


include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set ASIO_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args (ASIO DEFAULT_MSG ASIO_INCLUDE_DIRS)
