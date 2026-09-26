/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-2.cpp
 * @brief Session lifetime and proxy invalidation tests for the distributed lock.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"

namespace {

TEST(DlockSessionLifetime, ProxyRemainsLiveUntilClosed) {
  dmn::Dmn_DLock dlock{"dlock-lifetime"};
  auto session = dlock.openHandler("session-a");

  EXPECT_TRUE(session);
  EXPECT_TRUE(session.operator->());

  dlock.closeHandler(session);
  EXPECT_FALSE(session);
}

TEST(DlockSessionLifetime, OneSessionOneHandler) {
  dmn::Dmn_DLock dlock{"dlock-single-session"};

  auto first = dlock.openHandler("session-a");
  auto second = dlock.openHandler("session-b");

  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_NE(first.operator->(), second.operator->());

  dlock.closeHandler(first);
  dlock.closeHandler(second);

  EXPECT_FALSE(first);
  EXPECT_FALSE(second);
}

} // namespace

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  google::protobuf::ShutdownProtobufLibrary();
  return result;
}
