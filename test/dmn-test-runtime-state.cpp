/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-runtime-state.cpp
 * @brief Unit test for Dmn_Runtime_State_Manager construction.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <type_traits>

#include "dmn-runtime-state.hpp"

static std::mutex log_mutex{};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto first = dmn::Dmn_Runtime_State_Manager::createInstance();
  auto second = dmn::Dmn_Runtime_State_Manager::createInstance();

  EXPECT_NE(first, nullptr);
  EXPECT_EQ(first.get(), second.get());

  static_assert(std::is_base_of_v<dmn::Dmn_State, dmn::Dmn_Runtime_State>);

  auto state = first->createState("runtime-state-test");

  EXPECT_NE(state, nullptr);

  bool stateFncRan{};
  state->setStateFnc([&stateFncRan](dmn::Dmn_State &machine) {
    stateFncRan = true;
    machine.setEnd();
  });
  EXPECT_TRUE(state->hasStateFncs());

  auto &baseState = static_cast<dmn::Dmn_State &>(*state);
  EXPECT_TRUE(!baseState.isInitialized());
  EXPECT_TRUE(!baseState.isFinalized());

  while (baseState) {
    baseState.runNext();
  }

  EXPECT_TRUE(baseState.isInitialized());
  EXPECT_TRUE(baseState.isFinalized());
  EXPECT_TRUE(stateFncRan);

  return RUN_ALL_TESTS();
}
