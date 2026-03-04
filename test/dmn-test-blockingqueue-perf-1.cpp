/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue-perf-1.cpp
 * @brief The unit test for dmn-buffer module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <random> // Essential library
#include <string>
#include <thread>
#include <vector>

#include "dmn-blockingqueue.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  std::uniform_int_distribution<> global_distr(1, 10);

  auto queue = std::make_unique<dmn::Dmn_BlockingQueue<int>>();

  int count1{};
  int count2{};
  int count3{};
  int count4{};
  auto cons1 = std::make_unique<dmn::Dmn_Proc>(
      "cons1", [&count1, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        while (true) {
          [[maybe_unused]] auto val = queue->pop();
          count1++;

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }
      });

  auto cons2 = std::make_unique<dmn::Dmn_Proc>(
      "cons2", [&count2, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        while (true) {
          [[maybe_unused]] auto val = queue->pop();
          count2++;

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }
      });

  auto cons3 = std::make_unique<dmn::Dmn_Proc>(
      "cons3", [&count3, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        while (true) {
          [[maybe_unused]] auto val = queue->pop();
          count3++;

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }
      });

  auto cons4 = std::make_unique<dmn::Dmn_Proc>(
      "cons4", [&count4, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        while (true) {
          [[maybe_unused]] auto val = queue->pop();
          count4++;

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }
      });

  cons1->exec();
  cons2->exec();
  cons3->exec();
  cons4->exec();

  std::this_thread::sleep_for(std::chrono::seconds(5));

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < 10000; i++) {
    queue->push(i);
  }

  auto endSending = std::chrono::high_resolution_clock::now();
  auto durationSending =
      std::chrono::duration_cast<std::chrono::microseconds>(endSending - start);

  queue->waitForEmpty();

  auto endProcessing = std::chrono::high_resolution_clock::now();
  auto durationProcessing =
      std::chrono::duration_cast<std::chrono::microseconds>(endProcessing -
                                                            start);

  std::this_thread::sleep_for(std::chrono::microseconds(10000));
  queue = {};

  cons1 = {};
  cons2 = {};
  cons3 = {};
  cons4 = {};

  std::cout << "**** total duration: " << durationSending.count() << ", "
            << durationProcessing.count() << ", count:" << count1 << ", "
            << count2 << ", " << count3 << ", " << count4 << "\n";

  EXPECT_TRUE((count1 + count2 + count3 + count4) == 10000);

  return RUN_ALL_TESTS();
}
