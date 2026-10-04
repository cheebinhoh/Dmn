/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-socket.cpp
 * @brief Unit test for Dmn_Socket verifying socket-based message send and
 * receive.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>

#include "dmn-socket.hpp"

TEST(DmnSocketTest, PreservesEmptyDatagramsAndRejectsTruncation) {
  dmn::Dmn_Socket receiver{"127.0.0.1", 5000};
  dmn::Dmn_Socket sender{"127.0.0.1", 5000, true};

  sender.write(std::string(BUFSIZ + 1, 'x'));
  try {
    static_cast<void>(receiver.read());
    FAIL() << "Oversized datagram should report truncation";
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(), std::errc::message_size);
  }

  sender.write(std::string{});
  const auto empty = receiver.read();
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());

  sender.write("hello socket");
  const auto message = receiver.read();
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(*message, "hello socket");
}

TEST(DmnSocketTest, RejectsInvalidIPv4AddressAndPort) {
  EXPECT_THROW((dmn::Dmn_Socket{"not-an-ip", 5000}), std::invalid_argument);
  EXPECT_THROW((dmn::Dmn_Socket{"127.0.0.1", -1}), std::invalid_argument);
  EXPECT_THROW((dmn::Dmn_Socket{"127.0.0.1", 65536}), std::invalid_argument);
  EXPECT_THROW((dmn::Dmn_Socket{"127.0.0.1", 0, true}), std::invalid_argument);
  EXPECT_THROW((dmn::Dmn_Socket{"", 5000, true}), std::invalid_argument);
}

TEST(DmnSocketTest, AllowsWildcardBindToEphemeralPort) {
  EXPECT_NO_THROW((dmn::Dmn_Socket{"", 0}));
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
