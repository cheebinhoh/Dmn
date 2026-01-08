/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-buffer.cpp
 * @brief The unit test for dmn-buffer module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dmn-buffer.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  std::string data1{"abc"};
  std::string data2{"xyz"};
  std::string data3{"mno"};

  auto buf = std::make_unique<dmn::Dmn_Buffer<std::string>>();
  bool readDone{};
  auto proc = std::make_unique<dmn::Dmn_Proc>(
      "proc", [&buf, &readDone, data1, data2, data3]() -> void {
        auto listOfData = buf->pop(3, 15000000L);
        readDone = true;
      });

  proc->exec();
  std::this_thread::sleep_for(std::chrono::seconds(2));

  buf->push(data1);
  std::this_thread::sleep_for(std::chrono::seconds(2));

  buf->push(data2);
  std::this_thread::sleep_for(std::chrono::seconds(5));

  EXPECT_TRUE(!readDone);

  return RUN_ALL_TESTS();
}
