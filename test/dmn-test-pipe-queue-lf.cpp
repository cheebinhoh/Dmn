/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-pipe-queue-lf.cpp
 * @brief Unit test for Dmn_Pipe backed by a lock-free blocking queue with sequential write and ordered read.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "dmn-blockingqueue-lf.hpp"
#include "dmn-pipe.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  int cnt{};
  dmn::Dmn_Pipe<int, dmn::Dmn_BlockingQueue_Lf<int>> pipe{
      "pipe", [&cnt](int val) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        EXPECT_TRUE(val == cnt);

        cnt++;
        std::cout << "read\n";
      }};

  for (int i = 0; i < 5; i++) {
    pipe.write(i);
  }

  std::cout << "Before wait for empty\n";
  pipe.waitForEmpty();
  EXPECT_TRUE(5 == cnt);

  return RUN_ALL_TESTS();
}
