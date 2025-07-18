# Copyright © 2025 Chee Bin HOH. All rights reserved.

# build the dmn library
message(STATUS "build dmn...")
add_library(dmn
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-async.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesg.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-dmesgnet.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-event.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-proc.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/dmn-socket.cpp

              # kafka
              ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-dmesgnet-kafka.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-kafka-util.cpp
              ${CMAKE_CURRENT_SOURCE_DIR}/kafka/dmn-kafka.cpp

              # proto c++ sources
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.cc
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.cc
              ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.cc
)

GENERATE_PROTOBUF(dmn
                    dmn-dmesg-type.proto
                    dmn-dmesg-body.proto
                    dmn-dmesg.proto
)

target_include_directories(dmn PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_include_directories(dmn INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(dmn INTERFACE ${CMAKE_SOURCE_DIR})
target_compile_options(dmn PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(dmn PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_dependencies(dmn rdkafka)
target_sources(dmn
                 PRIVATE
                 ${CMAKE_SOURCE_DIR}/include/dmn-debug.hpp
)

target_sources(dmn
                 INTERFACE
                 ${CMAKE_SOURCE_DIR}/include/dmn.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-async.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-buffer.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-event.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-io.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-limit-buffer.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-dmesg.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-dmesgnet.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-dmesg-pb-util.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-pipe.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-proc.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-pub-sub.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-singleton.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-socket.hpp
                 ${CMAKE_SOURCE_DIR}/include/dmn-teepipe.hpp

                 # kafka
                 ${CMAKE_SOURCE_DIR}/include/kafka/dmn-dmesgnet-kafka.hpp
                 ${CMAKE_SOURCE_DIR}/include/kafka/dmn-kafka-util.hpp
                 ${CMAKE_SOURCE_DIR}/include/kafka/dmn-kafka.hpp

                 # proto c++ sources
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-body.pb.h
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg-type.pb.h
                 ${CMAKE_CURRENT_BINARY_DIR}/proto/dmn-dmesg.pb.h
)

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

target_link_libraries(dmn
                        PRIVATE
                        protobuf::libprotobuf
                        ${rdkafka_LIB}
)


# build executable that uses dmn
message(STATUS "build dmn applications...")
add_executable(dmn-dmesg-daemon
                 dmn-dmesg-daemon.cpp
)

target_include_directories(dmn-dmesg-daemon
                             PRIVATE
                             ${CMAKE_SOURCE_DIR}/include)

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
                             ${CMAKE_SOURCE_DIR}/include)

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
                             ${CMAKE_SOURCE_DIR}/include)

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

