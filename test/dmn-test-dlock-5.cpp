/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-5.cpp
 * @brief Local DLock API blocking and release semantics across threads.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"
#include "dmn-proc.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {

TEST(DlockRealAcquire, SecondThreadBlocksUntilRelease) {
  dmn::Dmn_DLock dlock{"dlock-real-acquire"};

  auto first = dlock.acquireLock({.m_start = 10, .m_end = 20},
                                 {.m_request_id = "first-request",
                                  .m_lease_ticks = 1000,
                                  .m_wait = true,
                                  .m_retries_allowed = false,
                                  .m_no_wait = false});
  EXPECT_EQ(first.m_code, dmn::Dmn_DLock_ResultCode::kOk);

  std::atomic<bool> second_started{false};
  std::atomic<bool> second_done{false};
  std::atomic<bool> release_done{false};

  dmn::Dmn_Proc second_thread{
      "dlock-second-thread", [&]() {
        second_started.store(true);

        auto result = dlock.acquireLock({.m_start = 15, .m_end = 17},
                                        {.m_request_id = "second-request",
                                         .m_lease_ticks = 1000,
                                         .m_wait = true,
                                         .m_retries_allowed = false,
                                         .m_no_wait = false});
        second_done.store(true);
        EXPECT_EQ(result.m_code, dmn::Dmn_DLock_ResultCode::kOk);
      }};

  EXPECT_TRUE(second_thread.exec());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_TRUE(second_started.load());
  EXPECT_FALSE(second_done.load());

  auto release = dlock.releaseLock("first-request");
  EXPECT_EQ(release.m_code, dmn::Dmn_DLock_ResultCode::kOk);
  release_done.store(true);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(second_done.load());

  second_thread.wait();
}

TEST(DlockRealAcquire, WaitFlagRejectsImmediateConflict) {
  dmn::Dmn_DLock dlock{"dlock-no-wait"};

  auto first = dlock.acquireLock({1, 5}, {"first", 1000, true, false, false});
  EXPECT_EQ(first.m_code, dmn::Dmn_DLock_ResultCode::kOk);

  auto second = dlock.acquireLock({3, 4}, {"second", 1000, false, false, true});
  EXPECT_EQ(second.m_code, dmn::Dmn_DLock_ResultCode::kNoWait);
}

TEST(DlockRealAcquire, InvalidRangesDoNotMutateAndMissingReleaseIsReported) {
  dmn::Dmn_DLock dlock{"dlock-invalid-range"};

  const auto reversed = dlock.acquireLock({5, 2}, {.m_request_id = "bad"});
  EXPECT_EQ(reversed.m_code, dmn::Dmn_DLock_ResultCode::kInvalidRange);
  const auto negative = dlock.acquireLock({-1, 2}, {.m_request_id = "bad"});
  EXPECT_EQ(negative.m_code, dmn::Dmn_DLock_ResultCode::kInvalidRange);
  EXPECT_TRUE(dlock.currentSnapshot().m_entries.empty());

  const auto missing = dlock.releaseLock("not-present");
  EXPECT_EQ(missing.m_code, dmn::Dmn_DLock_ResultCode::kInvalidState);
}

TEST(DlockRealAcquire, AsyncAcquireCompletesAndSnapshotIsAnIndependentCopy) {
  dmn::Dmn_DLock dlock{"dlock-async"};

  auto result = dlock.acquireLockAsync(
      {10, 20}, {.m_request_id = "async-request", .m_lease_ticks = 1000});
  EXPECT_EQ(result.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  ASSERT_EQ(result.get().m_code, dmn::Dmn_DLock_ResultCode::kOk);

  auto snapshot = dlock.currentSnapshot();
  ASSERT_EQ(snapshot.m_entries.size(), 1U);
  snapshot.m_entries.clear();
  EXPECT_EQ(dlock.currentSnapshot().m_entries.size(), 1U);
}

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  const int result = RUN_ALL_TESTS();

  google::protobuf::ShutdownProtobufLibrary();

  return result;
}
