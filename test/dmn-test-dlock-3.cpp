/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-3.cpp
 * @brief Local mirror and state-machine validation for the distributed lock.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"

namespace {

TEST(DlockLocalMirror, SingleGrantedEntryIsCanonicalAndValid) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";
  table.m_table_version = 1;
  table.m_base_table_version = 0;
  table.m_next_sequence = 1;
  table.m_next_fencing_token = 1;

  dmn::Dmn_DLock_Entry entry{};
  entry.m_domain = "domain-a";
  entry.m_request_id = "req-1";
  entry.m_session_id = "sess-1";
  entry.m_owner_id = "owner-1";
  entry.m_range = {10, 20};
  entry.m_priority = 5;
  entry.m_sequence = 1;
  entry.m_fence = 1;
  entry.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  entry.m_terminal_reason = dmn::Dmn_DLock_TerminalReason::kNone;
  entry.m_acquire_deadline_ticks = 100;
  entry.m_lease_deadline_ticks = 200;
  entry.m_waiting = false;
  entry.m_granted = true;
  table.m_entries.push_back(entry);

  EXPECT_TRUE(table.validate());
  auto proto = table.toProto();
  EXPECT_EQ(proto.domain(), "domain-a");
  EXPECT_EQ(proto.entries_size(), 1);
  EXPECT_EQ(proto.entries(0).range().start(), 10);
  EXPECT_EQ(proto.entries(0).range().end(), 20);

  auto round = dmn::Dmn_DLock_TableSnapshot::fromProto(proto);
  EXPECT_EQ(round.m_entries.size(), 1U);
  EXPECT_EQ(round.m_entries[0].m_request_id, "req-1");
  EXPECT_TRUE(round.validate());
}

TEST(DlockLocalMirror, OverlappingGrantedEntriesAreRejected) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";
  table.m_table_version = 1;

  dmn::Dmn_DLock_Entry one{};
  one.m_domain = "domain-a";
  one.m_request_id = "req-1";
  one.m_session_id = "sess-1";
  one.m_owner_id = "owner-1";
  one.m_range = {10, 20};
  one.m_priority = 1;
  one.m_sequence = 1;
  one.m_fence = 1;
  one.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  one.m_granted = true;

  dmn::Dmn_DLock_Entry two{};
  two.m_domain = "domain-a";
  two.m_request_id = "req-2";
  two.m_session_id = "sess-2";
  two.m_owner_id = "owner-2";
  two.m_range = {15, 25};
  two.m_priority = 2;
  two.m_sequence = 2;
  two.m_fence = 2;
  two.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  two.m_granted = true;

  table.m_entries.push_back(one);
  table.m_entries.push_back(two);

  EXPECT_FALSE(table.validate());

  auto candidate = two;
  auto result = table.withCandidate(candidate);
  EXPECT_EQ(result.m_entries.size(), 0U);
}

TEST(DlockLocalMirror, CandidateWithHigherPriorityWinsCanonicalOrder) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";
  table.m_table_version = 1;
  table.m_next_sequence = 2;

  dmn::Dmn_DLock_Entry low{};
  low.m_domain = "domain-a";
  low.m_request_id = "low";
  low.m_session_id = "s-low";
  low.m_owner_id = "o-low";
  low.m_range = {50, 60};
  low.m_priority = 1;
  low.m_sequence = 1;
  low.m_fence = 1;
  low.m_state = dmn::Dmn_DLock_EntryState::kWaiting;
  low.m_waiting = true;

  dmn::Dmn_DLock_Entry high{};
  high.m_domain = "domain-a";
  high.m_request_id = "high";
  high.m_session_id = "s-high";
  high.m_owner_id = "o-high";
  high.m_range = {50, 60};
  high.m_priority = 9;
  high.m_sequence = 2;
  high.m_fence = 2;
  high.m_state = dmn::Dmn_DLock_EntryState::kWaiting;
  high.m_waiting = true;

  table.m_entries.push_back(low);
  table.m_entries.push_back(high);
  table.canonicalize();

  ASSERT_EQ(table.m_entries[0].m_request_id, "low");
  ASSERT_EQ(table.m_entries[1].m_request_id, "high");
  EXPECT_EQ(table.m_entries[0].m_request_id, "low");
  EXPECT_EQ(table.m_entries[0].m_priority, 1);
  EXPECT_EQ(table.m_entries[0].m_sequence, 1ULL);
  EXPECT_EQ(table.m_entries[1].m_request_id, "high");
  EXPECT_EQ(table.m_entries[1].m_priority, 9);
  EXPECT_EQ(table.m_entries[1].m_sequence, 2ULL);
}

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  const int result = RUN_ALL_TESTS();

  google::protobuf::ShutdownProtobufLibrary();

  return result;
}
