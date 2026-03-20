/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-pipe.cpp
 * @brief Unit test for Dmn_Pipe with sequential write and ordered read
 * operations.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "dmn-blockingqueue-lf.hpp"
#include "dmn-pipe.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  int cnt{};
  std::atomic_flag shutdown_flag{};
  auto pipe = std::make_unique<dmn::Dmn_Pipe<int>, dmn::Dmn_BlockingQueue_Lf<int>>(
      "pipe", [&cnt, &shutdown_flag](int val) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        EXPECT_TRUE(val == cnt);

        cnt++;
        std::cout << "read\n";

        if (5 == cnt) {
          shutdown_flag.test_and_set(std::memory_order_release);
          shutdown_flag.notify_all();
        }
      });

  dmn::Dmn_Proc procToWrite("toWrite", [&pipe]() {
    for (int i = 0; i < 5; i++) {
      std::cout << "write\n";
      pipe->write(i);
    }
  });

  procToWrite.exec();

  while (!shutdown_flag.test(std::memory_order_acquire)) {
    shutdown_flag.wait(false, std::memory_order_acquire);
  }

  pipe.reset();

  EXPECT_TRUE(5 == cnt);

  return RUN_ALL_TESTS();
}
