/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-4.cpp
 * @brief Multi-thread blocked acquisition and release semantics.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"
#include "dmn-proc.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace {

TEST(DlockBlockingAcquire, OverlappingGrantBlocksCandidateUntilOwnerReleases) {
  dmn::Dmn_DLock_TableSnapshot snapshot{};
  snapshot.m_domain = "domain-a";
  snapshot.m_table_version = 1;

  dmn::Dmn_DLock_Entry grant{};
  grant.m_domain = "domain-a";
  grant.m_request_id = "grant-req";
  grant.m_session_id = "sess-grant";
  grant.m_owner_id = "owner";
  grant.m_range = {10, 20};
  grant.m_priority = 1;
  grant.m_sequence = 1;
  grant.m_fence = 1;
  grant.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  grant.m_granted = true;
  snapshot.m_entries.push_back(grant);

  dmn::Dmn_DLock_Entry candidate{};
  candidate.m_domain = "domain-a";
  candidate.m_request_id = "candidate-req";
  candidate.m_session_id = "sess-candidate";
  candidate.m_owner_id = "candidate";
  candidate.m_range = {15, 17};
  candidate.m_priority = 2;
  candidate.m_sequence = 2;
  candidate.m_fence = 2;
  candidate.m_state = dmn::Dmn_DLock_EntryState::kWaiting;
  candidate.m_granted = true;
  candidate.m_waiting = true;

  EXPECT_FALSE(snapshot.canAcceptEntry(candidate));

  std::mutex lock{};
  std::atomic<bool> release{false};
  std::atomic<bool> waiter_started{false};
  std::atomic<bool> candidate_allowed_after_release{false};

  dmn::Dmn_Proc waiter{"waiter", [&]() {
    waiter_started.store(true);
    while (!release.load()) {
      dmn::Dmn_Proc::yield();
    }

    std::lock_guard<std::mutex> guard(lock);
    auto post_release = snapshot;
    candidate_allowed_after_release.store(post_release.canAcceptEntry(candidate));
  }};

  dmn::Dmn_Proc owner{"owner", [&]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
      std::lock_guard<std::mutex> guard(lock);
      snapshot.m_entries.clear();
    }
    release.store(true);
  }};

  EXPECT_TRUE(waiter.exec());
  EXPECT_TRUE(owner.exec());
  owner.wait();
  waiter.wait();

  EXPECT_TRUE(waiter_started.load());
  EXPECT_TRUE(candidate_allowed_after_release.load());
}

TEST(DlockBlockingAcquire, WaitingRequestIsBlockedByAnExistingGrant) {
  dmn::Dmn_DLock_TableSnapshot snapshot{};
  snapshot.m_domain = "domain-a";
  snapshot.m_table_version = 1;

  dmn::Dmn_DLock_Entry grant{};
  grant.m_domain = "domain-a";
  grant.m_request_id = "grant-req";
  grant.m_session_id = "sess-grant";
  grant.m_owner_id = "owner";
  grant.m_range = {30, 40};
  grant.m_priority = 1;
  grant.m_sequence = 1;
  grant.m_fence = 1;
  grant.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  grant.m_granted = true;
  snapshot.m_entries.push_back(grant);

  dmn::Dmn_DLock_Entry waiting{};
  waiting.m_domain = "domain-a";
  waiting.m_request_id = "waiting-req";
  waiting.m_session_id = "sess-waiter";
  waiting.m_owner_id = "waiter";
  waiting.m_range = {35, 36};
  waiting.m_priority = 2;
  waiting.m_sequence = 2;
  waiting.m_fence = 2;
  waiting.m_state = dmn::Dmn_DLock_EntryState::kWaiting;
  waiting.m_granted = true;
  waiting.m_waiting = true;

  EXPECT_FALSE(snapshot.canAcceptEntry(waiting));

  snapshot.m_entries.clear();
  EXPECT_TRUE(snapshot.canAcceptEntry(waiting));
  EXPECT_FALSE(snapshot.isGrantedForRange({35, 36}));
}

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  google::protobuf::ShutdownProtobufLibrary();
  return result;
}
