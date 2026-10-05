/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-proc-thread-start-failure.cpp
 * @brief Fault-injection test for Dmn_Proc worker startup failure.
 */

#include <gtest/gtest.h>

#include <fiu.h>

#include <atomic>

#include "dmn-proc.hpp"

TEST(DmnProcFaultInjection, WorkerStartupFailureDoesNotRunTask) {
  ASSERT_EQ(fiu_init(0), 0);

  std::atomic<bool> taskRan{};
  dmn::Dmn_Proc proc{"injected-startup-failure",
                     [&taskRan] { taskRan.store(true); }};

  EXPECT_FALSE(proc.exec());
  EXPECT_FALSE(taskRan.load());
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
