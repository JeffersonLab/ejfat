
set(SIMPLE_SRC
        EjfatConsumer.cpp
        EjfatProducer.cpp
   )

set(EXECUTABLE_FILES
#        lbtest.cpp
        lbmonitor.cpp
        lbfree.cpp
        lbreserve.cpp
        simpleSender.cpp
   )

add_library(ejfat_simple SHARED ${SIMPLE_SRC})
#target_link_libraries(ejfat_simple PUBLIC  ${ET_LIBRARY})
#target_include_directories(ejfat_simple PUBLIC${ET_INCLUDE_DIRS}
#        ${CMAKE_SOURCE_DIR}/simulation
#        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
#        )
target_link_libraries(ejfat_simple PUBLIC pthread ${Boost_LIBRARIES} ${GRPC_LIBRARIES})

target_include_directories(ejfat_simple PUBLIC
        ${Boost_INCLUDE_DIRS}
        ${GRPC_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/simulation
        )

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
    target_include_directories(${execName} PUBLIC ${CMAKE_SOURCE_DIR}/simulation
            ${GRPC_INCLUDE_DIRS}
            ${Boost_INCLUDE_DIRS})

    # Needs these libs
    target_link_libraries(${execName} pthread ${Boost_LIBRARIES} ${GRPC_LIBRARIES} ejfat_simple)

    # Only install if installation directory has been defined
    if (DEFINED INSTALL_DIR_DEFINED)
        # Install into bin dir
        install(TARGETS ${execName} RUNTIME DESTINATION bin)
    endif()
endforeach(fileName)


if (DEFINED INSTALL_DIR_DEFINED)
    install(TARGETS ejfat_simple LIBRARY DESTINATION lib)
endif()
