/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-async.cpp
 * @brief The unit test for dmn-async module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>

#include <sys/time.h>

#include "dmn-async.hpp"

class Counter {
public:
  auto getCount() -> int { return m_count; }

  void operator()(int value) { m_count += value; }

private:
  int m_count{};
};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_Async async{"run"};

  Counter counter{};
  auto waitHandler = async.addExecTaskWithWait(std::ref(counter), 5);
  waitHandler->wait();

  EXPECT_TRUE(counter.getCount() == 5);

  return RUN_ALL_TESTS();
}
