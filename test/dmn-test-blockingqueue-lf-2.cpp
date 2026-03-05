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

#include "dmn-blockingqueue-lf.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  std::uniform_int_distribution<> global_distr(1, 10);

  auto queue = std::make_unique<dmn::Dmn_BlockingQueue_Lf<int>>();

  long long count1{};
  auto proc1 = std::make_unique<dmn::Dmn_Proc>(
      "proc1", [&queue, &global_distr, &count1]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] int i = queue->pop();
            count1++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  long long count2{};
  auto proc2 = std::make_unique<dmn::Dmn_Proc>(
      "proc2", [&queue, &global_distr, &count2]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] int i = queue->pop();
            count2++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  long long count3{};
  auto proc3 = std::make_unique<dmn::Dmn_Proc>(
      "proc3", [&queue, &global_distr, &count3]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] int i = queue->pop();
            count3++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  long long count4{};
  auto proc4 = std::make_unique<dmn::Dmn_Proc>(
      "proc4", [&queue, &global_distr, &count4]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] int i = queue->pop();
            count4++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  proc1->exec();
  proc2->exec();
  proc3->exec();
  proc4->exec();

  std::this_thread::sleep_for(std::chrono::seconds(5));

  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < 10000; i++) {
    queue->push(i);
  }

  auto endSending = std::chrono::high_resolution_clock::now();
  auto durationSending =
      std::chrono::duration_cast<std::chrono::microseconds>(endSending - start);

  size_t total = queue->waitForEmpty();
  EXPECT_TRUE((10000 == total));

  auto endProcessing = std::chrono::high_resolution_clock::now();
  auto durationProcessing =
      std::chrono::duration_cast<std::chrono::microseconds>(endProcessing -
                                                            start);

  std::this_thread::sleep_for(std::chrono::microseconds(10000));
  queue = {};

  proc1->wait();
  proc2->wait();
  proc3->wait();
  proc4->wait();

  proc1 = {};
  proc2 = {};
  proc3 = {};
  proc4 = {};

  std::cout << "**** total duration: " << durationSending.count() << ", "
            << durationProcessing.count() << ", count:" << count1 << ", "
            << count2 << ", " << count3 << ", " << count4 << "\n";

  EXPECT_TRUE((count1 + count2 + count3 + count4) == 10000);

  return RUN_ALL_TESTS();
}
