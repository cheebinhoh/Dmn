add_library(dmn-bluetooth-gui-core
             core/device-monitor-event-store.cpp
             core/device-selection-model.cpp
)

target_include_directories(dmn-bluetooth-gui-core
                             PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core
)

target_compile_options(dmn-bluetooth-gui-core
                         PRIVATE -Wall -Wextra -Wpedantic
)

target_link_libraries(dmn-bluetooth-gui-core
                        PUBLIC dmn-ancs
                        PRIVATE PkgConfig::GLIB2
)

add_subdirectory(gtk)
