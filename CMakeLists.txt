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

# Update VERSION.txt for a release; app binaries and AppStream metadata use it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" DMN_PROJECT_VERSION
     LIMIT_COUNT 1)
project(dmn VERSION "${DMN_PROJECT_VERSION}")

include(GNUInstallDirs)

# Install components are shared by all applications: Runtime contains
# executables and runtime assets/shared libraries; Development contains
# reusable static libraries, public headers, and developer tools.

# set build options
option(BUILD_KAFKA_TEST
       "Build Kafka integration tests when ENABLE_DMN_DMESGNET is enabled" OFF)
option(BUILD_NDEBUG "Define NDEBUG to disable assertions" OFF)
option(DMN_BUILD_TESTS "Build and register Dmn unit tests" ON)
option(DMN_ALLOW_DEPENDENCY_FETCH
       "Fetch missing build dependencies with CMake FetchContent" ON)
option(ENABLE_DMN_DMESGNET
       "Build the Protobuf-based DMesg/DMesgNet stack and Kafka transport" ON)
option(ENABLE_ANCS "Build the optional ANCS protocol library" OFF)
option(ENABLE_ANCS_HARDWARE_TESTS
       "Enable opt-in ANCS tests that use a physical iPhone over BlueZ" OFF)
option(ENABLE_APP_BLUEZ_DISCOVERY_CLI
       "Build the BlueZ discovery CLI application" OFF)
option(ENABLE_APP_BLUETOOTH_CLI
       "Build the Bluetooth ANCS monitor CLI application" OFF)
option(ENABLE_APP_BLUETOOTH_COSMIC
       "Build the C ABI library used by the Cargo-built COSMIC Bluetooth app" OFF)
option(ENABLE_APP_BLUETOOTH_GTK
       "Build the optional GTK Bluetooth monitor application" OFF)
option(ENABLE_BLUEZ "Build the optional Linux BlueZ library and tests" OFF)
option(ENABLE_DBUS
       "Build the optional D-Bus transport/client library and tests" OFF)
option(ENABLE_FAULT_INJECTION "Enable fault injection tests with libfiu" OFF)
option(ENABLE_VALGRIND "Enable valgrind-based tests (ctest -L valgrind)" OFF)
if (DMN_BUILD_TESTS)
  enable_testing()
endif()

if (BUILD_KAFKA_TEST AND NOT ENABLE_DMN_DMESGNET)
  message(STATUS
          "BUILD_KAFKA_TEST=ON but ENABLE_DMN_DMESGNET=OFF; Kafka integration tests are skipped.")
endif()
set(DMN_ANCS_HARDWARE_DEVICE "iPhone Old" CACHE STRING
    "Exact BlueZ display name or address used by opt-in ANCS hardware tests")

if (BUILD_KAFKA_TEST AND ENABLE_DMN_DMESGNET)
  set(DMN_KAFKA_BOOTSTRAP_SCRIPT
      "${PROJECT_SOURCE_DIR}/scripts/run-bookstrap-dmn.sh")
  set(DMN_KAFKA_BUILD_SCRIPT
      "${CMAKE_BINARY_DIR}/scripts/run-bookstrap-dmn.sh")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/scripts")
  configure_file("${DMN_KAFKA_BOOTSTRAP_SCRIPT}"
                 "${DMN_KAFKA_BUILD_SCRIPT}" COPYONLY)
  file(CHMOD "${DMN_KAFKA_BUILD_SCRIPT}"
       PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                   GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
  install(PROGRAMS "${DMN_KAFKA_BOOTSTRAP_SCRIPT}"
          DESTINATION scripts
          COMPONENT Development
  )
endif()

# Application options enable their required library layers; BlueZ in turn
# enables the D-Bus layer. All optional Bluetooth applications and libraries
# default off, so the default configuration does not require Linux or libdbus.
if (ENABLE_APP_BLUEZ_DISCOVERY_CLI)
  set(ENABLE_BLUEZ ON CACHE BOOL
      "Build the optional Linux BlueZ library and tests" FORCE)
endif()

if (ENABLE_APP_BLUETOOTH_CLI)
  set(ENABLE_BLUEZ ON CACHE BOOL
      "Build the optional Linux BlueZ library and tests" FORCE)
  set(ENABLE_ANCS ON CACHE BOOL
      "Build the optional ANCS protocol library" FORCE)
endif()

if (ENABLE_APP_BLUETOOTH_COSMIC)
  set(ENABLE_BLUEZ ON CACHE BOOL
      "Build the optional Linux BlueZ library and tests" FORCE)
  set(ENABLE_ANCS ON CACHE BOOL
      "Build the optional ANCS protocol library" FORCE)
  # The C ABI library is shared and links the static Dmn layers.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if (ENABLE_APP_BLUETOOTH_GTK)
  set(ENABLE_BLUEZ ON CACHE BOOL
      "Build the optional Linux BlueZ library and tests" FORCE)
  set(ENABLE_ANCS ON CACHE BOOL
      "Build the optional ANCS protocol library" FORCE)
endif()

if (ENABLE_BLUEZ)
  set(ENABLE_DBUS ON CACHE BOOL
      "Build the optional D-Bus transport/client library and tests" FORCE)
endif()

if (ENABLE_ANCS_HARDWARE_TESTS AND NOT ENABLE_APP_BLUETOOTH_CLI)
  message(FATAL_ERROR
          "ENABLE_ANCS_HARDWARE_TESTS=ON requires ENABLE_APP_BLUETOOTH_CLI=ON")
endif()

if (ENABLE_BLUEZ)
  if (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "ENABLE_BLUEZ=ON is supported only on Linux")
  endif()
endif()

if (ENABLE_DBUS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
endif()

if (ENABLE_APP_BLUETOOTH_COSMIC OR
    ENABLE_APP_BLUETOOTH_GTK)
  pkg_check_modules(GLIB2 REQUIRED IMPORTED_TARGET glib-2.0)
endif()

if (ENABLE_APP_BLUETOOTH_GTK)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.14)
  pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.5)

  find_program(GLIB_COMPILE_SCHEMAS_EXECUTABLE
               NAMES glib-compile-schemas
               REQUIRED
  )

  find_program(GTK4_UPDATE_ICON_CACHE_EXECUTABLE
               NAMES gtk4-update-icon-cache
  )
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
    if (NOT DMN_ALLOW_DEPENDENCY_FETCH)
      message(FATAL_ERROR
              "libfiu was not found and DMN_ALLOW_DEPENDENCY_FETCH=OFF. "
              "Install libfiu and fiu-run or disable ENABLE_FAULT_INJECTION.")
    endif()

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

if (ENABLE_DMN_DMESGNET)
  message(STATUS "Looking for Protobuf...")
  find_package(Protobuf QUIET)

  if (NOT Protobuf_FOUND)
    if (DMN_ALLOW_DEPENDENCY_FETCH)
      # The fetched dependency shares this build's CTest project. Keep its own
      # tests out of Dmn's test registry, since EXCLUDE_FROM_ALL leaves those
      # test executables unbuilt.
      set(protobuf_BUILD_TESTS OFF CACHE BOOL
          "Build Protobuf tests" FORCE)
      set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL
          "Build Protobuf conformance tests" FORCE)

      FetchContent_Declare(protobuf
                             GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
                             GIT_TAG        v30.0-rc1
                             EXCLUDE_FROM_ALL
      )

      FetchContent_MakeAvailable(protobuf)
    else()
      message(FATAL_ERROR
              "Protobuf was not found and DMN_ALLOW_DEPENDENCY_FETCH=OFF. "
              "Install Protobuf development files or provide it as a build "
              "dependency before configuring Dmn.")
    endif()
  endif()
endif()

if (DMN_BUILD_TESTS)
  message(STATUS "Looking for GoogleTest...")

  find_package(GTest QUIET)
  if (GTest_FOUND)
    set(DMN_GTEST_TARGET GTest::gtest)
    set(DMN_GTEST_MAIN_TARGET GTest::gtest_main)
  elseif (DMN_ALLOW_DEPENDENCY_FETCH)
    FetchContent_Declare(googletest
                           GIT_REPOSITORY https://github.com/google/googletest.git
                           GIT_TAG        v1.14.0
                           EXCLUDE_FROM_ALL
    )

    FetchContent_MakeAvailable(googletest)
    set(DMN_GTEST_TARGET gtest)
    set(DMN_GTEST_MAIN_TARGET gtest_main)
  else()
    message(FATAL_ERROR
            "GoogleTest was not found and DMN_ALLOW_DEPENDENCY_FETCH=OFF. "
            "Install GoogleTest or configure with DMN_BUILD_TESTS=OFF.")
  endif()
endif()

if (ENABLE_DMN_DMESGNET)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(RDKAFKA QUIET IMPORTED_TARGET rdkafka)

  if (TARGET PkgConfig::RDKAFKA)
    find_path(DMN_RDKAFKA_INCLUDE_DIR
                NAMES rdkafka.h
                HINTS ${RDKAFKA_INCLUDE_DIRS}
                PATH_SUFFIXES librdkafka
    )
    if (NOT DMN_RDKAFKA_INCLUDE_DIR)
      message(FATAL_ERROR
              "librdkafka was found through pkg-config, but rdkafka.h could "
              "not be located in its include directories.")
    endif()
    set(DMN_RDKAFKA_TARGET PkgConfig::RDKAFKA)
  else()
    if (NOT DMN_ALLOW_DEPENDENCY_FETCH)
      message(FATAL_ERROR
              "librdkafka was not found through pkg-config and "
              "DMN_ALLOW_DEPENDENCY_FETCH=OFF. Install librdkafka development "
              "files or provide it as a build dependency before configuring Dmn.")
    endif()

    # librdkafka registers its own tests, which Dmn does not use.
    set(RDKAFKA_BUILD_TESTS OFF CACHE BOOL
        "Build librdkafka tests" FORCE)
    FetchContent_Declare(rdkafka
                           GIT_REPOSITORY https://github.com/confluentinc/librdkafka.git
                           GIT_TAG        v2.8.0
                           EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(rdkafka)

    set(DMN_RDKAFKA_INCLUDE_DIR "${rdkafka_SOURCE_DIR}/src")
    if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      set(rdkafka_LIB "${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src/librdkafka.dylib")
    else()
      set(rdkafka_LIB "${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src/librdkafka.so")
    endif()
    set(DMN_RDKAFKA_TARGET rdkafka)
    set(CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/_deps/rdkafka-build/src")
  endif()
endif()

if(BUILD_NDEBUG)
  add_compile_definitions(NDEBUG)
endif()

# build DMN library and applications
add_subdirectory(src)

if (DMN_BUILD_TESTS)
  # Configure and register the DMN unit tests.
  message(STATUS "Configure DMN unit tests...")
  add_subdirectory(test)
endif()
