/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-fi-dbus-support.hpp
 * @brief Shared initialization for isolated D-Bus fault-injection tests.
 *
 * Each executable initializes GoogleTest and libfiu, then runs its tests
 * against a private session bus. CTest enables only that executable's failure
 * point through fiu-run.
 */

#ifndef DMN_TEST_FI_DBUS_SUPPORT_HPP_
#define DMN_TEST_FI_DBUS_SUPPORT_HPP_

#include "dmn-test-dbus-support.hpp"

#include <fiu.h>

#include <cstdio>
#include <cstdlib>

namespace dmn_test_fi {

/**
 * @brief Initialize and run one isolated D-Bus fault-injection test program.
 *
 * Call this from the executable's `main`. CTest's `fiu-run` invocation enables
 * that executable's failure point before this function initializes libfiu.
 */
inline auto run(int argc, char **argv) -> int {
  ::testing::InitGoogleTest(&argc, argv);

  if (fiu_init(0) != 0) {
    std::fprintf(stderr, "unable to initialize libfiu\n");

    return EXIT_FAILURE;
  }

  dmn_test_dbus::PrivateBus bus;
  if (!bus.setAsSessionBus()) {
    return EXIT_FAILURE;
  }

  return RUN_ALL_TESTS();
}

} // namespace dmn_test_fi

#endif // DMN_TEST_FI_DBUS_SUPPORT_HPP_
