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

  set(DMN_BLUETOOTH_COSMIC_EXECUTABLE
      "${CMAKE_CURRENT_SOURCE_DIR}/cosmic/target/release/dmn-bluetooth-cosmic"
      CACHE FILEPATH
      "Cargo-built COSMIC Bluetooth executable to install"
  )

  # The Cargo-built executable has an $ORIGIN runpath fallback, so colocate
  # the C ABI library with it for relocatable native installations.
  install(TARGETS dmn-bluetooth-cabi
            LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
  )

  install(PROGRAMS "${DMN_BLUETOOTH_COSMIC_EXECUTABLE}"
          DESTINATION "${CMAKE_INSTALL_BINDIR}"
          COMPONENT Runtime
  )

  set(DMN_BLUETOOTH_COSMIC_DATA_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/cosmic/data"
  )

  install(FILES
            "${DMN_BLUETOOTH_COSMIC_DATA_DIR}/io.github.cheebinhoh.dmnbluetoothcosmic.desktop"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
          COMPONENT Runtime
  )

  install(FILES
            "${DMN_BLUETOOTH_COSMIC_DATA_DIR}/io.github.cheebinhoh.dmnbluetoothcosmic.metainfo.xml"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo"
          COMPONENT Runtime
  )

  install(FILES
            "${DMN_BLUETOOTH_COSMIC_DATA_DIR}/icons/hicolor/scalable/apps/io.github.cheebinhoh.dmnbluetoothcosmic.svg"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
          COMPONENT Runtime
  )

  install(FILES
            "${DMN_BLUETOOTH_COSMIC_DATA_DIR}/icons/hicolor/128x128/apps/io.github.cheebinhoh.dmnbluetoothcosmic.png"
          DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps"
          COMPONENT Runtime
  )
endif()
