# file for cmake to find local gRPC installation

find_package(PkgConfig REQUIRED)

# Ensure the gPRC_INSTALL_DIR is set
if(NOT DEFINED ENV{GRPC_INSTALL_DIR})
    message(FATAL_ERROR "GRPC_INSTALL_DIR environment variable is not set.")
endif()

# The base directory of the gRPC installation
set(GRPC_INSTALL_DIR $ENV{GRPC_INSTALL_DIR})

# Search for the libraries: libgrpc++.so and libgrpc++_reflection.so
find_library(GRPC_LIBRARY grpc++ HINTS ${GRPC_INSTALL_DIR}/lib)
find_library(GRPC_REFLECTION_LIBRARY grpc++_reflection HINTS ${GRPC_INSTALL_DIR}/lib)

# Check if both libraries are found
if(NOT GRPC_LIBRARY)
    message(FATAL_ERROR "Failed to find libgrpc++.so in ${GRPC_INSTALL_DIR}/lib")
endif()

if(NOT GRPC_REFLECTION_LIBRARY)
    message(FATAL_ERROR "Failed to find libgrpc++_reflection.so in ${GRPC_INSTALL_DIR}/lib")
endif()

# If libraries are found, provide results
message(STATUS "Found gRPC++ library: ${GRPC_LIBRARY}")
message(STATUS "Found gRPC++ reflection library: ${GRPC_REFLECTION_LIBRARY}")

# Optionally, add other paths such as include directories or additional libraries
find_path(GRPC_INCLUDE_DIR grpc++/grpc++.h HINTS ${GRPC_INSTALL_DIR}/include)

# Provide the include directory if found
if(GRPC_INCLUDE_DIR)
    message(STATUS "Found gRPC include directory: ${GRPC_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "Failed to find gRPC include directory in ${GRPC_INSTALL_DIR}/include/grpc++")
endif()



if(GRPC_LIBRARY)
    set(GRPC_FOUND ON)
endif()

set ( GRPC_LIBRARIES  ${GRPC_LIBRARY} ${GPRC_REFLECTION_LIBRARY} )
set ( GRPC_INCLUDE_DIRS  ${GRPC_INCLUDE_DIR} )


if(NOT TARGET libgrpc++)
    add_library(libgrpc++ UNKNOWN IMPORTED)
    add_library(libgrpc++_reflection UNKNOWN IMPORTED)

    set_target_properties(libgrpc++ PROPERTIES
            IMPORTED_LOCATION ${GRPC_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${GRPC_INCLUDE_DIRS})

    set_target_properties(libgrpc++_reflection PROPERTIES
            IMPORTED_LOCATION ${GRPC_REFLECTION_LIBRARY}
            INTERFACE_INCLUDE_DIRECTORIES ${GRPC_INCLUDE_DIRS})
endif()

include ( FindPackageHandleStandardArgs )
# handle the QUIETLY and REQUIRED arguments and set ET_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args ( GRPC DEFAULT_MSG GRPC_LIBRARIES GRPC_INCLUDE_DIRS )



# Return the found libraries and include directories
#set(GRPC_LIBRARIES_FOUND TRUE CACHE INTERNAL "gRPC Libraries found")
#set(GRPC_INCLUDE_DIR_FOUND TRUE CACHE INTERNAL "gRPC Include directory found")





