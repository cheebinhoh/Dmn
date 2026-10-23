# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

# Executable entry points belong here; reusable implementation targets are
# defined by the parent src/CMakeLists.txt before this directory is added.
message(STATUS "build dmn applications...")

add_executable(dmn-dmesg-daemon
                 dmn-dmesg-daemon.cpp
)

target_compile_definitions(dmn-dmesg-daemon
                             PRIVATE DMN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(dmn-dmesg-daemon
                             PRIVATE ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(dmn-dmesg-daemon
                        PRIVATE dmn
)

add_executable(dmn-kafka-receiver
                 dmn-kafka-receiver.cpp
)

target_compile_definitions(dmn-kafka-receiver
                             PRIVATE DMN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(dmn-kafka-receiver
                             PRIVATE
                             ${PROJECT_SOURCE_DIR}/include
                             dmn
)

target_link_libraries(dmn-kafka-receiver
                        PRIVATE dmn
)

add_executable(dmn-kafka-sender
                 dmn-kafka-sender.cpp
)

target_compile_definitions(dmn-kafka-sender
                             PRIVATE DMN_VERSION="${PROJECT_VERSION}"
)

target_include_directories(dmn-kafka-sender
                             PRIVATE
                             ${PROJECT_SOURCE_DIR}/include
                             dmn
)

target_link_libraries(dmn-kafka-sender
                        PRIVATE dmn
)

if (ENABLE_APP_BLUEZ_DISCOVERY)
  add_executable(dmn-bluez-cli
                   dmn-bluez-cli.cpp
  )

  target_compile_options(dmn-bluez-cli
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_compile_definitions(dmn-bluez-cli
                               PRIVATE DMN_VERSION="${PROJECT_VERSION}"
  )

  target_link_libraries(dmn-bluez-cli PRIVATE dmn-bluez)

  install(TARGETS dmn-bluez-cli
            RUNTIME DESTINATION bin
            COMPONENT Runtime
  )

  install(FILES ${PROJECT_SOURCE_DIR}/docs/man/dmn-bluez-cli.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
            COMPONENT Runtime
  )
endif()

if (ENABLE_APP_BLUETOOTH)
  add_executable(dmn-bluetooth-cli
                   dmn-bluetooth-cli.cpp
  )

  target_compile_options(dmn-bluetooth-cli
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_compile_definitions(dmn-bluetooth-cli
                               PRIVATE DMN_VERSION="${PROJECT_VERSION}"
  )

  target_link_libraries(dmn-bluetooth-cli
                          PRIVATE dmn-ancs-bluez-monitor
  )

  install(TARGETS dmn-bluetooth-cli
            RUNTIME DESTINATION bin
            COMPONENT Runtime
  )

  install(FILES ${PROJECT_SOURCE_DIR}/docs/man/dmn-bluetooth-cli.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
            COMPONENT Runtime
  )
endif()

if (ENABLE_APP_BLUETOOTH_GTK)
  add_subdirectory(gui)
endif()
