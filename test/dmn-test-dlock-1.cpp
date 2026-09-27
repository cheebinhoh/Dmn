/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-1.cpp
 * @brief TDD step for the canonical lock-table schema and DMesg body wiring.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"
#include "proto/dmn-dmesg.pb.h"

#include <string>

namespace {

TEST(DlockCanonicalProto, RoundTripTablePayload) {
  dmn::DMesgPb message{};
  message.set_topic("lock.domain");

  auto *body = message.mutable_body();
  auto *lock_table = body->mutable_lock_table();

  lock_table->set_domain("domain-a");
  lock_table->set_schema_version(1);
  lock_table->set_protocol_version(1);
  lock_table->set_publisher_incarnation(7);
  lock_table->set_base_table_version(12);
  lock_table->set_table_version(13);
  lock_table->set_next_sequence(123);
  lock_table->set_next_fencing_token(456);

  auto *entry = lock_table->add_entries();
  entry->set_domain("domain-a");
  entry->set_request_id("req-1");
  entry->set_session_id("sess-1");
  entry->set_owner_id("owner-1");
  entry->mutable_range()->set_start(10);
  entry->mutable_range()->set_end(20);
  entry->set_priority(3);
  entry->set_sequence(1);
  entry->set_fence(77);
  entry->set_state(dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_GRANTED);
  entry->set_terminal_reason(
      dmn::DLockTerminalReasonPb::DLOCK_TERMINAL_REASON_NONE);
  entry->set_acquire_deadline_ticks(1000);
  entry->set_lease_deadline_ticks(2000);
  entry->set_waiting(false);
  entry->set_granted(true);

  EXPECT_TRUE(body->has_lock_table());
  EXPECT_EQ(body->lock_table().domain(), "domain-a");
  EXPECT_EQ(body->lock_table().entries_size(), 1);
  EXPECT_EQ(body->lock_table().entries(0).range().start(), 10);
  EXPECT_EQ(body->lock_table().entries(0).range().end(), 20);
  EXPECT_EQ(body->lock_table().entries(0).state(),
            dmn::DLockEntryStatePb::DLOCK_ENTRY_STATE_GRANTED);

  std::string wire;
  ASSERT_TRUE(message.SerializeToString(&wire));
  dmn::DMesgPb parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  EXPECT_TRUE(parsed.body().has_lock_table());
  EXPECT_EQ(parsed.body().lock_table().table_version(), 13);
  EXPECT_EQ(parsed.body().lock_table().entries(0).owner_id(), "owner-1");
  EXPECT_EQ(parsed.body().lock_table().entries(0).range().end(), 20);
}

TEST(DlockCanonicalProto, MessageBodyCompatibility) {
  dmn::DMesgPb message{};
  message.set_topic("legacy");
  message.mutable_body()->set_message("legacy payload");

  EXPECT_FALSE(message.body().has_lock_table());
  std::string wire;
  ASSERT_TRUE(message.SerializeToString(&wire));
  dmn::DMesgPb parsed;
  ASSERT_TRUE(parsed.ParseFromString(wire));
  EXPECT_FALSE(parsed.body().has_lock_table());
  EXPECT_EQ(parsed.body().message(), "legacy payload");
}

TEST(DlockRange, IntervalConversionPreservesEndpointsAndValidity) {
  const dmn::Dmn_IntervalRange interval{-1, 7};
  const auto converted = dmn::Dmn_DLock_Range::fromInterval(interval);
  EXPECT_EQ(converted.m_start, -1);
  EXPECT_EQ(converted.m_end, 7);
  EXPECT_FALSE(converted.isValid());

  const dmn::Dmn_DLock_Range lockRange{4, 9};
  const auto roundTrip = static_cast<dmn::Dmn_IntervalRange>(lockRange);
  EXPECT_EQ(roundTrip.m_start, lockRange.m_start);
  EXPECT_EQ(roundTrip.m_end, lockRange.m_end);
  EXPECT_TRUE(lockRange.overlaps({9, 12}));
  EXPECT_FALSE(lockRange.overlaps({10, 12}));
  EXPECT_FALSE(lockRange.overlaps({-1, 2}));
}

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  const int result = RUN_ALL_TESTS();

  google::protobuf::ShutdownProtobufLibrary();

  return result;
}
