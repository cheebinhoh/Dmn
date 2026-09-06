/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-state.cpp
 * @brief Unit tests for Dmn_State lifecycle and user-state transitions.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "dmn-state.hpp"

static std::mutex log_mutex{};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_State s1{"default"};

  EXPECT_FALSE(s1);
  EXPECT_TRUE(!s1);
  EXPECT_TRUE(!s1.isInitialized());
  EXPECT_TRUE(!s1.isFinalized());
  EXPECT_FALSE(s1.hasStateFncs());

  EXPECT_FALSE(s1.runNext());
  EXPECT_FALSE(s1);
  EXPECT_TRUE(s1.isInitialized());
  EXPECT_TRUE(s1.isFinalized());

  dmn::Dmn_State s2{"default2"};
  int s2_count = 0;
  s2.setStateFnc([&s2_count](dmn::Dmn_State &s) {
    ++s2_count;
    s.setEnd();
  });

  EXPECT_TRUE(s2);
  EXPECT_TRUE(!s2.isInitialized());
  EXPECT_TRUE(!s2.isFinalized());

  EXPECT_FALSE(s2.runNext());
  EXPECT_TRUE(!s2);
  EXPECT_TRUE(s2.isInitialized());
  EXPECT_TRUE(s2.isFinalized());
  EXPECT_EQ(s2_count, 1);
  EXPECT_THROW(s2.setNext(0), std::out_of_range);
  EXPECT_THROW(s2.setNext(-1), std::out_of_range);
  EXPECT_THROW(s2.setNext(3), std::out_of_range);

  int s3_count = 0;
  dmn::Dmn_State s3{"count up to 3"};
  EXPECT_TRUE(!s3.isInitialized());
  EXPECT_TRUE(!s3.isFinalized());

  s3.setStateFnc([&s3_count](dmn::Dmn_State &s) {
    s3_count++;

    if (3 <= s3_count) {
      s.setEnd();
    }
  });

  EXPECT_TRUE(s3.hasStateFncs());

  EXPECT_TRUE(s3.runNext());
  EXPECT_EQ(s3_count, 1);
  EXPECT_TRUE(s3.isInitialized());
  EXPECT_FALSE(s3.isFinalized());

  while (s3.runNext()) {
  }

  EXPECT_TRUE(!s3);
  EXPECT_TRUE(s3.isInitialized());
  EXPECT_TRUE(s3.isFinalized());
  EXPECT_TRUE(3 == s3_count);

  int s4_state_count = 0;
  int s4_count = 0;
  dmn::Dmn_State s4{"two states: step to 4 (by 1), step to 10 (by 2)"};
  EXPECT_TRUE(!s4.isInitialized());
  EXPECT_TRUE(!s4.isFinalized());

  s4.setStateFnc([&s4_count, &s4_state_count](dmn::Dmn_State &s) {
    s4_count++;
    s4_state_count++;

    if (4 <= s4_count) {
      s.setNext();
    }
  });

  s4.setStateFnc([&s4_count, &s4_state_count](dmn::Dmn_State &s) {
    s4_count += 2;
    s4_state_count++;

    if (10 <= s4_count) {
      s.setNext();
    }
  });

  while (s4) {
    s4.runNext();
  }

  EXPECT_TRUE(!s4);
  EXPECT_TRUE(s4.isInitialized());
  EXPECT_TRUE(s4.isFinalized());
  EXPECT_TRUE(10 == s4_count);
  EXPECT_TRUE(7 == s4_state_count);

  return RUN_ALL_TESTS();
}
