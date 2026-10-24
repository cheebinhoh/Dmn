# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

# [TEST TYPE: GTEST]
# Core unit and component tests (CTest label: dmn).
ADD_TEST_EXECUTABLE(dmn
                      # FIXME: refactor this test so it does not run indefinitely
                      # dmn-test
                      dmn-test-async
                      dmn-test-async-2
                      dmn-test-async-2-queue-lf
                      dmn-test-async-queue-lf
                      dmn-test-blockingqueue
                      dmn-test-blockingqueue-2
                      dmn-test-blockingqueue-3
                      dmn-test-blockingqueue-4
                      dmn-test-blockingqueue-5
                      dmn-test-blockingqueue-6
                      dmn-test-blockingqueue-lf-1
                      dmn-test-blockingqueue-lf-2
                      dmn-test-blockingqueue-lf-3
                      dmn-test-blockingqueue-lf-4
                      dmn-test-blockingqueue-lf-5
                      dmn-test-blockingqueue-lf-6
                      dmn-test-blockingqueue-lf-7
                      dmn-test-blockingqueue-lf-8
                      dmn-test-blockingqueue-lf-stress
                      dmn-test-blockingqueue-lf-wait-empty
                      dmn-test-blockingqueue-perf-1
                      dmn-test-blockingqueue-perf-2
                      dmn-test-blockingqueue-shutdown
                      dmn-test-inflight-guard
                      dmn-test-interval-btree
                      dmn-test-io
                      dmn-test-io-contract
                      dmn-test-pipe
                      dmn-test-pipe-2
                      dmn-test-pipe-3
                      dmn-test-pipe-4
                      dmn-test-pipe-queue-lf
                      dmn-test-pipe-queue-lf-2
                      dmn-test-pipe-queue-lf-3
                      dmn-test-pipe-queue-lf-4
                      dmn-test-pipe-scheduled
                      dmn-test-proc
                      dmn-test-pub-sub
                      dmn-test-pub-sub-2
                      dmn-test-pub-sub-2-queue-lf
                      dmn-test-pub-sub-errors
                      dmn-test-pub-sub-queue-lf
                      dmn-test-runtime
                      dmn-test-runtime-2
                      dmn-test-runtime-3
                      dmn-test-runtime-queue-lf
                      dmn-test-runtime-queue-lf-2
                      dmn-test-runtime-queue-lf-3
                      dmn-test-runtime-state
                      dmn-test-singleton
                      dmn-test-socket
                      dmn-test-state
                      dmn-test-timer
                      dmn-test-umbrella
                      dmn-test-util
)

if (ENABLE_DMN_DMESGNET)
  ADD_TEST_EXECUTABLE(dmn
                        dmn-test-dlock-1
                        dmn-test-dlock-2
                        dmn-test-dlock-3
                        dmn-test-dlock-4
                        dmn-test-dlock-5
                        dmn-test-dlock-dmesg-seam
                        dmn-test-dmesg-1
                        dmn-test-dmesg-10
                        dmn-test-dmesg-11
                        dmn-test-dmesg-2
                        dmn-test-dmesg-3
                        # FIXME: the following test is not deterministic
                        # dmn-test-dmesg-4
                        dmn-test-dmesg-5
                        dmn-test-dmesg-6
                        dmn-test-dmesg-7
                        dmn-test-dmesg-8
                        dmn-test-dmesg-9
                        dmn-test-dmesg-conflict
                        dmn-test-dmesg-pb
                        dmn-test-dmesg-pb-util
                        dmn-test-dmesgnet-1
                        dmn-test-dmesgnet-2
                        dmn-test-dmesgnet-3
                        dmn-test-dmesgnet-4
                        dmn-test-dmesgnet-5
                        dmn-test-dmesgnet-6
                        dmn-test-dmesgnet-shutdown
                        dmn-test-kafka-config
  )
endif()

function(add_dmn_cli_version_test target)
  add_test(NAME "${target}-version"
             COMMAND "${target}" --version
  )

  set_tests_properties("${target}-version"
                         PROPERTIES
                         LABELS "dmn-cli"
                         PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}"
  )
endfunction()

add_dmn_cli_version_test(dmn-dmesg-daemon)
if (ENABLE_DMN_DMESGNET)
  add_dmn_cli_version_test(dmn-kafka-receiver)
  add_dmn_cli_version_test(dmn-kafka-sender)
endif()

if (ENABLE_ANCS)
  ADD_TEST_EXECUTABLE(dmn
                        dmn-test-ancs-client
                        dmn-test-ancs-parser
  )

  target_link_libraries(dmn-test-ancs-client PRIVATE dmn-ancs)
  target_link_libraries(dmn-test-ancs-parser PRIVATE dmn-ancs)
endif()

if (ENABLE_BLUEZ)
  target_link_libraries(dmn-test-umbrella PRIVATE dmn-bluez)
endif()

if (ENABLE_DBUS)
  target_link_libraries(dmn-test-umbrella PRIVATE dmn-dbus)
endif()

if (ENABLE_DBUS)
  # Keep the D-Bus library suite enabled for every configuration that enables
  # this layer, including app configurations that imply ENABLE_DBUS.
  # [TEST TYPE: GTEST - DBUS]
  find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon REQUIRED)

  ADD_TEST_EXECUTABLE(dmn
                        dmn-test-dbus-client
                        dmn-test-dbus-client-session
  )

  foreach(test_target IN ITEMS
          dmn-test-dbus-client
          dmn-test-dbus-client-session)
    target_compile_definitions(${test_target}
                                 PRIVATE
                                 DMN_DBUS_DAEMON_EXECUTABLE="${DBUS_DAEMON_EXECUTABLE}"
    )

    target_link_libraries(${test_target}
                            PRIVATE dmn-dbus PkgConfig::DBUS
    )
  endforeach()

  if (ENABLE_DMN_DMESGNET)
    ADD_TEST_EXECUTABLE(dmn
                        dmn-test-dbus-facade
                        dmn-test-dbus-io
    )

    foreach(test_target IN ITEMS dmn-test-dbus-facade dmn-test-dbus-io)
      target_compile_definitions(${test_target}
                                   PRIVATE
                                   DMN_DBUS_DAEMON_EXECUTABLE="${DBUS_DAEMON_EXECUTABLE}"
      )

      target_link_libraries(${test_target}
                              PRIVATE dmn-dbus PkgConfig::DBUS
      )
    endforeach()
  endif()
endif()

if (ENABLE_BLUEZ)
  find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon REQUIRED)

  add_executable(dmn-bluez-cli-test
                   ${PROJECT_SOURCE_DIR}/src/app/dmn-bluez-cli.cpp
  )

  target_compile_definitions(dmn-bluez-cli-test
                               PRIVATE DMN_BLUEZ_TESTING=1
                                       DMN_VERSION="${PROJECT_VERSION}"
                                       DMN_BLUEZ_TEST_OPERATION_TIMEOUT_MS=500
  )

  target_compile_options(dmn-bluez-cli-test
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluez-cli-test PRIVATE dmn-bluez)

  ADD_TEST_EXECUTABLE(dmn
                        dmn-test-bluez-device-list
                        dmn-test-bluez-gatt-client
  )

  foreach(test_target IN ITEMS
          dmn-test-bluez-device-list
          dmn-test-bluez-gatt-client)
    target_compile_definitions(${test_target}
                                 PRIVATE
                                 DMN_DBUS_DAEMON_EXECUTABLE="${DBUS_DAEMON_EXECUTABLE}"
    )

    target_link_libraries(${test_target}
                          PRIVATE dmn-bluez PkgConfig::DBUS
    )
  endforeach()

  target_compile_definitions(dmn-test-bluez-device-list
                               PRIVATE
                               DMN_BLUEZ_CLI_EXECUTABLE="$<TARGET_FILE:dmn-bluez-cli-test>"
  )

  add_dependencies(dmn-test-bluez-device-list dmn-bluez-cli-test)
endif()

if (ENABLE_ANCS AND ENABLE_BLUEZ)
  add_executable(dmn-bluetooth-cli-test
                   ${PROJECT_SOURCE_DIR}/src/app/dmn-bluetooth-cli.cpp
  )

  target_compile_options(dmn-bluetooth-cli-test
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluetooth-cli-test
                        PRIVATE dmn-ancs-bluez-monitor
  )

  target_compile_definitions(dmn-bluetooth-cli-test
                               PRIVATE
                               DMN_BLUETOOTH_TESTING=1
                               DMN_VERSION="${PROJECT_VERSION}"
  )

  target_link_libraries(dmn-test-bluez-device-list
                        PRIVATE dmn-ancs-bluez-monitor
  )

  target_compile_definitions(dmn-test-bluez-device-list
                               PRIVATE
                               DMN_BLUETOOTH_CLI_EXECUTABLE="$<TARGET_FILE:dmn-bluetooth-cli-test>"
  )

  add_dependencies(dmn-test-bluez-device-list dmn-bluetooth-cli-test)

  add_test(NAME dmn-bluetooth-cli-monitor-private-bus
             COMMAND dmn-test-bluez-device-list
                     --gtest_filter=DmnBluetoothMonitorCli.*:DmnAncsBluezMonitorTest.*
  )

  set_tests_properties(dmn-bluetooth-cli-monitor-private-bus
                         PROPERTIES LABELS "dmn"
  )

endif()

# Production application smoke/CLI tests are gated by their app options,
# separately from the supporting library and integration-test blocks above.
if (ENABLE_APP_BLUEZ_DISCOVERY_CLI)
  add_dmn_cli_version_test(dmn-bluez-cli)

  add_test(NAME dmn-bluez-cli-help
             COMMAND dmn-bluez-cli --help
  )

  set_tests_properties(dmn-bluez-cli-help PROPERTIES LABELS "dmn")

  add_test(NAME dmn-bluez-cli-rejects-duplicate-option
             COMMAND dmn-bluez-cli --discovery --discovery
  )

  set_tests_properties(dmn-bluez-cli-rejects-duplicate-option
                         PROPERTIES LABELS "dmn" WILL_FAIL TRUE
  )
endif()

if (ENABLE_APP_BLUETOOTH_CLI)
  add_dmn_cli_version_test(dmn-bluetooth-cli)

  add_test(NAME dmn-bluetooth-cli-help
             COMMAND dmn-bluetooth-cli --help
  )

  add_test(NAME dmn-bluetooth-cli-monitor-help
             COMMAND dmn-bluetooth-cli monitor --help
  )

  add_test(NAME dmn-bluetooth-cli-monitor-rejects-missing-device
             COMMAND dmn-bluetooth-cli monitor
  )

  add_test(NAME dmn-bluetooth-cli-monitor-rejects-invalid-timeout
             COMMAND dmn-bluetooth-cli monitor "iPhone Old"
                     --discovery-timeout 0
  )

  set_tests_properties(dmn-bluetooth-cli-help
                       dmn-bluetooth-cli-monitor-help PROPERTIES LABELS "dmn")
  set_tests_properties(dmn-bluetooth-cli-monitor-rejects-missing-device
                       dmn-bluetooth-cli-monitor-rejects-invalid-timeout
                         PROPERTIES LABELS "dmn" WILL_FAIL TRUE
  )
endif()

if (ENABLE_APP_BLUETOOTH_GTK)
  ADD_TEST_EXECUTABLE(dmn-gui dmn-test-bluetooth-gui-selection)

  target_link_libraries(dmn-test-bluetooth-gui-selection
                          PRIVATE dmn-bluetooth-gui-core
  )

  add_test(NAME dmn-bluetooth-gtk-help
             COMMAND dmn-bluetooth-gtk --help
  )

  add_test(NAME dmn-bluetooth-gtk-version
             COMMAND dmn-bluetooth-gtk --version
  )

  set_tests_properties(dmn-bluetooth-gtk-help
                         PROPERTIES LABELS "dmn-gui"
  )

  set_tests_properties(dmn-bluetooth-gtk-version
                         PROPERTIES
                         LABELS "dmn-gui"
                         PASS_REGULAR_EXPRESSION "${PROJECT_VERSION}"
  )

  find_program(DESKTOP_FILE_VALIDATE_EXECUTABLE
               NAMES desktop-file-validate
  )

  if (DESKTOP_FILE_VALIDATE_EXECUTABLE)
    add_test(NAME dmn-bluetooth-gtk-desktop-entry
               COMMAND "${DESKTOP_FILE_VALIDATE_EXECUTABLE}"
                       "${CMAKE_BINARY_DIR}/src/app/gui/gtk/${DMN_BLUETOOTH_GTK_APP_ID}.desktop"
    )

    set_tests_properties(dmn-bluetooth-gtk-desktop-entry
                           PROPERTIES LABELS "dmn-gui"
    )
  endif()

  find_program(APPSTREAMCLI_EXECUTABLE NAMES appstreamcli)
  if (APPSTREAMCLI_EXECUTABLE)
    add_test(NAME dmn-bluetooth-gtk-appstream
               COMMAND "${APPSTREAMCLI_EXECUTABLE}" validate
                       "${CMAKE_BINARY_DIR}/src/app/gui/gtk/${DMN_BLUETOOTH_GTK_APP_ID}.metainfo.xml"
    )

    set_tests_properties(dmn-bluetooth-gtk-appstream
                           PROPERTIES LABELS "dmn-gui"
    )
  endif()
endif()

if (ENABLE_ANCS_HARDWARE_TESTS)
  target_compile_definitions(dmn-test-bluez-device-list
                              PRIVATE
                              DMN_ANCS_HARDWARE_TESTS=1
                              DMN_ANCS_HARDWARE_DEVICE="${DMN_ANCS_HARDWARE_DEVICE}"
                              DMN_BLUETOOTH_HARDWARE_CLI_EXECUTABLE="$<TARGET_FILE:dmn-bluetooth-cli>"
  )

  add_test(NAME dmn-ancs-iphone-hardware
             COMMAND dmn-test-bluez-device-list
                     --gtest_filter=DmnBluetoothMonitorHardware.*
  )

  set_tests_properties(dmn-ancs-iphone-hardware
                        PROPERTIES LABELS "hardware"
                                   TIMEOUT 120
  )
endif()

# Keep fault-injection targets out of builds that do not enable libfiu.
if (ENABLE_FAULT_INJECTION)
  # [TEST TYPE: FAULT-INJECTION]
  # These executables run through fiu-run and use the CTest label
  # "fault-injection"; each target enables its own configured failure point.
  ADD_TEST_FAULT_INJECTION_EXECUTABLE(dmn-test-fi-proc-thread-start-failure
                                        dmn/timer/pipe/proc/pthread_create
  )

  ADD_TEST_FAULT_INJECTION_EXECUTABLE(dmn-test-fi-timer-thread-start-failure
                                        dmn/timer/pipe/proc/pthread_create
  )

  ADD_TEST_FAULT_INJECTION_EXECUTABLE(dmn-test-fi-timer-reschedule-failure
                                        dmn/timer/reschedule/write_at
  )

  if (ENABLE_DBUS)
    ADD_TEST_FAULT_INJECTION_EXECUTABLE(
      dmn-test-fi-dbus-worker-start-failure
      dmn/timer/pipe/proc/pthread_create
    )

    ADD_TEST_FAULT_INJECTION_EXECUTABLE(
      dmn-test-fi-dbus-output-worker-start-failure
      dmn/dbus/output/worker_start
    )

    ADD_TEST_FAULT_INJECTION_EXECUTABLE(
      dmn-test-fi-dbus-input-allocation
      dmn/dbus/input/payload_allocation
    )

    ADD_TEST_FAULT_INJECTION_EXECUTABLE(
      dmn-test-fi-dbus-output-stall
      dmn/dbus/output/send_stall
    )

    set(DMN_DBUS_FAULT_TEST_TARGETS
            dmn-test-fi-dbus-input-allocation
            dmn-test-fi-dbus-output-stall
            dmn-test-fi-dbus-output-worker-start-failure
            dmn-test-fi-dbus-worker-start-failure
    )

    if (ENABLE_DMN_DMESGNET)
      ADD_TEST_FAULT_INJECTION_EXECUTABLE(
        dmn-test-fi-dbus-facade-rollback
        dmn/dbus/facade/output_endpoint_creation
      )
      list(APPEND DMN_DBUS_FAULT_TEST_TARGETS
             dmn-test-fi-dbus-facade-rollback
      )
    endif()

    foreach(test_target IN LISTS DMN_DBUS_FAULT_TEST_TARGETS)
      target_compile_definitions(${test_target}
                                   PRIVATE
                                   DMN_DBUS_DAEMON_EXECUTABLE="${DBUS_DAEMON_EXECUTABLE}"
      )

      target_link_libraries(${test_target}
                              PRIVATE
                              dmn-dbus
                              PkgConfig::DBUS
      )
    endforeach()

    # fiu-run preloads libfiu into the test. The test starts a private
    # dbus-daemon subprocess, which inherits that preload and needs to locate
    # libfiu too; add its library directory without discarding the caller's
    # existing loader path.
    if (APPLE)
      set(fiu_loader_path DYLD_LIBRARY_PATH)
      set(fiu_existing_loader_path "$ENV{DYLD_LIBRARY_PATH}")
    else()
      set(fiu_loader_path LD_LIBRARY_PATH)
      set(fiu_existing_loader_path "$ENV{LD_LIBRARY_PATH}")
    endif()

    list(JOIN DMN_LIBFIU_LIBRARY_DIR ":" fiu_library_path)

    set_tests_properties(
      ${DMN_DBUS_FAULT_TEST_TARGETS}
      PROPERTIES ENVIRONMENT
                 "${fiu_loader_path}=${fiu_library_path}:${fiu_existing_loader_path}"
    )
  endif()
endif()

if (BUILD_KAFKA_TEST AND ENABLE_DMN_DMESGNET)
  # [TEST TYPE: GTEST - KAFKA]
  # Kafka integration tests (CTest label: kafka).
  ADD_TEST_EXECUTABLE(kafka
                        dmn-test-dmesgnet-with-kafka-1
                        dmn-test-dmesgnet-with-kafka-2
                        dmn-test-dmesgnet-with-kafka-3
                        dmn-test-dmesgnet-with-kafka-4
                        dmn-test-dmesgnet-with-kafka-5
                        dmn-test-kafka
                        dmn-test-kafka-protobuf
  )
endif()

# [TEST TYPE: STANDALONE-HEADER]
# Compile each public header independently to check self-contained includes.
file(GLOB DMN_PUBLIC_HEADERS CONFIGURE_DEPENDS
       "${PROJECT_SOURCE_DIR}/include/*.hpp"
       "${PROJECT_SOURCE_DIR}/include/app/gui/core/*.hpp"
       "${PROJECT_SOURCE_DIR}/include/kafka/*.hpp"
)

set(DMN_STANDALONE_HEADER_SOURCES)

foreach(header IN LISTS DMN_PUBLIC_HEADERS)
  if (NOT ENABLE_DMN_DMESGNET AND
      header MATCHES "/(dmn-(dlock|dmesg)[^/]*|kafka)/?")
    continue()
  endif()

  file(RELATIVE_PATH header_relative "${PROJECT_SOURCE_DIR}/include" "${header}")
  string(MAKE_C_IDENTIFIER "${header_relative}" header_id)

  set(header_source
        "${CMAKE_CURRENT_BINARY_DIR}/standalone-headers/${header_id}.cpp"
  )

  file(WRITE "${header_source}" "#include \"${header}\"\n")
  list(APPEND DMN_STANDALONE_HEADER_SOURCES "${header_source}")
endforeach()

add_library(dmn-standalone-header-check OBJECT
              ${DMN_STANDALONE_HEADER_SOURCES}
)

target_link_libraries(dmn-standalone-header-check PRIVATE dmn
)

target_include_directories(dmn-standalone-header-check
                             PRIVATE ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(dmn-standalone-header-check PRIVATE
                         -Wall -Wextra -Wpedantic
)

add_test(NAME dmn-standalone-header-check
           COMMAND ${CMAKE_COMMAND} --build "${PROJECT_BINARY_DIR}"
           --target dmn-standalone-header-check
)

set_tests_properties(dmn-standalone-header-check PROPERTIES LABELS "dmn")

if (ENABLE_VALGRIND)
  # [TEST TYPE: VALGRIND]
  # Run the selected regular GTest executables under Valgrind.
  SET(DMN_VALGRIND_TESTS
        dmn-test-async
        dmn-test-async-2
        dmn-test-async-2-queue-lf
        dmn-test-async-queue-lf
        dmn-test-blockingqueue
        dmn-test-blockingqueue-2
        dmn-test-blockingqueue-3
        dmn-test-blockingqueue-4
        dmn-test-blockingqueue-5
        dmn-test-blockingqueue-6
        dmn-test-blockingqueue-lf-1
        dmn-test-blockingqueue-lf-2
        dmn-test-blockingqueue-lf-3
        dmn-test-blockingqueue-lf-4
        dmn-test-blockingqueue-lf-5
        dmn-test-blockingqueue-lf-6
        dmn-test-blockingqueue-lf-7
        dmn-test-blockingqueue-lf-8
        dmn-test-blockingqueue-lf-stress
        dmn-test-blockingqueue-lf-wait-empty
        dmn-test-blockingqueue-perf-1
        dmn-test-blockingqueue-perf-2
        dmn-test-blockingqueue-shutdown
        dmn-test-inflight-guard
        dmn-test-interval-btree
        dmn-test-io
        dmn-test-pipe
        dmn-test-pipe-2
        dmn-test-pipe-3
        dmn-test-pipe-4
        dmn-test-pipe-queue-lf
        dmn-test-pipe-queue-lf-2
        dmn-test-pipe-queue-lf-3
        dmn-test-pipe-queue-lf-4
        dmn-test-pipe-scheduled
        dmn-test-proc
        dmn-test-pub-sub
        dmn-test-pub-sub-2
        dmn-test-pub-sub-2-queue-lf
        dmn-test-pub-sub-errors
        dmn-test-pub-sub-queue-lf
        dmn-test-runtime
        dmn-test-runtime-2
        dmn-test-runtime-3
        dmn-test-runtime-queue-lf
        dmn-test-runtime-queue-lf-2
        dmn-test-runtime-queue-lf-3
        dmn-test-runtime-state
        dmn-test-singleton
        dmn-test-socket
        dmn-test-state
        dmn-test-timer
        dmn-test-umbrella
        dmn-test-util
  )

  if (ENABLE_DMN_DMESGNET)
    list(APPEND DMN_VALGRIND_TESTS
           dmn-test-dlock-1
           dmn-test-dlock-2
           dmn-test-dlock-3
           dmn-test-dlock-4
           dmn-test-dlock-5
           dmn-test-dlock-dmesg-seam
           dmn-test-dmesg-1
           dmn-test-dmesg-10
           dmn-test-dmesg-11
           dmn-test-dmesg-2
           dmn-test-dmesg-3
           dmn-test-dmesg-5
           dmn-test-dmesg-6
           dmn-test-dmesg-7
           dmn-test-dmesg-8
           dmn-test-dmesg-9
           dmn-test-dmesg-pb
           dmn-test-dmesg-pb-util
           dmn-test-dmesgnet-1
           dmn-test-dmesgnet-2
           dmn-test-dmesgnet-3
           dmn-test-dmesgnet-4
           dmn-test-dmesgnet-5
           dmn-test-dmesgnet-6
           dmn-test-dmesgnet-shutdown
    )
  endif()

  ADD_TEST_VALGRIND(dmn
                      ${DMN_VALGRIND_TESTS}
  )

  add_custom_target(valgrind-check
                      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L valgrind
                      DEPENDS ${DMN_VALGRIND_TESTS}
                      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()
