/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-timer-reschedule-failure.cpp
 * @brief Fault-injection test for recurring timer tick scheduling failure.
 */

#include <gtest/gtest.h>

#include <fiu.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "dmn-timer.hpp"

TEST(DmnTimerFaultInjection, RescheduleFailurePausesAndCanBeReported) {
  ASSERT_EQ(fiu_init(0), 0);

  std::atomic_int callback_count{};
  dmn::Dmn_Timer timer{std::chrono::milliseconds{10}, [&callback_count]() {
                         callback_count.fetch_add(1, std::memory_order_release);
                       }};

  const auto failure_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool failure_observed{};
  while (std::chrono::steady_clock::now() < failure_deadline) {
    try {
      timer.rethrowFailure();
    } catch (const std::runtime_error &error) {
      EXPECT_STREQ(error.what(), "injected timer tick rescheduling failure");
      failure_observed = true;
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  ASSERT_TRUE(failure_observed);
  ASSERT_EQ(callback_count.load(std::memory_order_acquire), 1);
  EXPECT_THROW(timer.resume(), std::runtime_error);

  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_EQ(callback_count.load(std::memory_order_acquire), 1);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
