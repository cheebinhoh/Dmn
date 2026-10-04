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
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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

auto waitForCount(const std::atomic_int &count, int expected,
                  std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (count.load(std::memory_order_acquire) < expected &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return count.load(std::memory_order_acquire) >= expected;
}

template <typename Timer>
auto waitForFailure(const Timer &timer,
                    std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (std::chrono::steady_clock::now() < deadline) {
    try {
      timer.rethrowFailure();
    } catch (...) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  return false;
}

} // namespace

TEST(DmnTimer, FiresAfterConfiguredInterval) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kInterval = std::chrono::milliseconds(100);
  std::atomic_bool timer_run{};
  Clock::time_point callback_at{};
  const auto earliest_callback = Clock::now() + kInterval;
  dmn::Dmn_Timer timer{kInterval, [&]() {
                         callback_at = Clock::now();
                         timer_run.store(true, std::memory_order_release);
                       }};

  EXPECT_FALSE(timer_run.load(std::memory_order_acquire));
  EXPECT_TRUE(waitForFlag(timer_run, std::chrono::seconds(2)));
  EXPECT_GE(callback_at, earliest_callback);
}

TEST(DmnTimer, RestartDiscardsPreviouslyScheduledTick) {
  std::atomic_bool old_callback_run{};
  std::atomic_bool new_callback_run{};
  dmn::Dmn_Timer timer{
      std::chrono::milliseconds(250), [&old_callback_run]() -> void {
        old_callback_run.store(true, std::memory_order_release);
      }};

  timer.start(std::chrono::milliseconds(10), [&new_callback_run]() -> void {
    new_callback_run.store(true, std::memory_order_release);
  });

  EXPECT_TRUE(waitForFlag(new_callback_run, std::chrono::seconds(2)));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(old_callback_run.load(std::memory_order_acquire));
}

TEST(DmnTimer, StopDiscardsPendingTickUntilResumed) {
  std::atomic_int callback_count{};
  dmn::Dmn_Timer timer{std::chrono::milliseconds(80), [&]() {
                         callback_count.fetch_add(1, std::memory_order_release);
                       }};

  timer.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  EXPECT_EQ(callback_count.load(std::memory_order_acquire), 0);
}

TEST(DmnTimer, RejectsNonPositiveIntervals) {
  EXPECT_THROW((dmn::Dmn_Timer<std::chrono::milliseconds>{
                   std::chrono::milliseconds::zero(), []() {}}),
               std::invalid_argument);
  EXPECT_THROW((dmn::Dmn_Timer<std::chrono::milliseconds>{
                   std::chrono::milliseconds(-1), []() {}}),
               std::invalid_argument);
}

TEST(DmnTimer, InvalidRestartLeavesRunningTimerUnchanged) {
  std::atomic_bool old_callback_run{};
  std::atomic_bool replacement_callback_run{};
  dmn::Dmn_Timer timer{std::chrono::milliseconds(80), [&old_callback_run]() {
                         old_callback_run.store(true,
                                                std::memory_order_release);
                       }};

  EXPECT_THROW(timer.start(std::chrono::milliseconds::zero(),
                           [&replacement_callback_run]() {
                             replacement_callback_run.store(
                                 true, std::memory_order_release);
                           }),
               std::invalid_argument);
  EXPECT_TRUE(waitForFlag(old_callback_run, std::chrono::seconds(2)));
  EXPECT_FALSE(replacement_callback_run.load(std::memory_order_acquire));
}

TEST(DmnTimer, StopPausesAndResumeStartsANewGeneration) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kInterval = std::chrono::milliseconds(180);
  std::atomic_int callback_count{};
  std::vector<Clock::time_point> callback_times;
  std::mutex times_mutex;
  dmn::Dmn_Timer timer{kInterval, [&]() {
                         std::lock_guard<std::mutex> lock(times_mutex);
                         callback_times.push_back(Clock::now());
                         callback_count.fetch_add(1, std::memory_order_release);
                       }};

  timer.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const auto resumed_at = Clock::now();
  timer.resume();

  ASSERT_TRUE(waitForCount(callback_count, 2, std::chrono::seconds(2)));
  timer.stop();

  std::lock_guard<std::mutex> lock(times_mutex);
  ASSERT_EQ(callback_times.size(), 2);
  EXPECT_GE(callback_times.front(), resumed_at + kInterval);
  EXPECT_GE(callback_times[1] - callback_times[0], kInterval);
}

TEST(DmnTimer, StopReturnsWhileAdmittedCallbackIsRunning) {
  std::mutex mutex;
  std::condition_variable condition;
  bool callback_started{};
  bool release_callback{};
  std::atomic_bool callback_finished{};
  dmn::Dmn_Timer timer{
      std::chrono::milliseconds(20), [&]() {
        std::unique_lock<std::mutex> lock(mutex);
        callback_started = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return release_callback; });
        callback_finished.store(true, std::memory_order_release);
      }};

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2),
                                   [&]() { return callback_started; }));
  }

  timer.stop();
  EXPECT_FALSE(callback_finished.load(std::memory_order_acquire));

  {
    std::lock_guard<std::mutex> lock(mutex);
    release_callback = true;
  }

  condition.notify_all();
  EXPECT_TRUE(waitForFlag(callback_finished, std::chrono::seconds(2)));
}

TEST(DmnTimer, StandardCallbackExceptionIsLoggedAndTimerContinues) {
  std::atomic_int callback_count{};
  dmn::Dmn_Timer timer{std::chrono::milliseconds(20), [&]() {
                         const auto count = callback_count.fetch_add(
                             1, std::memory_order_acq_rel);
                         if (count == 0) {
                           throw std::runtime_error("expected test exception");
                         }
                       }};

  EXPECT_TRUE(waitForCount(callback_count, 2, std::chrono::seconds(2)));
  timer.stop();
  EXPECT_NO_THROW(timer.rethrowFailure());
}

TEST(DmnTimer, NonStandardCallbackExceptionCanBeReported) {
  std::atomic_bool callback_run{};
  dmn::Dmn_Timer timer{std::chrono::milliseconds(20), [&]() {
                         callback_run.store(true, std::memory_order_release);
                         throw 7;
                       }};

  ASSERT_TRUE(waitForFlag(callback_run, std::chrono::seconds(2)));
  EXPECT_TRUE(waitForFailure(timer, std::chrono::seconds(2)));
  EXPECT_THROW(timer.rethrowFailure(), int);
  EXPECT_THROW(timer.resume(), int);
}

TEST(DmnTimer, RejectsIntervalsOutsideSteadyClockRange) {
  using Clock = std::chrono::steady_clock;
  using ClockRep = Clock::duration::rep;
  using HugeDuration = std::chrono::duration<long double, std::ratio<3600>>;
  using ClockTickDuration = std::chrono::duration<long double, Clock::period>;
  const auto first_unrepresentable_tick =
      std::ldexp(1.0L, std::numeric_limits<ClockRep>::digits);

  EXPECT_THROW(
      (dmn::Dmn_Timer<HugeDuration>{
          HugeDuration{std::numeric_limits<long double>::max()}, []() {}}),
      std::overflow_error);
  EXPECT_THROW((dmn::Dmn_Timer<ClockTickDuration>{
                   ClockTickDuration{first_unrepresentable_tick}, []() {}}),
               std::overflow_error);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
