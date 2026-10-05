# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

ADD_TEST_EXECUTABLE(dmn
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
                      dmn-test-dlock-1
                      dmn-test-dlock-2
                      dmn-test-dlock-3
                      dmn-test-dlock-4
                      dmn-test-dlock-5
                      dmn-test-dlock-dmesg-seam
                      dmn-test-dmesg-1
                      dmn-test-dmesg-2
                      dmn-test-dmesg-3
                      # FIXME: the following test is not deterministic
                      # dmn-test-dmesg-4
                      dmn-test-dmesg-5
                      dmn-test-dmesg-6
                      dmn-test-dmesg-7
                      dmn-test-dmesg-8
                      dmn-test-dmesg-9
                      dmn-test-dmesg-10
                      dmn-test-dmesg-11
                      dmn-test-dmesg-pb
                      dmn-test-dmesg-pb-util
                      dmn-test-dmesgnet-1
                      dmn-test-dmesgnet-2
                      dmn-test-dmesgnet-3
                      dmn-test-dmesgnet-4
                      dmn-test-dmesgnet-5
                      dmn-test-dmesgnet-6
                      #dmn-test-dmesgnet-7
                      #dmn-test-dmesgnet-8
                      #dmn-test-dmesgnet-9
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
                      # FIXME: the following test needs to be refactored to avoid none-stop running
                      # dmn-test
)

if (ENABLE_DBUS)
  find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon REQUIRED)

  ADD_TEST_EXECUTABLE(dmn
                        dmn-test-dbus-io
  )
  target_compile_definitions(dmn-test-dbus-io
                               PRIVATE
                               DMN_DBUS_DAEMON_EXECUTABLE="${DBUS_DAEMON_EXECUTABLE}"
  )
  target_link_libraries(dmn-test-dbus-io
                          PRIVATE dmn-dbus PkgConfig::DBUS
  )
endif()

# Keep fault-injection targets out of builds that do not enable libfiu.
if (ENABLE_FAULT_INJECTION)
  ADD_TEST_FAULT_INJECTION_EXECUTABLE(dmn-test-fi-timer-thread-start-failure
                                        dmn/timer/pipe/proc/pthread_create
  )
  ADD_TEST_FAULT_INJECTION_EXECUTABLE(dmn-test-fi-timer-reschedule-failure
                                        dmn/timer/reschedule/write_at
  )
endif()

if (BUILD_KAFKA_TEST)
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

file(GLOB DMN_PUBLIC_HEADERS CONFIGURE_DEPENDS
       "${PROJECT_SOURCE_DIR}/include/*.hpp"
       "${PROJECT_SOURCE_DIR}/include/kafka/*.hpp"
)

set(DMN_STANDALONE_HEADER_SOURCES)

foreach(header IN LISTS DMN_PUBLIC_HEADERS)
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
        dmn-test-dlock-1
        dmn-test-dlock-2
        dmn-test-dlock-3
        dmn-test-dlock-4
        dmn-test-dlock-5
        dmn-test-dlock-dmesg-seam
        dmn-test-dmesg-1
        dmn-test-dmesg-2
        dmn-test-dmesg-3
        dmn-test-dmesg-5
        dmn-test-dmesg-6
        dmn-test-dmesg-7
        dmn-test-dmesg-8
        dmn-test-dmesg-9
        dmn-test-dmesg-10
        dmn-test-dmesg-11
        dmn-test-dmesg-pb
        dmn-test-dmesg-pb-util
        dmn-test-dmesgnet-1
        dmn-test-dmesgnet-2
        dmn-test-dmesgnet-3
        dmn-test-dmesgnet-4
        dmn-test-dmesgnet-5
        dmn-test-dmesgnet-6
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

  ADD_TEST_VALGRIND(dmn
                      ${DMN_VALGRIND_TESTS}
  )

  add_custom_target(valgrind-check
                      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L valgrind
                      DEPENDS ${DMN_VALGRIND_TESTS}
                      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
endif()
