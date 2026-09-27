/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-5.cpp
 * @brief Real DLock acquisition blocking and release semantics across threads.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"
#include "dmn-proc.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

TEST(DlockRealAcquire, SecondThreadBlocksUntilRelease) {
  dmn::Dmn_DLock dlock{"dlock-real-acquire"};

  auto first =
      dlock.acquireLock({.m_start = 10, .m_end = 20}, {.m_request_id = "first-request", .m_lease_ticks = 1000, .m_wait = true, .m_retries_allowed = false, .m_no_wait = false});
  EXPECT_EQ(first.m_code, dmn::Dmn_DLock_ResultCode::kOk);

  std::atomic<bool> second_started{false};
  std::atomic<bool> second_done{false};
  std::atomic<bool> release_done{false};

  dmn::Dmn_Proc second_thread{
      "dlock-second-thread", [&]() {
        second_started.store(true);
        auto result = dlock.acquireLock(
            {.m_start = 15, .m_end = 17}, {.m_request_id = "second-request", .m_lease_ticks = 1000, .m_wait = true, .m_retries_allowed = false, .m_no_wait = false});
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

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  google::protobuf::ShutdownProtobufLibrary();

  return result;
}
