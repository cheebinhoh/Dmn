/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-ancs-parser.cpp
 * @brief TDD-ready acceptance tests for the ANCS parser and scheduler contract.
 */

#include <gtest/gtest.h>

namespace {

// These acceptance tests define the Phase 1 contract before implementation.
// They intentionally skip until the actual ANCS parser and scheduler exist so
// the test suite remains stable and the specification remains executable as a
// TDD target.
TEST(DmnAncsParserSpec, NotificationSourceRejectsMalformedInput) {
  GTEST_SKIP() << "ANCS parser implementation pending; this is the TDD contract "
                  "for malformed Notification Source payloads.";
}

TEST(DmnAncsParserSpec, NotificationSourceAcceptsKnownValues) {
  GTEST_SKIP() << "ANCS parser implementation pending; this is the TDD contract "
                  "for valid Notification Source decoding.";
}

TEST(DmnAncsParserSpec, ReassemblesFragmentedDataSourceResponse) {
  GTEST_SKIP() << "ANCS parser implementation pending; this is the TDD contract "
                  "for Data Source response reassembly.";
}

TEST(DmnAncsParserSpec, IgnoresLateRemovedUidResponses) {
  GTEST_SKIP() << "ANCS parser implementation pending; this is the TDD contract "
                  "for stale UID removal handling.";
}

TEST(DmnAncsRequestSchedulerSpec, SerializesOutstandingRequests) {
  GTEST_SKIP() << "ANCS request scheduler implementation pending; this is the TDD "
                  "contract for a single outstanding control-point request.";
}

TEST(DmnAncsRequestSchedulerSpec, RecoversFromTimedOutRequest) {
  GTEST_SKIP() << "ANCS request scheduler implementation pending; this is the TDD "
                  "contract for timeout recovery and queue resumption.";
}

TEST(DmnBluezGattClientSpec, MapsBluezErrorsToTypedFailures) {
  GTEST_SKIP() << "BlueZ GATT adapter implementation pending; this is the TDD "
                  "contract for typed backend error mapping.";
}

TEST(DmnAncsSessionSpec, ReconnectsAfterDisconnect) {
  GTEST_SKIP() << "ANCS session implementation pending; this is the TDD contract "
                  "for disconnect and rediscovery recovery.";
}

} // namespace
