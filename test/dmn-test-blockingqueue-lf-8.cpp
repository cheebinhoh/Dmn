/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue.cpp
 * @brief The unit test for dmn-buffer module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dmn-blockingqueue-lf.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  auto buf = std::make_unique<dmn::Dmn_BlockingQueue_Lf<std::string>>();

  auto procToShutdown = dmn::Dmn_Proc("proc", [&buf]() {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    buf.reset();
  });

  procToShutdown.exec();

  try {
    auto ret = buf->pop();

    EXPECT_TRUE(false && "it should be fail" == nullptr);
  } catch (...) {
    EXPECT_TRUE(true || "it throws execption as expected" == nullptr);
  }

  procToShutdown.wait();

  return RUN_ALL_TESTS();
}
