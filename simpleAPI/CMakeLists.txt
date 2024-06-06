
set(NO_GRPC_SRC
        serverProducer.cpp
        serverConsumer.cpp
   )

set(NO_GRPC_EXECUTABLES
        simpleServerSender.cpp
        simpleServerConsumer.cpp
   )

set(SIMPLE_SRC
        EjfatConsumer.cpp
        EjfatProducer.cpp
        EjfatServer.cpp
   )

set(EXECUTABLE_FILES
#        lbtest.cpp
        lbmonitor.cpp
        lbfree.cpp
        lbreserve.cpp
        lbgeturi.cpp
        simpleSender.cpp
        simpleConsumer.cpp
        simpleServer.cpp
   )

set(HEADER_FILES
        EjfatConsumer.h
        EjfatProducer.h
        EjfatServer.h
        serverConsumer.h
        serverProducer.h
        simpleEjfat.h
        EjfatException.h
   )


add_library(ejfat_simple SHARED ${SIMPLE_SRC})

target_include_directories(ejfat_simple PUBLIC
        ${CMAKE_SOURCE_DIR}/simpleAPI
        ${CMAKE_SOURCE_DIR}/simulation
        ${Boost_INCLUDE_DIRS}
        ${Ejfat_grpc_INCLUDE_DIRS}
        ${gRPC_INCLUDE_DIRS}
        )
target_link_libraries(ejfat_simple PUBLIC pthread
        ${Boost_LIBRARIES}
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${Ejfat_grpc_LIBRARIES}
        )


# This files need util files added to executable
foreach(fileName ${NO_GRPC_EXECUTABLES})
    # Get file name with no directory or extension as executable name
    get_filename_component(execName ${fileName} NAME_WE)
    # Create executable from file
    message(STATUS "Create grpc-related executable " ${execName})
    add_executable(${execName} ${fileName} ${NO_GRPC_SRC})
    # Put debug extension on if applicable
    set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
    # Location of include files
    target_include_directories(${execName} PUBLIC
            ${CMAKE_SOURCE_DIR}/simpleAPI
            ${CMAKE_SOURCE_DIR}/simulation
            ${Boost_INCLUDE_DIRS}
            )

    # Needs these libs
    target_link_libraries(${execName}  pthread
            ${Boost_LIBRARIES}
            )

    # Only install if installation directory has been defined
    if (INSTALL_DIR_DEFINED)
        # Install into bin dir
        install(TARGETS ${execName} RUNTIME DESTINATION bin)
    endif()
endforeach(fileName)



# This files need util files added to executable
foreach(fileName ${EXECUTABLE_FILES})
    # Get file name with no directory or extension as executable name
    get_filename_component(execName ${fileName} NAME_WE)
    # Create executable from file
    message(STATUS "Create grpc-related executable " ${execName})
    add_executable(${execName} ${fileName})
    # Put debug extension on if applicable
    set_target_properties(${execName} PROPERTIES DEBUG_POSTFIX ${CMAKE_DEBUG_POSTFIX})
    # Location of include files
    target_include_directories(${execName} PUBLIC
            ${CMAKE_SOURCE_DIR}/simpleAPI
            ${CMAKE_SOURCE_DIR}/simulation
            ${Boost_INCLUDE_DIRS}
            ${Ejfat_grpc_INCLUDE_DIRS}
            ${gRPC_INCLUDE_DIRS}
            )

    # Needs these libs
    target_link_libraries(${execName}  pthread
            ${Boost_LIBRARIES}
            ${_REFLECTION}
            ${_GRPC_GRPCPP}
            ${Ejfat_grpc_LIBRARIES}
            ejfat_simple)

    # Only install if installation directory has been defined
    if (INSTALL_DIR_DEFINED)
        # Install into bin dir
        install(TARGETS ${execName} RUNTIME DESTINATION bin)
    endif()
endforeach(fileName)


if (INSTALL_DIR_DEFINED)
    install(TARGETS ejfat_simple LIBRARY DESTINATION lib)
endif()

# Only install if installation directory has been defined
if (INSTALL_DIR_DEFINED)
    install(FILES ${HEADER_FILES} DESTINATION include)
    install(FILES ${HEADER_FILES} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
