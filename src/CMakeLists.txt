# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

# build the dmn library
message(STATUS "build dmn...")
add_library(dmn
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesg.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesgnet.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-interval-btree.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-proc.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-runtime-state.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-runtime.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-socket.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-state.cpp

              # kafka
              ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-dmesgnet-kafka.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-kafka-util.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-kafka.cpp

              # proto c++ sources
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dlock.pb.cc
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.cc
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.cc
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.cc
)

GENERATE_PROTOBUF(dmn
                    dmn-dlock.proto
                    dmn-dmesg-body.proto
                    dmn-dmesg-type.proto
                    dmn-dmesg.proto
)

target_include_directories(dmn PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_include_directories(dmn INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(dmn INTERFACE ${PROJECT_SOURCE_DIR})
target_compile_options(dmn PRIVATE -Wall -Wextra -Wpedantic)

set_target_properties(dmn PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_dependencies(dmn rdkafka)
# Public core headers: consumers see these through the dmn target interface.
target_sources(dmn
                 INTERFACE
                 ${PROJECT_SOURCE_DIR}/include/dmn-async.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-blockingqueue-lf.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-blockingqueue-mt.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-blockingqueue.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-debug.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-dlock.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-dmesg-pb-util.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-dmesg.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-dmesgnet.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-inflight-guard.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-interval-btree.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-io.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-pipe.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-proc.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-pub-sub.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-runtime-state.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-runtime-task.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-runtime.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-singleton.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-socket.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-state.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-timer.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn-util.hpp
                 ${PROJECT_SOURCE_DIR}/include/dmn.hpp

                 # kafka
                 ${PROJECT_SOURCE_DIR}/include/kafka/dmn-dmesgnet-kafka.hpp
                 ${PROJECT_SOURCE_DIR}/include/kafka/dmn-kafka-util.hpp
                 ${PROJECT_SOURCE_DIR}/include/kafka/dmn-kafka.hpp

                 # proto c++ sources
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dlock.pb.h
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.h
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.h
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.h
)

# Deprecated headers are retained in the source tree for migration reference;
# they are not supported public interfaces and are intentionally not installed.
target_sources(dmn
                 PRIVATE
                 ${PROJECT_SOURCE_DIR}/include/deprecated/dmn-limit-blockingqueue.hpp
                 ${PROJECT_SOURCE_DIR}/include/deprecated/dmn-teepipe.hpp
)

if (ENABLE_FAULT_INJECTION)
  target_sources(dmn
                   INTERFACE
                   ${PROJECT_SOURCE_DIR}/include/dmn-fault-injection.hpp
  )
endif()

target_include_directories(dmn
                             INTERFACE
                             ${protobuf_SOURCE_DIR}/src
                             ${absl_SOURCE_DIR}
                             ${CMAKE_CURRENT_SOURCE_DIR}
                             ${rdkafka_INCLUDE}
)

target_include_directories(dmn
                             PRIVATE
                             ${rdkafka_INCLUDE}
)

if(ENABLE_FAULT_INJECTION)
  # Expose libfiu's enabled failure-point API to Dmn and its test executables.
  target_compile_definitions(dmn
                               PUBLIC
                               FIU_ENABLE=1
  )

  target_link_libraries(dmn PUBLIC ${DMN_LIBFIU_TARGET})
  if (TARGET libfiu-build)
    add_dependencies(dmn libfiu-build)
  endif()
endif()

if(NOT APPLE)
  target_link_libraries(dmn
                          PUBLIC
                          ICU::uc
                          protobuf::libprotobuf
                          ${rdkafka_LIB}
                          atomic
  )
else()
  target_link_libraries(dmn
                          PUBLIC
                          ICU::uc
                          protobuf::libprotobuf
                          ${rdkafka_LIB}
  )
endif()

if (ENABLE_DBUS)
  add_library(dmn-dbus
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-client.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-io.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-runtime.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dbus-value.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesgnet-dbus.cpp
  )

  target_include_directories(dmn-dbus
                               PUBLIC ${PROJECT_SOURCE_DIR}/include
  )

  # Public optional API; the implementation-only runtime header stays PRIVATE.
  target_sources(dmn-dbus
                   INTERFACE
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-client.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-config.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-io.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dbus-value.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-dmesgnet-dbus.hpp
  )

  # Internal implementation header shared only by this target's .cpp files.
  target_sources(dmn-dbus
                   PRIVATE ${PROJECT_SOURCE_DIR}/include/dmn-dbus-runtime.hpp
  )

  target_compile_options(dmn-dbus PRIVATE -Wall -Wextra -Wpedantic)

  target_compile_definitions(dmn-dbus PUBLIC DMN_ENABLE_DBUS=1)

  target_link_libraries(dmn-dbus
                          PUBLIC dmn
                          PRIVATE PkgConfig::DBUS
  )

  install(TARGETS dmn-dbus
            ARCHIVE DESTINATION lib/dmn
            LIBRARY DESTINATION lib/dmn
            RUNTIME DESTINATION bin
  )
endif()

if (ENABLE_BLUEZ)
  add_library(dmn-bluez
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-bluez-client.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/dmn-bluez-gatt-client.cpp
  )

  target_include_directories(dmn-bluez
                               PUBLIC ${PROJECT_SOURCE_DIR}/include
  )

  target_sources(dmn-bluez
                   INTERFACE
                   ${PROJECT_SOURCE_DIR}/include/dmn-bluez-client.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-bluez-gatt-client.hpp
                   ${PROJECT_SOURCE_DIR}/include/dmn-gatt-client.hpp
  )

  target_compile_definitions(dmn-bluez PUBLIC DMN_ENABLE_BLUEZ=1)
  target_compile_options(dmn-bluez PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(dmn-bluez PUBLIC dmn-dbus)

  add_executable(dmn-bluez-discovery
                   dmn-bluez-discovery.cpp
  )

  target_compile_options(dmn-bluez-discovery
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluez-discovery PRIVATE dmn-bluez)

  add_executable(dmn-bluez-discovery-test
                   dmn-bluez-discovery.cpp
  )

  target_compile_definitions(dmn-bluez-discovery-test
                               PRIVATE DMN_BLUEZ_TESTING=1
                                       DMN_BLUEZ_TEST_OPERATION_TIMEOUT_MS=500
  )

  target_compile_options(dmn-bluez-discovery-test
                           PRIVATE -Wall -Wextra -Wpedantic
  )

  target_link_libraries(dmn-bluez-discovery-test PRIVATE dmn-bluez)

  install(TARGETS dmn-bluez dmn-bluez-discovery
            ARCHIVE DESTINATION lib/dmn
            LIBRARY DESTINATION lib/dmn
            RUNTIME DESTINATION bin
  )

  install(FILES ${PROJECT_SOURCE_DIR}/docs/man/dmn-bluez-discovery.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
  )
endif()

# Install the library binary
install(TARGETS dmn
          #   EXPORT mylibTargets
          ARCHIVE DESTINATION lib/dmn
          LIBRARY DESTINATION lib/dmn
          RUNTIME DESTINATION bin
          INCLUDES DESTINATION include/dmn
)

# Install only headers for enabled optional features.
set(DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS)

list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
       PATTERN "dmn-dbus-runtime.hpp" EXCLUDE
)

if (NOT ENABLE_DBUS)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-dbus-*.hpp" EXCLUDE
         PATTERN "dmn-dmesgnet-dbus.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_BLUEZ)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-bluez-*.hpp" EXCLUDE
  )
endif()

if (NOT ENABLE_FAULT_INJECTION)
  list(APPEND DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS
         PATTERN "dmn-fault-injection.hpp" EXCLUDE
  )
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/
          DESTINATION include/dmn
          PATTERN "deprecated" EXCLUDE
          ${DMN_EXCLUDED_PUBLIC_HEADER_PATTERNS}
)

# Export the targets for use with find_package()
#install(EXPORT mylibTargets
#    FILE mylibTargets.cmake
#    NAMESPACE MyLib::
#    DESTINATION lib/cmake/mylib
#)


# build executable that uses dmn
message(STATUS "build dmn applications...")
add_executable(dmn-dmesg-daemon
                 dmn-dmesg-daemon.cpp
)

target_include_directories(dmn-dmesg-daemon
                             PRIVATE
                             ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(dmn-dmesg-daemon
                        PRIVATE
                        dmn
                        ${rdkafka_LIB}
)


# build executable that uses dmn kafka
add_executable(dmn-kafka-receiver
                 dmn-kafka-receiver.cpp
)

target_include_directories(dmn-kafka-receiver
                             PRIVATE
                             ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(dmn-kafka-receiver
                             PRIVATE
                             dmn
                             ${rdkafka_LIB}
)

target_link_libraries(dmn-kafka-receiver
                        PRIVATE
                        dmn
                        ${rdkafka_LIB}
)

add_executable(dmn-kafka-sender
                 dmn-kafka-sender.cpp
)

target_include_directories(dmn-kafka-sender
                             PRIVATE
                             ${PROJECT_SOURCE_DIR}/include
)

target_include_directories(dmn-kafka-sender
                             PRIVATE
                             dmn
                             ${rdkafka_LIB}
)

target_link_libraries(dmn-kafka-sender
                        PRIVATE
                        dmn
                        ${rdkafka_LIB}
)
