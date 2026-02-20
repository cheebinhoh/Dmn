/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-buffer.cpp
 * @brief The unit test for dmn-buffer module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dmn-blockingqueue.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  auto buf = std::make_unique<dmn::Dmn_BlockingQueue<std::string>>();
  auto proc = std::make_unique<dmn::Dmn_Proc>("proc", [&buf]() -> void {
    static std::vector<std::string> result{"hello", "abc"};
    static int index{};

    auto str = buf->pop();
    EXPECT_TRUE(str == result[index++]);
    std::cout << "value pop: " << str << "\n";
  });

  std::string value{"hello"};

  buf->push(value);
  EXPECT_TRUE(value.empty());

  proc->exec();

  dmn::Dmn_Proc::yield();
  proc->wait();

  buf->push("abc");

  proc->exec();
  dmn::Dmn_Proc::yield();
  proc->wait();

  proc->exec();
  dmn::Dmn_Proc::yield();

  std::this_thread::sleep_for(std::chrono::seconds(2));
  proc = {};
  std::this_thread::sleep_for(std::chrono::seconds(2));
  buf = {};
  std::this_thread::sleep_for(std::chrono::seconds(3));

  dmn::Dmn_BlockingQueue<int> int_buf{};
  int_buf.push(2);

  EXPECT_TRUE(2 == int_buf.pop());

  dmn::Dmn_BlockingQueue<std::string> string_not_move_buf{};
  std::string string_to_mot_move_buf{"not move"};
  string_not_move_buf.push(string_to_mot_move_buf, false);
  const std::string string_from_not_move_buf = string_not_move_buf.pop();

  EXPECT_TRUE("not move" == string_from_not_move_buf);
  EXPECT_TRUE("not move" == string_to_mot_move_buf);

  dmn::Dmn_BlockingQueue<int> int_buf_2{1, 2, 3};
  EXPECT_TRUE(1 == int_buf_2.pop());
  EXPECT_TRUE(2 == int_buf_2.pop());
  EXPECT_TRUE(3 == int_buf_2.pop());

  return RUN_ALL_TESTS();
}
