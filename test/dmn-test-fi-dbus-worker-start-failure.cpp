/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-dbus-worker-start-failure.cpp
 * @brief Fault-injection coverage for D-Bus worker startup rollback.
 */

#include <gtest/gtest.h>

#include <fiu.h>

#include <system_error>

#include "dmn-dbus-io.hpp"
#include "dmn-test-dbus-support.hpp"

TEST(DmnDbusWorkerFaultInjection, StartupFailureReleasesEndpointConnection) {
  ASSERT_EQ(fiu_init(0), 0);

  dmn_test_dbus::PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  ASSERT_TRUE(bus.setAsSessionBus());

  const auto initialNameCount{dmn_test_dbus::sessionBusNameCount()};
  const auto startupFailure{
      std::make_error_code(std::errc::resource_unavailable_try_again)};

  try {
    dmn::Dmn_DbusInput input{dmn_test_dbus::makeConfig()};
    FAIL() << "Expected injected input worker startup failure";
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(), startupFailure);
  }

  ASSERT_TRUE(dmn_test_dbus::waitUntil([initialNameCount] {
    return dmn_test_dbus::sessionBusNameCount() == initialNameCount;
  }));

  try {
    dmn::Dmn_DbusOutput output{dmn_test_dbus::makeConfig()};
    FAIL() << "Expected injected output worker startup failure";
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(), startupFailure);
  }

  EXPECT_TRUE(dmn_test_dbus::waitUntil([initialNameCount] {
    return dmn_test_dbus::sessionBusNameCount() == initialNameCount;
  }));
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
