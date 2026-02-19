# CMake generated Testfile for 
# Source directory: /home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src
# Build directory: /home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[lite-test]=] "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-build/lite-test")
set_tests_properties([=[lite-test]=] PROPERTIES  WORKING_DIRECTORY "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src" _BACKTRACE_TRIPLES "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/cmake/tests.cmake;165;add_test;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/cmake/tests.cmake;0;;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/CMakeLists.txt;333;include;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/CMakeLists.txt;0;")
add_test([=[full-test]=] "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-build/tests")
set_tests_properties([=[full-test]=] PROPERTIES  WORKING_DIRECTORY "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src" _BACKTRACE_TRIPLES "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/cmake/tests.cmake;174;add_test;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/cmake/tests.cmake;0;;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/CMakeLists.txt;333;include;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/CMakeLists.txt;0;")
add_test([=[upb-test]=] "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-build/upb-test")
set_tests_properties([=[upb-test]=] PROPERTIES  WORKING_DIRECTORY "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src" _BACKTRACE_TRIPLES "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/cmake/tests.cmake;208;add_test;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/cmake/tests.cmake;0;;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/CMakeLists.txt;333;include;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/protobuf-src/CMakeLists.txt;0;")
subdirs("../googletest-build")
subdirs("../absl-build")
subdirs("third_party/utf8_range")
