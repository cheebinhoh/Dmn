/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue.cpp
 * @brief The unit test for dmn-buffer module.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <random> // Essential library
#include <string>
#include <thread>
#include <vector>

#include "dmn-lf-blockingqueue.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  auto queue = std::make_unique<dmn::Dmn_Lf_BlockingQueue<int>>();

  auto proc1 = std::make_unique<dmn::Dmn_Proc>("proc1", [&queue]() {
    for (int i = 0; i < 5; i++) {
      queue->push(i);

      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  });

  proc1->exec();

  auto res = queue->pop(10, 15000000);
  EXPECT_TRUE((res.size() == 5));
  EXPECT_TRUE((res[1] == 1));
  EXPECT_TRUE((res[4] == 4));

  return RUN_ALL_TESTS();
}
