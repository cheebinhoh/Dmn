/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-io.cpp
 * @brief Unit test for Dmn_Pipe and Dmn_Proc I/O operations including
 * multi-threaded read/write.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "dmn-state.hpp"

static std::mutex log_mutex{};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_State countTo5{"Count to 5"};

  // Dmn_Proc and Dmn_Pipe will be destroyed and display statistics
  return RUN_ALL_TESTS();
}
