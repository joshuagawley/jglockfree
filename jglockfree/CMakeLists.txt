include(GNUInstallDirs)

add_library(jglockfree INTERFACE)

target_sources(jglockfree INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/queue.h ${CMAKE_CURRENT_SOURCE_DIR}/hazard_pointer.h)

target_include_directories(jglockfree INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(jglockfree INTERFACE project_settings)

install(FILES queue.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/jglockfree)
