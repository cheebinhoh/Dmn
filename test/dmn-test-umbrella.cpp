/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-umbrella.cpp
 * @brief Verify that the public umbrella header exposes enabled public APIs.
 */

#include <gtest/gtest.h>

#include <type_traits>

#include "dmn.hpp"

TEST(DmnUmbrellaTest, ExposesDLockTypes) {
  const dmn::Dmn_DLock_Range range{1, 2};

  EXPECT_TRUE(range.isValid());
}

#if defined(DMN_ENABLE_DBUS)
static_assert(std::is_class_v<dmn::Dmn_DbusClient>);
#endif

#if defined(DMN_ENABLE_BLUEZ)
static_assert(std::is_class_v<dmn::Dmn_BluezClient>);
#endif
