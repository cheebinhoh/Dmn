/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-dbus-output-stall.cpp
 * @brief Fault-injection test for bounded shutdown of stalled D-Bus output.
 */

#include "dmn-dbus-io.hpp"
#include "dmn-test-fi-dbus-support.hpp"

#include <chrono>
#include <string>

using namespace std::chrono_literals;

TEST(DmnDbusFaultInjectionTest, StalledOutputShutdownHonorsDrainDeadline) {
  dmn::Dmn_DbusOutput output{dmn_test_dbus::makeConfig()};
  output.write(std::string{"held-until-shutdown"});

  // Wait until the test message is queued, then verify shutdown reaches its
  // finite deadline and accounts for the message that could not be sent.
  ASSERT_TRUE(dmn_test_dbus::waitUntil(
      [&output] { return output.status().pending_output_messages == 1; }));
  const auto shutdownStart{std::chrono::steady_clock::now()};
  output.shutdown();
  const auto shutdownDuration{std::chrono::steady_clock::now() - shutdownStart};

  EXPECT_GE(shutdownDuration, 900ms);
  EXPECT_LT(shutdownDuration, 2s);
  EXPECT_EQ(output.status().shutdown_unsent_messages, 1U);
}

int main(int argc, char **argv) { return dmn_test_fi::run(argc, argv); }
