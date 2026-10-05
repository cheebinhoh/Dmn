# Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
#
# This defines reusable CMake macro
#

# @brief Create GoogleTest executables and register them with CTest.
#
# @param label First argument; assigned as the CTest label for every test.
# @param ... Test executable target names. Each target is built from a matching
#        <target>.cpp source file.
#
# Example:
# @code
# ADD_TEST_EXECUTABLE(dmn dmn-test-async dmn-test-timer)
# @endcode
macro(ADD_TEST_EXECUTABLE ...)
  foreach (arg ${ARGN})
    message("adding test executable ${arg}")

    add_executable(${arg}
                     ${arg}.cpp
    )

    target_include_directories(${arg}
                                 PRIVATE
                                 ${PROJECT_SOURCE_DIR}/include
                                 ${PROJECT_SOURCE_DIR}/test/include
    )

    target_compile_options(${arg}
                             PRIVATE
                             -Wall -Wextra -Wpedantic
    )

    target_link_libraries(${arg}
                            PRIVATE
                            gtest_main
                            gtest
                            dmn
                            protobuf::libprotobuf
    )

    add_test(NAME ${arg} COMMAND ${arg})
    set_tests_properties(${arg} PROPERTIES LABELS "${ARGV0}")
  endforeach()
endmacro()

# @brief Create a GoogleTest executable that runs under libfiu.
#
# @param test_program Test executable target name; its source is
#        <test_program>.cpp.
# @param ... One or more libfiu failure-point names to enable for this test.
#        All listed failure points are enabled together for each CTest run.
#
# The executable is registered with the CTest label "fault-injection" and is
# invoked through fiu-run.
# Use separate test executables when different test cases require different
# failure points, so each CTest invocation enables only the intended point.
#
# Example:
# @code
# ADD_TEST_FAULT_INJECTION_EXECUTABLE(
#   dmn-test-fi-timer-thread-start-failure
#   dmn/timer/pipe/proc/pthread_create
# )
# @endcode
function(ADD_TEST_FAULT_INJECTION_EXECUTABLE test_program)
  if (NOT ARGN)
    message(FATAL_ERROR
              "ADD_TEST_FAULT_INJECTION_EXECUTABLE requires at least one failure point"
    )
  endif()

  if (NOT TARGET fault-injection-tests)
    add_custom_target(fault-injection-tests)
  endif()

  message("adding fault-injection test executable ${test_program}")

  add_executable(${test_program}
                   ${test_program}.cpp
  )

  target_include_directories(${test_program}
                               PRIVATE
                               ${PROJECT_SOURCE_DIR}/include
                               ${PROJECT_SOURCE_DIR}/test/include
  )

  target_compile_options(${test_program}
                           PRIVATE
                           -Wall -Wextra -Wpedantic
  )

  target_link_libraries(${test_program}
                          PRIVATE
                          gtest
                          dmn
                          protobuf::libprotobuf
  )

  if (TARGET libfiu-tools)
    add_dependencies(${test_program} libfiu-tools)
  endif()

  add_dependencies(fault-injection-tests ${test_program})

  set(fiu_run_args)
  foreach (failure_point IN LISTS ARGN)
    list(APPEND fiu_run_args -c "enable name=${failure_point}")
  endforeach()

  add_test(NAME ${test_program}
             COMMAND "${LIBFIU_RUN}"
             ${fiu_run_args}
             --
             $<TARGET_FILE:${test_program}>
  )

  set_tests_properties(${test_program} PROPERTIES LABELS "fault-injection")
endfunction()


# @brief Register existing executable targets as Valgrind tests.
#
# @param unused First argument is currently ignored; existing calls pass
#        "dmn" here.
# @param ... Executable target names to run under Valgrind. Each test receives
#        the CTest label "valgrind".
#
# Example:
# @code
# ADD_TEST_VALGRIND(dmn dmn-test-async dmn-test-timer)
# @endcode
macro(ADD_TEST_VALGRIND ...)
  foreach (arg ${ARGN})
    message(STATUS "adding test executable ${arg} for valgrind")

    add_test(NAME valgrind-${arg}
               COMMAND
               ${VALGRIND_EXECUTABLE}
               --quiet
               --error-exitcode=42
               --leak-check=full
               --show-leak-kinds=all
               --track-origins=yes
               $<TARGET_FILE:${arg}>
    )

    set_tests_properties(valgrind-${arg} PROPERTIES LABELS "valgrind")
  endforeach()
endmacro()


# @brief Generate C++ protobuf sources and add them to an existing target.
#
# @param target First argument; existing target that receives the generated
#        sources and the binary directory as a private include directory.
# @param ... Proto file names relative to
#        ${CMAKE_CURRENT_SOURCE_DIR}/proto.
#
# Example:
# @code
# GENERATE_PROTOBUF(dmn dmn-dmesg.proto)
# @endcode
macro(GENERATE_PROTOBUF ...)
  foreach(arg ${ARGN})
    target_include_directories(${ARGV0} PRIVATE
                                 ${CMAKE_CURRENT_BINARY_DIR}
    )

    protobuf_generate(TARGET ${ARGV0}
                        PROTOS ${CMAKE_CURRENT_SOURCE_DIR}/proto/${arg}
                        LANGUAGE cpp
                        OUT_VAR PROTOBUF_GENERATED_FILES
                        PROTOC_OPTIONS "-I${protobuf_SOURCE_DIR}/src"
                        PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}
    )
  endforeach()
endmacro()
