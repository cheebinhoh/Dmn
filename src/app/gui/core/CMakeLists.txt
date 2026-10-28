# Copyright © 2026 Chee Bin HOH. All rights reserved.

# Frontend-independent device selection and monitor policy. No GLib/GUI
# dependency, so the engine and the CLI can use it.
add_library(dmn-bluetooth-device-core
             dmn-device-monitor-controller.cpp
             dmn-device-selection-model.cpp
)

target_sources(dmn-bluetooth-device-core
               PUBLIC
               ${PROJECT_SOURCE_DIR}/include/app/gui/core/dmn-device-monitor-controller.hpp
               ${PROJECT_SOURCE_DIR}/include/app/gui/core/dmn-device-selection-model.hpp
)

target_include_directories(dmn-bluetooth-device-core
                             PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(dmn-bluetooth-device-core
                         PRIVATE -Wall -Wextra -Wpedantic
)

target_link_libraries(dmn-bluetooth-device-core
                        PUBLIC dmn-ancs
)

# GUI presentation helpers that use GLib (UTF-8 case folding for search).
if (ENABLE_APP_BLUETOOTH_COSMIC OR ENABLE_APP_BLUETOOTH_GTK)
  add_library(dmn-bluetooth-gui-core
               dmn-desktop-alert-policy.cpp
               dmn-device-monitor-event-store.cpp
  )

  target_sources(dmn-bluetooth-gui-core
                 PUBLIC
                 ${PROJECT_SOURCE_DIR}/include/app/gui/core/dmn-desktop-alert-policy.hpp
                 ${PROJECT_SOURCE_DIR}/include/app/gui/core/dmn-device-monitor-event-store.hpp
  )

  target_include_directories(dmn-bluetooth-gui-core
                               PUBLIC ${PROJECT_SOURCE_DIR}/include
  )

  target_compile_options(dmn-bluetooth-gui-core
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluetooth-gui-core
                          PUBLIC dmn-bluetooth-device-core
                          PRIVATE PkgConfig::GLIB2
  )
endif()
