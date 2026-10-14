/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-async.cpp
 * @brief The unit test for dmn-async module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/time.h>

#include "dmn-async.hpp"
#include "dmn-proc.hpp"

class Counter : dmn::Dmn_Async<> {
public:
  Counter() : dmn::Dmn_Async<>{"counter"} {}

  void increment() {
    DMN_ASYNC_CALL_WITH_REF_CAPTURE({ this->m_count++; });
  }

  explicit operator long long() const { return m_count; }

private:
  long long m_count{};
};

static void timer_handler([[maybe_unused]] int sig) { EXPECT_TRUE(false); }

TEST(DmnAsync, DelayedTaskRunsNoEarlierThanDeadline) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kDelay = std::chrono::milliseconds{250};
  dmn::Dmn_Async async{"delayed-deadline"};
  Clock::time_point executed_at{};
  const auto earliest_execution = Clock::now() + kDelay;

  auto handle = async.addExecTaskAfterWithWait(
      kDelay, [&executed_at]() { executed_at = Clock::now(); });
  handle->wait();

  EXPECT_GE(executed_at, earliest_execution);
}

TEST(DmnAsync, FutureTasksReturnValuesAndPropagateExceptions) {
  dmn::Dmn_Async async{"future-results"};

  auto result = async.addExecTaskFuture(
      [](int left, int right) { return left + right; }, 20, 22);
  EXPECT_EQ(result.get(), 42);

  std::string argument{"owned"};
  auto ownedArgument = async.addExecTaskFuture(
      [](std::string value) { return value; }, argument);
  argument = "changed";
  EXPECT_EQ(ownedArgument.get(), "owned");

  bool voidTaskRan{};
  auto voidResult =
      async.addExecTaskFuture([&voidTaskRan]() { voidTaskRan = true; });
  EXPECT_NO_THROW(voidResult.get());
  EXPECT_TRUE(voidTaskRan);

  auto failure = async.addExecTaskFuture(
      []() -> int { throw std::runtime_error{"future task failure"}; });
  EXPECT_THROW(
      {
        try {
          static_cast<void>(failure.get());
        } catch (const std::runtime_error &error) {
          EXPECT_STREQ(error.what(), "future task failure");
          throw;
        }
      },
      std::runtime_error);

  std::vector<int> executionOrder;
  auto first = async.addExecTaskFuture([&executionOrder]() {
    executionOrder.push_back(1);
    return 1;
  });
  auto second = async.addExecTaskFuture([&executionOrder]() {
    executionOrder.push_back(2);
    return 2;
  });
  EXPECT_EQ(first.get(), 1);
  EXPECT_EQ(second.get(), 2);
  EXPECT_EQ(executionOrder, (std::vector<int>{1, 2}));

  EXPECT_EQ(async.addExecTaskFuture([]() { return 7; }).get(), 7);
}

TEST(DmnAsync, DelayedFutureRespectsDeadlineAndValidatesDuration) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kDelay = std::chrono::milliseconds{30};
  dmn::Dmn_Async async{"delayed-future"};
  const auto earliestExecution = Clock::now() + kDelay;
  Clock::time_point executedAt{};

  auto result = async.addExecTaskAfterFuture(kDelay, [&executedAt]() {
    executedAt = Clock::now();
    return 17;
  });
  EXPECT_EQ(result.get(), 17);
  EXPECT_GE(executedAt, earliestExecution);

  EXPECT_EQ(async
                .addExecTaskAfterFuture(std::chrono::milliseconds{0},
                                        []() { return 21; })
                .get(),
            21);

  auto failure =
      async.addExecTaskAfterFuture(std::chrono::milliseconds{1}, []() -> int {
        throw std::runtime_error{"delayed future failure"};
      });
  EXPECT_THROW(
      {
        try {
          static_cast<void>(failure.get());
        } catch (const std::runtime_error &error) {
          EXPECT_STREQ(error.what(), "delayed future failure");
          throw;
        }
      },
      std::runtime_error);

  EXPECT_THROW(
      async.addExecTaskAfterFuture(std::chrono::milliseconds{-1}, []() {}),
      std::invalid_argument);

  using ClockTickDuration = std::chrono::duration<long double, Clock::period>;
  const auto firstUnrepresentableTick =
      std::ldexp(1.0L, std::numeric_limits<Clock::duration::rep>::digits);
  EXPECT_THROW(async.addExecTaskAfterFuture(
                   ClockTickDuration{firstUnrepresentableTick}, []() {}),
               std::overflow_error);

  using ClockRep = Clock::duration::rep;
  if (std::numeric_limits<long double>::digits >=
          std::numeric_limits<ClockRep>::digits &&
      Clock::now().time_since_epoch().count() > 0) {
    const auto exclusiveMax =
        std::ldexp(1.0L, std::numeric_limits<ClockRep>::digits);
    const auto nowTicks =
        static_cast<long double>(Clock::now().time_since_epoch().count());
    const ClockTickDuration overflowingDeadline{exclusiveMax - nowTicks + 1.0L};
    EXPECT_THROW(async.addExecTaskAfterFuture(overflowingDeadline, []() {}),
                 std::overflow_error);
  }
}

TEST(DmnAsync, DiscardingFutureDoesNotCancelAcceptedTask) {
  dmn::Dmn_Async async{"discarded-future"};
  std::promise<void> taskStarted;
  std::promise<void> releaseTask;
  auto startedSignal = taskStarted.get_future();
  auto releaseSignal = releaseTask.get_future().share();
  bool submittedTaskRan{};

  async.addExecTask([&taskStarted, releaseSignal = std::move(releaseSignal)] {
    taskStarted.set_value();
    releaseSignal.wait();
  });
  startedSignal.wait();

  {
    auto abandoned = async.addExecTaskFuture(
        [&submittedTaskRan]() { submittedTaskRan = true; });
    static_cast<void>(abandoned);
  }

  releaseTask.set_value();
  async.waitForEmpty();
  EXPECT_TRUE(submittedTaskRan);
}

TEST(DmnAsync, AcceptedFutureCompletesWhenAsyncIsDestroyed) {
  std::future<int> result;
  {
    dmn::Dmn_Async async{"future-destruction"};
    result = async.addExecTaskFuture([]() {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
      return 23;
    });
  }

  EXPECT_EQ(result.get(), 23);
}

TEST(DmnAsync, DelayedFutureCannotRunBeforeDeadline) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kDelay = std::chrono::milliseconds{30};
  dmn::Dmn_Async async{"delayed-future-deadline"};
  std::promise<void> taskStarted;
  std::promise<void> releaseTask;
  auto startedSignal = taskStarted.get_future();
  auto releaseSignal = releaseTask.get_future().share();
  Clock::time_point executedAt{};

  async.addExecTask([&taskStarted, releaseSignal = std::move(releaseSignal)] {
    taskStarted.set_value();
    releaseSignal.wait();
  });
  startedSignal.wait();

  const auto deadline = Clock::now() + kDelay;
  auto completion = async.addExecTaskAfterFuture(
      kDelay, [&executedAt]() { executedAt = Clock::now(); });
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds{1}),
            std::future_status::timeout);
  std::this_thread::sleep_until(deadline);
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds{1}),
            std::future_status::timeout);

  releaseTask.set_value();
  completion.get();
  EXPECT_GE(executedAt, deadline);
}

TEST(DmnAsync, NegativeDelayedDurationsAreRejected) {
  dmn::Dmn_Async async{"negative-delay"};
  auto callback = []() {};

  EXPECT_THROW(async.addExecTaskAfter(std::chrono::milliseconds{-1}, callback),
               std::invalid_argument);
  EXPECT_THROW(
      async.addExecTaskAfterWithWait(std::chrono::milliseconds{-1}, callback),
      std::invalid_argument);
}

TEST(DmnAsync, UnrepresentableDelayedDurationIsRejectedBeforeEnqueue) {
  using Clock = std::chrono::steady_clock;
  using ClockRep = Clock::duration::rep;
  using ClockTickDuration = std::chrono::duration<long double, Clock::period>;
  const auto first_unrepresentable_tick =
      std::ldexp(1.0L, std::numeric_limits<ClockRep>::digits);
  dmn::Dmn_Async async{"overflow-delay"};
  bool callback_run{};

  EXPECT_THROW(async.addExecTaskAfterWithWait(
                   ClockTickDuration{first_unrepresentable_tick},
                   [&callback_run]() { callback_run = true; }),
               std::overflow_error);
  EXPECT_FALSE(callback_run);

  EXPECT_THROW(
      async.addExecTaskAfter(ClockTickDuration{first_unrepresentable_tick},
                             [&callback_run]() { callback_run = true; }),
      std::overflow_error);
  EXPECT_FALSE(callback_run);
}

TEST(DmnAsync, DelayedDeadlineOverflowIsRejectedBeforeEnqueue) {
  using Clock = std::chrono::steady_clock;
  using ClockRep = Clock::duration::rep;
  using ClockTickDuration = std::chrono::duration<long double, Clock::period>;
  if (std::numeric_limits<long double>::digits <
          std::numeric_limits<ClockRep>::digits ||
      Clock::now().time_since_epoch().count() <= 0) {
    GTEST_SKIP() << "steady_clock deadline overflow is not representable "
                    "for this platform";
  }

  const auto exclusive_max =
      std::ldexp(1.0L, std::numeric_limits<ClockRep>::digits);
  const auto now_ticks =
      static_cast<long double>(Clock::now().time_since_epoch().count());
  const ClockTickDuration delay{exclusive_max - now_ticks + 1.0L};
  dmn::Dmn_Async async{"overflow-deadline"};
  bool callback_run{};

  EXPECT_THROW(async.addExecTaskAfterWithWait(
                   delay, [&callback_run]() { callback_run = true; }),
               std::overflow_error);
  EXPECT_FALSE(callback_run);
}

TEST(DmnAsync, DelayedTaskDoesNotBusyPollWhileWaiting) {
  using Clock = std::chrono::steady_clock;
  constexpr auto kDelay = std::chrono::milliseconds{600};
  dmn::Dmn_Async async{"delayed-idle-cpu"};
  Clock::time_point executed_at{};
  const auto earliest_execution = Clock::now() + kDelay;
  const auto wall_start = Clock::now();
  const auto cpu_start = std::clock();
  auto handle = async.addExecTaskAfterWithWait(
      kDelay, [&executed_at]() { executed_at = Clock::now(); });
  handle->wait();

  const auto cpu_end = std::clock();
  const auto wall_elapsed = Clock::now() - wall_start;
  const auto wall_seconds = std::chrono::duration<double>(wall_elapsed).count();

  EXPECT_GE(executed_at, earliest_execution);
  EXPECT_GE(wall_elapsed, kDelay);
  ASSERT_NE(cpu_start, static_cast<std::clock_t>(-1));
  ASSERT_NE(cpu_end, static_cast<std::clock_t>(-1));
  const auto cpu_seconds = static_cast<double>(cpu_end - cpu_start) /
                           static_cast<double>(CLOCKS_PER_SEC);
  EXPECT_LT(cpu_seconds, wall_seconds / 2.0);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  Counter cnt{};
  dmn::Dmn_Proc proc1{"proc1", [&cnt]() -> void {
                        for (int ind = 0; ind < 100; ind++) {
                          cnt.increment();
                          dmn::Dmn_Proc::yield();
                        }
                      }};

  dmn::Dmn_Proc proc2{"proc2", [&cnt]() -> void {
                        for (int ind = 0; ind < 100; ind++) {
                          cnt.increment();
                          dmn::Dmn_Proc::yield();
                        }
                      }};

  proc1.exec();
  proc2.exec();
  std::this_thread::sleep_for(std::chrono::seconds(5));

  EXPECT_TRUE(static_cast<long long>(cnt) == 200);

  dmn::Dmn_Async async{"timer"};
  int val = 1;
  std::chrono::steady_clock::time_point executed_at{};
  const auto due_time =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{250};
  async.addExecTaskAfter(std::chrono::milliseconds{250}, [&]() -> void {
    val = 2;
    executed_at = std::chrono::steady_clock::now();
  });

  async.waitForEmpty();
  EXPECT_GE(executed_at, due_time);
  EXPECT_TRUE(2 == val);

  bool done{};
  dmn::Dmn_Async asyncWithWait{"async"};

  auto waitHandler = asyncWithWait.addExecTaskWithWait([&done]() -> void {
    std::this_thread::sleep_for(std::chrono::seconds(5));
    done = true;
  });

  waitHandler->wait();
  EXPECT_TRUE(done);

  struct sigaction sact {};
  struct itimerval timer {};

  // Install timer_handler as the signal handler for SIGALRM
  sact.sa_handler = &timer_handler;
  sigaction(SIGALRM, &sact, nullptr);

  // Configure the timer to expire after 1 second...
  timer.it_value.tv_sec = 10;
  timer.it_value.tv_usec = 0;

  // ... and every 1 second after that
  timer.it_interval.tv_sec = 1;
  timer.it_interval.tv_usec = 0;

  setitimer(ITIMER_REAL, &timer, nullptr);

  waitHandler = asyncWithWait.addExecTaskWithWait([]() -> void {
    std::this_thread::sleep_for(std::chrono::seconds(5));

    throw std::runtime_error("just exception");
  });

  bool exceptionCatch{};
  try {
    waitHandler->wait();
  } catch (...) {
    exceptionCatch = true;
  }

  EXPECT_TRUE(exceptionCatch);

  return RUN_ALL_TESTS();
}
