/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-dbus-output-worker-start-failure.cpp
 * @brief Verify facade rollback when the output endpoint worker cannot start.
 */

#include "dmn-dmesgnet-dbus.hpp"
#include "dmn-test-fi-dbus-support.hpp"

#include <system_error>

TEST(DmnDbusFaultInjectionTest, OutputWorkerStartFailureRollsBackFacade) {
  const auto initialNameCount{dmn_test_dbus::sessionBusNameCount()};
  const auto startupFailure{
      std::make_error_code(std::errc::resource_unavailable_try_again)};

  try {
    dmn::Dmn_DMesgDbus facade{"output-worker-start-failure",
                              dmn_test_dbus::makeConfig()};
    FAIL() << "Expected the injected output worker startup failure";
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(), startupFailure);
  }

  EXPECT_TRUE(dmn_test_dbus::waitUntil([initialNameCount] {
    return dmn_test_dbus::sessionBusNameCount() == initialNameCount;
  }));
}

int main(int argc, char **argv) { return dmn_test_fi::run(argc, argv); }
