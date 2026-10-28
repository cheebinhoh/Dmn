# Copyright © 2026 Chee Bin HOH. All rights reserved.

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
                          PRIVATE dmn-bluetooth-engine
  )

  install(TARGETS dmn-bluetooth-cabi
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT Runtime
  )
endif()
