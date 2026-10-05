# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.

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
option(BUILD_NDEBUG "build without debug" OFF)
option(BUILD_KAFKA_TEST "build with kafka test" OFF)
option(ENABLE_DBUS "Build the optional D-Bus transport" OFF)
option(ENABLE_VALGRIND "Enable valgrind-based tests (ctest -L valgrind)" OFF)
option(ENABLE_FAULT_INJECTION "Enable fault injection tests with libfiu" OFF)

if (ENABLE_DBUS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
endif()

if (ENABLE_VALGRIND)
  find_program(VALGRIND_EXECUTABLE valgrind)
  if (NOT VALGRIND_EXECUTABLE)
    message(FATAL_ERROR "ENABLE_VALGRIND=ON but valgrind was not found in PATH")
  endif()
endif()

# include cmake macro and fetch dependent modules
include(CMakeMacro.cmake)
include(FetchContent)

find_package(ICU REQUIRED COMPONENTS uc)

# Fault injection is opt-in; prefer installed libfiu, otherwise fetch and build it.
if (ENABLE_FAULT_INJECTION)
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(LIBFIU QUIET IMPORTED_TARGET libfiu)
  endif()

  if (TARGET PkgConfig::LIBFIU)
    set(DMN_LIBFIU_TARGET PkgConfig::LIBFIU)
    set(DMN_LIBFIU_LIBRARY_DIR "${LIBFIU_LIBRARY_DIRS}")
    find_program(LIBFIU_RUN NAMES fiu-run REQUIRED)
  else()
    message(STATUS "find or fetching libfiu...")

    FetchContent_Declare(libfiu
                           GIT_REPOSITORY https://github.com/albertito/libfiu.git
                           GIT_TAG 1.2
    )

    FetchContent_GetProperties(libfiu)
    if (NOT libfiu_POPULATED)
      FetchContent_Populate(libfiu)
    endif()

    find_program(LIBFIU_MAKE_EXECUTABLE NAMES gmake make REQUIRED)

    set(LIBFIU_SHARED "${libfiu_SOURCE_DIR}/libfiu/libfiu.so.1.2")
    set(DMN_LIBFIU_LIBRARY_DIR "${libfiu_SOURCE_DIR}/libfiu")

    add_custom_command(OUTPUT "${LIBFIU_SHARED}"
                         COMMAND "${LIBFIU_MAKE_EXECUTABLE}" libs
                         WORKING_DIRECTORY "${libfiu_SOURCE_DIR}/libfiu"
                         COMMENT "Building libfiu"
                         VERBATIM
    )

    add_custom_target(libfiu-build DEPENDS "${LIBFIU_SHARED}")
    add_library(dmn-libfiu SHARED IMPORTED GLOBAL)

    set_target_properties(dmn-libfiu PROPERTIES
                            IMPORTED_LOCATION "${LIBFIU_SHARED}"
                            INTERFACE_INCLUDE_DIRECTORIES "${libfiu_SOURCE_DIR}/libfiu"
                            INTERFACE_LINK_OPTIONS "LINKER:-rpath,${libfiu_SOURCE_DIR}/libfiu"
    )

    set(DMN_LIBFIU_TARGET dmn-libfiu)
    set(LIBFIU_RUN_DIR "${libfiu_SOURCE_DIR}/preload/run")
    set(LIBFIU_RUN_PRELOAD "${LIBFIU_RUN_DIR}/fiu_run_preload.so")
    set(LIBFIU_RUN "${LIBFIU_RUN_DIR}/fiu-run")

    add_custom_command(OUTPUT "${LIBFIU_RUN_PRELOAD}" "${LIBFIU_RUN}"
                         COMMAND "${LIBFIU_MAKE_EXECUTABLE}" all "PLIBPATH=${LIBFIU_RUN_DIR}"
                         WORKING_DIRECTORY "${LIBFIU_RUN_DIR}"
                         DEPENDS "${LIBFIU_SHARED}"
                         COMMENT "Building fiu-run"
                         VERBATIM
    )

    add_custom_target(libfiu-tools
                        DEPENDS "${LIBFIU_RUN_PRELOAD}" "${LIBFIU_RUN}"
    )
  endif()
endif()

message(STATUS "find or fetching protobuf...")
find_package(Protobuf QUIET)

if (NOT Protobuf_FOUND)
FetchContent_Declare(protobuf
                       GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
                       GIT_TAG        v30.0-rc1
                       EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(protobuf)
endif()

message(STATUS "find or fetching googletest...")

FetchContent_Declare(googletest
                       GIT_REPOSITORY https://github.com/google/googletest.git
                       GIT_TAG        v1.14.0
                       EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(googletest)

message(STATUS "fetching rdkafka...")
FetchContent_Declare(rdkafka
                       GIT_REPOSITORY https://github.com/confluentinc/librdkafka.git
                       GIT_TAG        v2.8.0
                       EXCLUDE_FROM_ALL
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
