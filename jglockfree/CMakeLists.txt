include(GNUInstallDirs)

add_library(jglockfree INTERFACE)

target_sources(jglockfree
    INTERFACE FILE_SET HEADERS
    BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/..
    FILES
        config.h
        hazard_pointer.h
        queue.h
        spsc.h
)

target_link_libraries(jglockfree INTERFACE project_settings)

install(FILES queue.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/jglockfree)
