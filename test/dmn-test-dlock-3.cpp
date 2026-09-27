/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-3.cpp
 * @brief Snapshot helpers and local state validation for the DLock prototype.
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

TEST(DlockLocalMirror, SnapshotRejectsInvalidDomainAndEntryData) {
  dmn::Dmn_DLock_TableSnapshot table{};
  EXPECT_FALSE(table.validate());

  table.m_domain = "domain-a";
  dmn::Dmn_DLock_Entry entry{};
  entry.m_domain = "domain-a";
  entry.m_request_id = "request";
  entry.m_session_id = "session";
  entry.m_range = {-1, 2};
  table.m_entries.push_back(entry);
  EXPECT_FALSE(table.validate());

  table.m_entries.front().m_range = {4, 2};
  EXPECT_FALSE(table.validate());

  table.m_entries.front().m_range = {1, 2};
  table.m_entries.front().m_request_id.clear();
  table.m_entries.front().m_session_id.clear();
  EXPECT_FALSE(table.validate());
}

TEST(DlockLocalMirror, SnapshotRejectsEntriesFromAnotherDomain) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";
  dmn::Dmn_DLock_Entry entry{};
  entry.m_domain = "domain-b";
  entry.m_request_id = "request";
  entry.m_session_id = "session";
  entry.m_range = {1, 2};
  table.m_entries.push_back(entry);

  EXPECT_FALSE(table.validate());
}

TEST(DlockLocalMirror, CandidateClassificationCoversInvalidWaitingAndGranted) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";
  table.m_table_version = 3;

  dmn::Dmn_DLock_Entry grant{};
  grant.m_domain = "domain-a";
  grant.m_request_id = "grant";
  grant.m_session_id = "session-grant";
  grant.m_owner_id = "owner";
  grant.m_range = {10, 20};
  grant.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  grant.m_granted = true;
  table.m_entries.push_back(grant);

  auto candidate = grant;
  candidate.m_request_id = "candidate";
  candidate.m_session_id = "session-candidate";
  candidate.m_range = {15, 16};
  EXPECT_EQ(table.classifyGrantCandidate(candidate),
            dmn::Dmn_DLock_ResultCode::kConflict);

  candidate.m_state = dmn::Dmn_DLock_EntryState::kWaiting;
  candidate.m_granted = false;
  EXPECT_EQ(table.classifyGrantCandidate(candidate),
            dmn::Dmn_DLock_ResultCode::kOk);

  candidate.m_range = {-1, 2};
  EXPECT_EQ(table.classifyGrantCandidate(candidate),
            dmn::Dmn_DLock_ResultCode::kInvalidRange);

  candidate.m_range = {21, 25};
  const auto extended = table.withCandidate(candidate);
  EXPECT_EQ(extended.m_entries.size(), 2U);
  EXPECT_EQ(extended.m_table_version, 4U);
  EXPECT_EQ(table.m_entries.size(), 1U);
  EXPECT_EQ(table.m_table_version, 3U);
}

TEST(DlockLocalMirror, CandidateFromAnotherDomainIsRejected) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";

  dmn::Dmn_DLock_Entry candidate{};
  candidate.m_domain = "domain-b";
  candidate.m_request_id = "request";
  candidate.m_session_id = "session";
  candidate.m_range = {1, 2};
  EXPECT_EQ(table.classifyGrantCandidate(candidate),
            dmn::Dmn_DLock_ResultCode::kInvalidState);
  EXPECT_TRUE(table.withCandidate(candidate).m_entries.empty());
}

TEST(DlockLocalMirror, SnapshotCodecPreservesAllTableAndEntryFields) {
  dmn::Dmn_DLock_TableSnapshot table{};
  table.m_domain = "domain-a";
  table.m_schema_version = 2;
  table.m_protocol_version = 3;
  table.m_publisher_incarnation = 4;
  table.m_base_table_version = 5;
  table.m_table_version = 6;
  table.m_next_sequence = 7;
  table.m_next_fencing_token = 8;

  dmn::Dmn_DLock_Entry entry{};
  entry.m_domain = table.m_domain;
  entry.m_request_id = "request";
  entry.m_session_id = "session";
  entry.m_owner_id = "owner";
  entry.m_range = {10, 20};
  entry.m_priority = -3;
  entry.m_sequence = 9;
  entry.m_fence = 10;
  entry.m_state = dmn::Dmn_DLock_EntryState::kGranted;
  entry.m_terminal_reason = dmn::Dmn_DLock_TerminalReason::kNone;
  entry.m_acquire_deadline_ticks = 11;
  entry.m_lease_deadline_ticks = 12;
  entry.m_waiting = false;
  entry.m_granted = true;
  table.m_entries.push_back(entry);

  const auto proto = table.toProto();
  const auto restored = dmn::Dmn_DLock_TableSnapshot::fromProto(proto);

  EXPECT_EQ(restored.m_domain, table.m_domain);
  EXPECT_EQ(restored.m_schema_version, table.m_schema_version);
  EXPECT_EQ(restored.m_protocol_version, table.m_protocol_version);
  EXPECT_EQ(restored.m_publisher_incarnation, table.m_publisher_incarnation);
  EXPECT_EQ(restored.m_base_table_version, table.m_base_table_version);
  EXPECT_EQ(restored.m_table_version, table.m_table_version);
  EXPECT_EQ(restored.m_next_sequence, table.m_next_sequence);
  EXPECT_EQ(restored.m_next_fencing_token, table.m_next_fencing_token);
  ASSERT_EQ(restored.m_entries.size(), 1U);
  const auto &restoredEntry = restored.m_entries.front();
  EXPECT_EQ(restoredEntry.m_domain, entry.m_domain);
  EXPECT_EQ(restoredEntry.m_request_id, entry.m_request_id);
  EXPECT_EQ(restoredEntry.m_session_id, entry.m_session_id);
  EXPECT_EQ(restoredEntry.m_owner_id, entry.m_owner_id);
  EXPECT_EQ(restoredEntry.m_range.m_start, entry.m_range.m_start);
  EXPECT_EQ(restoredEntry.m_range.m_end, entry.m_range.m_end);
  EXPECT_EQ(restoredEntry.m_priority, entry.m_priority);
  EXPECT_EQ(restoredEntry.m_sequence, entry.m_sequence);
  EXPECT_EQ(restoredEntry.m_fence, entry.m_fence);
  EXPECT_EQ(restoredEntry.m_state, entry.m_state);
  EXPECT_EQ(restoredEntry.m_terminal_reason, entry.m_terminal_reason);
  EXPECT_EQ(restoredEntry.m_acquire_deadline_ticks,
            entry.m_acquire_deadline_ticks);
  EXPECT_EQ(restoredEntry.m_lease_deadline_ticks,
            entry.m_lease_deadline_ticks);
  EXPECT_EQ(restoredEntry.m_waiting, entry.m_waiting);
  EXPECT_EQ(restoredEntry.m_granted, entry.m_granted);
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
