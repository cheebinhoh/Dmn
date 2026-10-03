/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-timer.cpp
 * @brief Unit test for Dmn_Timer verifying that the timer callback fires after
 * the configured interval.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "dmn-timer.hpp"

namespace {

auto waitForFlag(const std::atomic_bool &flag,
                 std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (!flag.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return flag.load(std::memory_order_acquire);
}

} // namespace

TEST(DmnTimer, FiresAfterConfiguredInterval) {
  std::atomic_bool timer_run{};
  dmn::Dmn_Timer timer{std::chrono::milliseconds(100), [&timer_run]() -> void {
                         timer_run.store(true, std::memory_order_release);
                       }};

  EXPECT_FALSE(timer_run.load(std::memory_order_acquire));
  EXPECT_TRUE(waitForFlag(timer_run, std::chrono::seconds(2)));
}

TEST(DmnTimer, RestartStopsOldWorkerBeforeReplacingTimerSettings) {
  std::atomic_bool old_callback_run{};
  std::atomic_bool new_callback_run{};
  dmn::Dmn_Timer timer{
      std::chrono::milliseconds(10000), [&old_callback_run]() -> void {
        old_callback_run.store(true, std::memory_order_release);
      }};

  timer.start(std::chrono::milliseconds(10), [&new_callback_run]() -> void {
    new_callback_run.store(true, std::memory_order_release);
  });

  EXPECT_TRUE(waitForFlag(new_callback_run, std::chrono::seconds(2)));
  EXPECT_FALSE(old_callback_run.load(std::memory_order_acquire));
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
