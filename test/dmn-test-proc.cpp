/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-proc.cpp
 * @brief Unit tests for Dmn_Proc exception reporting and cancellation.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "dmn-proc.hpp"

namespace {

struct NonStandardFailure {};

class TestDmnProc : public dmn::Dmn_Proc {
public:
  using Dmn_Proc::Dmn_Proc;
  using Dmn_Proc::stopExec;
};

void runDefaultPolicyFailure() {
  dmn::Dmn_Proc proc{"legacy-terminate",
                     [] { throw std::runtime_error("legacy task failure"); }};
  proc.exec();
  proc.wait();
}

} // namespace

TEST(DmnProc, CapturesStandardTaskExceptionForJoiner) {
  dmn::Dmn_Proc proc{
      "throw-standard", [] { throw std::runtime_error("task failure"); },
      dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());
  EXPECT_THROW(proc.wait(), std::runtime_error);

  ASSERT_TRUE(proc.exec([] {}));
  EXPECT_TRUE(proc.wait());
}

TEST(DmnProc, CapturesNonStandardTaskExceptionForJoiner) {
  dmn::Dmn_Proc proc{
      "throw-non-standard", [] { throw NonStandardFailure{}; },
      dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());
  EXPECT_THROW(proc.wait(), NonStandardFailure);
}

TEST(DmnProc, StopExecPreservesDeferredCancellation) {
  TestDmnProc proc{"cancel",
                   [] {
                     while (true) {
                       dmn::Dmn_Proc::yield();
                     }
                   },
                   dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());
  EXPECT_TRUE(proc.stopExec());
}

/**
 * @brief Verify a client-owned atomic flag can stop a task before wait joins.
 *
 * Dmn_Proc does not provide a cooperative stop API; the task and its owner
 * share and synchronize the stop state.
 */
TEST(DmnProc, ClientFlagLetsTaskExitCooperativelyBeforeWait) {
  std::condition_variable startedCondition;
  std::mutex startedMutex;
  bool started{};
  std::atomic<bool> stopRequested{};
  std::atomic<bool> completed{};
  dmn::Dmn_Proc proc{
      "client-cooperative-stop",
      [&] {
        {
          std::lock_guard lock{startedMutex};
          started = true;
        }

        startedCondition.notify_one();
        while (!stopRequested.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        completed.store(true, std::memory_order_release);
      },
      dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());

  std::unique_lock lock{startedMutex};
  const bool taskStarted{startedCondition.wait_for(
      lock, std::chrono::seconds(2), [&started] { return started; })};
  lock.unlock();

  stopRequested.store(true, std::memory_order_release);

  EXPECT_TRUE(taskStarted);
  EXPECT_TRUE(proc.wait());
  EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

TEST(DmnProc, DefaultPolicyPreservesTerminateBehavior) {
  EXPECT_DEATH(runDefaultPolicyFailure(), "");
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
