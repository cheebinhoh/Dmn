# Copyright © 2026 Chee Bin HOH. All rights reserved.

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

if (ENABLE_APP_BLUETOOTH_COSMIC OR ENABLE_APP_BLUETOOTH_GTK)
  # Static engine (GUI-independent orchestration over the Dmn Bluetooth layers)
  add_library(dmn-bluetooth-gui-engine STATIC
                engine/dmn-bluez-backend.cpp
                engine/dmn-demo-backend.cpp
                engine/dmn-engine.cpp
                engine/dmn-json.cpp
  )

  target_include_directories(dmn-bluetooth-gui-engine
                               PUBLIC ${PROJECT_SOURCE_DIR}/include
  )

  target_compile_options(dmn-bluetooth-gui-engine
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluetooth-gui-engine
                          PUBLIC dmn-ancs-bluez-monitor dmn-bluetooth-gui-core
  )
endif()

if (ENABLE_APP_BLUETOOTH_GTK)
  add_subdirectory(gtk)
endif()

if (ENABLE_APP_BLUETOOTH_COSMIC)
  # Shared C ABI consumed by the Rust frontend, which Cargo builds through
  # dmn-build.rs; CMake never invokes Cargo.
  add_library(dmn-bluetooth-cabi SHARED cabi/dmn-bluetooth-cabi.cpp)

  target_include_directories(dmn-bluetooth-cabi
                               PUBLIC ${PROJECT_SOURCE_DIR}/include
  )

  target_compile_options(dmn-bluetooth-cabi
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluetooth-cabi
                          PRIVATE dmn-bluetooth-gui-engine
  )

  install(TARGETS dmn-bluetooth-cabi
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT Runtime
  )
endif()
