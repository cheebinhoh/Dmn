/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-pipe-scheduled.cpp
 * @brief Test deadline-scheduled writes on Dmn_Pipe.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dmn-blockingqueue-lf.hpp"
#include "dmn-pipe.hpp"

namespace {

using Pipe = dmn::Dmn_Pipe<int>;
using PipeLf = dmn::Dmn_Pipe<int, dmn::Dmn_BlockingQueue_Lf<int>>;
using Clock = std::chrono::steady_clock;

constexpr bool kEnableScheduledWrites = true;

template <typename PipeType> void expectShutdownDrainsScheduledWrite() {
  std::atomic_bool processed{};
  Clock::time_point processed_at{};
  const auto deadline = Clock::now() + std::chrono::milliseconds(60);

  {
    PipeType pipe{"scheduled",
                  [&processed, &processed_at](int) {
                    processed_at = Clock::now();
                    processed.store(true, std::memory_order_release);
                  },
                  1, 0, kEnableScheduledWrites};
    pipe.writeAt(deadline, 1);
  }

  EXPECT_TRUE(processed.load(std::memory_order_acquire));
  EXPECT_GE(processed_at, deadline);
}

} // namespace

TEST(DmnPipeScheduledTest, RunsScheduledItemNoEarlierThanDeadline) {
  std::mutex mutex;
  Clock::time_point processed_at{};
  const auto deadline = Clock::now() + std::chrono::milliseconds(60);

  Pipe pipe{"scheduled",
            [&mutex, &processed_at](int) {
              std::lock_guard<std::mutex> lock(mutex);
              processed_at = Clock::now();
            },
            1, 0, kEnableScheduledWrites};

  pipe.writeAt(deadline, 1);
  EXPECT_EQ(pipe.waitForEmpty(), 1);

  std::lock_guard<std::mutex> lock(mutex);
  EXPECT_GE(processed_at, deadline);
}

TEST(DmnPipeScheduledTest, EarlierScheduledWriteWakesWorkerForNewDeadline) {
  std::mutex mutex;
  std::vector<int> processed;
  Clock::time_point earlier_processed_at{};
  const auto later_deadline = Clock::now() + std::chrono::milliseconds(600);

  Pipe pipe{"scheduled",
            [&mutex, &processed, &earlier_processed_at](int item) {
              std::lock_guard<std::mutex> lock(mutex);
              processed.push_back(item);
              if (item == 2) {
                earlier_processed_at = Clock::now();
              }
            },
            1, 0, kEnableScheduledWrites};

  pipe.writeAt(later_deadline, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  const auto earlier_deadline = Clock::now() + std::chrono::milliseconds(120);
  pipe.writeAt(earlier_deadline, 2);

  EXPECT_EQ(pipe.waitForEmpty(), 2);

  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(processed.size(), 2);
  EXPECT_EQ(processed[0], 2);
  EXPECT_EQ(processed[1], 1);
  EXPECT_GE(earlier_processed_at, earlier_deadline);
  EXPECT_LT(earlier_processed_at, later_deadline);
}

TEST(DmnPipeScheduledTest, ImmediateWritesRunBeforeFutureScheduledWrites) {
  std::mutex mutex;
  std::vector<int> processed;
  Clock::time_point immediate_processed_at{};
  const auto scheduled_deadline = Clock::now() + std::chrono::milliseconds(300);

  Pipe pipe{"scheduled",
            [&mutex, &processed, &immediate_processed_at](int item) {
              std::lock_guard<std::mutex> lock(mutex);
              processed.push_back(item);
              if (item == 1) {
                immediate_processed_at = Clock::now();
              }
            },
            1, 0, kEnableScheduledWrites};

  pipe.writeAt(scheduled_deadline, 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  pipe.write(1);

  EXPECT_EQ(pipe.waitForEmpty(), 2);

  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(processed.size(), 2);
  EXPECT_EQ(processed[0], 1);
  EXPECT_EQ(processed[1], 2);
  EXPECT_LT(immediate_processed_at, scheduled_deadline);
}

TEST(DmnPipeScheduledTest, EqualDeadlinesRetainSubmissionOrder) {
  std::mutex mutex;
  std::vector<int> processed;
  const auto deadline = Clock::now() + std::chrono::milliseconds(40);

  Pipe pipe{"scheduled",
            [&mutex, &processed](int item) {
              std::lock_guard<std::mutex> lock(mutex);
              processed.push_back(item);
            },
            1, 0, kEnableScheduledWrites};

  pipe.writeAt(deadline, 1);
  pipe.writeAt(deadline, 2);
  EXPECT_EQ(pipe.waitForEmpty(), 2);

  std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(processed.size(), 2);
  EXPECT_EQ(processed[0], 1);
  EXPECT_EQ(processed[1], 2);
}

TEST(DmnPipeScheduledTest, ShutdownDrainsScheduledWritesAtTheirDeadlines) {
  expectShutdownDrainsScheduledWrite<Pipe>();
}

TEST(DmnPipeScheduledTest,
     ShutdownDrainsScheduledWritesAtTheirDeadlinesWithLockFreeQueue) {
  expectShutdownDrainsScheduledWrite<PipeLf>();
}

TEST(DmnPipeScheduledTest, WriteAtRequiresScheduledMode) {
  Pipe pipe{"ordinary", [](int) {}};

  EXPECT_THROW(pipe.writeAt(Clock::now(), 1), std::logic_error);
}

TEST(DmnPipeScheduledTest, ScheduledModeRequiresProcessingTask) {
  EXPECT_THROW((Pipe{"scheduled", std::function<void(int &&)>{}, 1, 0,
                     kEnableScheduledWrites}),
               std::invalid_argument);
}

TEST(DmnPipeScheduledTest, WorksWithLockFreeUnderlyingQueue) {
  std::atomic_int processed{};
  using LockFreePipe = dmn::Dmn_Pipe<int, dmn::Dmn_BlockingQueue_Lf<int>>;

  LockFreePipe pipe{
      "scheduled",
      [&processed](int) { processed.fetch_add(1, std::memory_order_relaxed); },
      1, 0, kEnableScheduledWrites};
  pipe.writeAt(Clock::now() + std::chrono::milliseconds(30), 1);

  EXPECT_EQ(pipe.waitForEmpty(), 1);
  EXPECT_EQ(processed.load(std::memory_order_relaxed), 1);
}

TEST(DmnPipeScheduledTest, OrdinaryWorkerFailureIsRethrownByWaitForEmpty) {
  Pipe pipe{"ordinary-failure",
            [](int) { throw std::runtime_error("ordinary callback failed"); }};
  pipe.write(1);

  try {
    static_cast<void>(pipe.waitForEmpty());
    FAIL() << "waitForEmpty should rethrow the callback failure";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "ordinary callback failed");
  }
}

TEST(DmnPipeScheduledTest, ScheduledWorkerFailureIsRethrownByWaitForEmpty) {
  Pipe pipe{"scheduled-failure",
            [](int) { throw std::runtime_error("scheduled callback failed"); },
            1, 0, kEnableScheduledWrites};
  pipe.writeAt(Clock::now(), 1);

  try {
    static_cast<void>(pipe.waitForEmpty());
    FAIL() << "waitForEmpty should rethrow the callback failure";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "scheduled callback failed");
  }
}

TEST(DmnPipeScheduledTest,
     SynchronousProcessingFailureIsRethrownByWaitForEmpty) {
  Pipe pipe{"synchronous-failure"};
  pipe.write(1);

  EXPECT_THROW(pipe.readAndProcess([](int) {
    throw std::runtime_error("synchronous callback failed");
  }),
               std::runtime_error);
  try {
    static_cast<void>(pipe.waitForEmpty());
    FAIL() << "waitForEmpty should rethrow the callback failure";
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "synchronous callback failed");
  }
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
