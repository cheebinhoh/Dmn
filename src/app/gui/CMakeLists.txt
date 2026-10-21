add_library(dmn-bluetooth-gui-core
             core/desktop-alert-policy.cpp
             core/device-monitor-controller.cpp
             core/device-monitor-event-store.cpp
             core/device-selection-model.cpp
)

target_sources(dmn-bluetooth-gui-core
               PUBLIC
               ${PROJECT_SOURCE_DIR}/include/app/gui/core/desktop-alert-policy.hpp
               ${PROJECT_SOURCE_DIR}/include/app/gui/core/device-monitor-controller.hpp
               ${PROJECT_SOURCE_DIR}/include/app/gui/core/device-monitor-event-store.hpp
               ${PROJECT_SOURCE_DIR}/include/app/gui/core/device-selection-model.hpp
)

target_include_directories(dmn-bluetooth-gui-core
                             PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(dmn-bluetooth-gui-core
                         PRIVATE -Wall -Wextra -Wpedantic
)

target_link_libraries(dmn-bluetooth-gui-core
                        PUBLIC dmn-ancs
                        PRIVATE PkgConfig::GLIB2
)

add_subdirectory(gtk)
