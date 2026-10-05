/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-dbus-facade-rollback.cpp
 * @brief Fault-injection test for facade endpoint-construction rollback.
 */

#include "dmn-dmesgnet-dbus.hpp"
#include "dmn-test-fi-dbus-support.hpp"

#include <new>

TEST(DmnDbusFaultInjectionTest, FacadeRollsBackInputIfOutputCreationFails) {
  const auto initialNameCount{dmn_test_dbus::sessionBusNameCount()};

  // The configured failure point throws after input creation. Construction
  // must propagate the failure and release the input connection via RAII.
  EXPECT_THROW((dmn::Dmn_DMesgDbus{"injected-output-failure",
                                   dmn_test_dbus::makeConfig()}),
               std::bad_alloc);

  // Bus name count is a process-independent check that the failed facade left
  // no input endpoint connection registered on the private bus.
  ASSERT_TRUE(dmn_test_dbus::waitUntil([initialNameCount] {
    return dmn_test_dbus::sessionBusNameCount() == initialNameCount;
  }));
}

int main(int argc, char **argv) { return dmn_test_fi::run(argc, argv); }
