/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-runtime-state.cpp
 * @brief Unit test for Dmn_Runtime_State_Manager construction.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "dmn-runtime-state.hpp"

static std::mutex log_mutex{};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto first = dmn::Dmn_Runtime_State_Manager::createInstance();
  auto second = dmn::Dmn_Runtime_State_Manager::createInstance();

  EXPECT_NE(first, nullptr);
  EXPECT_EQ(first.get(), second.get());

  return RUN_ALL_TESTS();
}
