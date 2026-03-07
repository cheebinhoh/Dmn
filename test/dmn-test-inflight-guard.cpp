/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-inflight-guard.cpp
 * @brief The unit test for dmn-inflight-guard module.
 */

#include <gtest/gtest.h>

#include <iostream>

#include "dmn-inflight-guard.hpp"

class InflightTest : public dmn::Dmn_Inflight_Guard {
public:
  InflightTest() {}

  ~InflightTest() {}

  void doMethod1() {
    auto ticket = this->enterInflightGate();

    EXPECT_TRUE((this->inflight_count() == 1));

    doMethod2_from_1();

    EXPECT_TRUE((this->inflight_count() == 1));
  }

  void doMethod2_from_1() {
    auto ticket = this->enterInflightGate();

    EXPECT_TRUE((this->inflight_count() == 2));
  }
};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  InflightTest ft{};
  ft.doMethod1();

  return RUN_ALL_TESTS();
}
