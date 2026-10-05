/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-dbus-input-allocation.cpp
 * @brief Fault-injection test for D-Bus input payload allocation failure.
 */

#include "dmn-dbus-io.hpp"
#include "dmn-test-fi-dbus-support.hpp"

#include <string>

TEST(DmnDbusFaultInjectionTest, ReportsInputPayloadAllocationFailure) {
  dmn::Dmn_DbusInput input{dmn_test_dbus::makeConfig()};
  dmn::Dmn_DbusOutput output{dmn_test_dbus::makeConfig()};

  // fiu-run enables the allocation point for this executable; the callback
  // should report the failure without accepting the dropped payload.
  output.write(std::string{"allocation-failure"});
  ASSERT_TRUE(dmn_test_dbus::waitUntil(
      [&input] { return input.status().input_allocation_errors == 1; }));
  EXPECT_EQ(input.status().messages_received, 0U);
}

int main(int argc, char **argv) { return dmn_test_fi::run(argc, argv); }
