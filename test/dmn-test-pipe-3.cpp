/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test programs asserts that two Dmn_DMesgNet objects can
 * one send message through a Dmn_Socket at a particular ip and port
 * and another one receive sent message through another Dmn_Socket
 * at the same ip and port.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "dmn-io.hpp"
#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto pipe_1 = std::make_unique<dmn::Dmn_Pipe<std::string>>("pipe_1");

  auto pipe_2 = std::make_unique<dmn::Dmn_Pipe<std::string>>("pipe_2");

  auto pipe_3 = std::make_unique<dmn::Dmn_Pipe<std::string>>("pipe_3");

  size_t pipe_1_cnt{};
  size_t pipe_2_cnt{};
  size_t pipe_3_cnt{};

  dmn::Dmn_Proc proc_1{"proc_1", [&pipe_1, &pipe_1_cnt]() {
                         do {
                           auto data = pipe_1->read(4);
                           pipe_1_cnt += data.size();
                         } while (true);
                       }};

  dmn::Dmn_Proc proc_2{"proc_2", [&pipe_2, &pipe_1, &pipe_2_cnt]() {
                         do {
                           auto data = pipe_2->read(2);
                           pipe_2_cnt += data.size();

                           for (auto &d : data) {
                             pipe_1->write(d);
                           }
                         } while (true);
                       }};

  dmn::Dmn_Proc proc_3{"proc_3", [&pipe_3, &pipe_2, &pipe_3_cnt]() {
                         do {
                           auto data = pipe_3->read(1);
                           pipe_3_cnt += data.size();

                           for (auto &d : data) {
                             pipe_2->write(d);
                           }
                         } while (true);
                       }};
  proc_3.exec();
  proc_2.exec();
  proc_1.exec();

  dmn::Dmn_Proc::yield();
  std::this_thread::sleep_for(std::chrono::seconds(2));

  pipe_3->write("1");
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE((1 == pipe_3_cnt));
  EXPECT_TRUE((0 == pipe_2_cnt));
  EXPECT_TRUE((0 == pipe_1_cnt));

  pipe_3->write("2");
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE((2 == pipe_3_cnt));
  EXPECT_TRUE((2 == pipe_2_cnt));
  EXPECT_TRUE((0 == pipe_1_cnt));

  pipe_3->write("3");
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE((3 == pipe_3_cnt));
  EXPECT_TRUE((2 == pipe_2_cnt));
  EXPECT_TRUE((0 == pipe_1_cnt));

  pipe_3->write("4");
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE((4 == pipe_3_cnt));
  EXPECT_TRUE((4 == pipe_2_cnt));
  EXPECT_TRUE((4 == pipe_1_cnt));

  return RUN_ALL_TESTS();
}
