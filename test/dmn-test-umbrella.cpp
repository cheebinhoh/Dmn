/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-umbrella.cpp
 * @brief Verify that the public umbrella header exposes the DLock API.
 */

#include <gtest/gtest.h>

#include "dmn-dlock.hpp"

TEST(DmnUmbrellaTest, ExposesDLockTypes) {
  const dmn::Dmn_DLock_Range range{1, 2};

  EXPECT_TRUE(range.isValid());
}
