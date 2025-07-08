# Copyright © 2025 Chee Bin HOH. All rights reserved.

# set project and generic compilation options
cmake_minimum_required(VERSION 3.26)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ABSL_PROPAGATE_CXX_STD ON)

#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

project(dmn)

enable_testing()


# set build options
option(FETCH_PROTOBUF "fetch protobuf from github" OFF)
option(BUILD_NDEBUG "build without debug" OFF)
option(BUILD_KAFKA_TEST "build with kafka test" OFF)


# include cmake macro and fetch dependent modules
include(CMakeMacro.cmake)
include(FetchContent)

if (FETCH_PROTOBUF)
  message(STATUS "fetching protobuf from github")
  FetchContent_Declare(protobuf
                       GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
                       GIT_TAG        v30.0-rc1
  )

  FetchContent_MakeAvailable(protobuf)
else() 
  message(STATUS "find or fetching protobuf...")
  find_package(Protobuf QUIET)

  if(NOT Protobuf_FOUND)
    message(STATUS "fetching protobuf from github")
    FetchContent_Declare(protobuf
                         GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
                         GIT_TAG        v30.0-rc1
    )

    FetchContent_MakeAvailable(protobuf)
  endif()
endif()

message(STATUS "find or fetching googletest...")

FetchContent_Declare(googletest
                       GIT_REPOSITORY https://github.com/google/googletest.git
                       GIT_TAG        v1.14.0
)

FetchContent_MakeAvailable(googletest)

message(STATUS "fetching rdkafka...")
FetchContent_Declare(rdkafka
                       GIT_REPOSITORY https://github.com/confluentinc/librdkafka.git
                       GIT_TAG        v2.8.0
)

FetchContent_MakeAvailable(rdkafka)

string(REPLACE "-src" "-build" rdkafka_BUILD_DIR "${rdkafka_SOURCE_DIR}")
set(rdkafka_INCLUDE ${rdkafka_SOURCE_DIR}/src)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(STATUS "Running on macOS")
    set(rdkafka_LIB ${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src/librdkafka.dylib)
else()
    message(STATUS "Running on none-macOS")
    set(rdkafka_LIB ${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src/librdkafka.so)
endif()

message(STATUS "kakfa library reference is set to ${rdkafka_LIB}")

set(CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src")
set(CMAKE_INSTALL_RPATH "${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src")

if(BUILD_NDEBUG)
  add_compile_definitions(NDEBUG)
endif()

# build DMN library and applications
add_subdirectory(src)

# run unit test for DMN
message(STATUS "build and run unit test...")
add_subdirectory(test)

