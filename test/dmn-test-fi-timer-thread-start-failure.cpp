/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-timer-thread-start-failure.cpp
 * @brief Fault-injection test for timer worker startup failure.
 */

#include <gtest/gtest.h>

#include <fiu.h>

#include <chrono>
#include <stdexcept>

#include "dmn-timer.hpp"

TEST(DmnTimerFaultInjection, WorkerStartupFailurePropagatesFromConstructor) {
  ASSERT_EQ(fiu_init(0), 0);

  try {
    dmn::Dmn_Timer timer{std::chrono::milliseconds{1}, [] {}};
    FAIL() << "Expected timer construction to fail when pthread_create is "
              "injected to fail";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "failed to start Dmn_Pipe worker");
  }
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
