# CMake generated Testfile for 
# Source directory: /home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests
# Build directory: /home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(RdKafkaTestInParallel "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-build/tests/test-runner" "-p5")
set_tests_properties(RdKafkaTestInParallel PROPERTIES  _BACKTRACE_TRIPLES "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests/CMakeLists.txt;157;add_test;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests/CMakeLists.txt;0;")
add_test(RdKafkaTestSequentially "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-build/tests/test-runner" "-p1")
set_tests_properties(RdKafkaTestSequentially PROPERTIES  _BACKTRACE_TRIPLES "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests/CMakeLists.txt;158;add_test;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests/CMakeLists.txt;0;")
add_test(RdKafkaTestBrokerLess "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-build/tests/test-runner" "-p5" "-l")
set_tests_properties(RdKafkaTestBrokerLess PROPERTIES  _BACKTRACE_TRIPLES "/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests/CMakeLists.txt;159;add_test;/home/runner/work/Dmn/Dmn/_codeql_build_dir/_deps/rdkafka-src/tests/CMakeLists.txt;0;")
subdirs("interceptor_test")
