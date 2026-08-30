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

  dmn::Dmn_State s1{"default"};

  EXPECT_TRUE(s1);
  EXPECT_TRUE(!s1.isInitialized());
  EXPECT_TRUE(!s1.isFinalized());

  s1.runNext();
  EXPECT_TRUE(s1);
  EXPECT_TRUE(s1.isInitialized());
  EXPECT_TRUE(!s1.isFinalized());

  s1.runNext();
  EXPECT_TRUE(!s1);
  EXPECT_TRUE(s1.isInitialized());
  EXPECT_TRUE(s1.isFinalized());

  dmn::Dmn_State s2{"default2"};
  EXPECT_TRUE(!s2.isInitialized());
  EXPECT_TRUE(!s2.isFinalized());

  while (s2) {
    s2.runNext();
  }

  EXPECT_TRUE(!s2);
  EXPECT_TRUE(s2.isInitialized());
  EXPECT_TRUE(s2.isFinalized());

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

  while (s3) {
    s3.runNext();
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

  // Dmn_Proc and Dmn_Pipe will be destroyed and display statistics
  return RUN_ALL_TESTS();
}
