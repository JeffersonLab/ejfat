
set(LEGACY_SRC
        ejfat_packetize_engine
        ejfat_assemble_engine
        ejfat_assemble_et_engine
)

add_library(ersap_ejfat SHARED ${LEGACY_SRC})
target_link_libraries(ersap_ejfat PUBLIC ${ZeroMQ_LIBRARIES} ${ET_LIBRARY})
target_include_directories(ersap_ejfat PUBLIC ${ZeroMQ_INCLUDE_DIRS} ${ET_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/simulation
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

install(TARGETS ersap_ejfat DESTINATION ${CMAKE_INSTALL_PLUGINDIR}/ejfat/lib)
