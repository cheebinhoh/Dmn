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

#include "dmn-blockingqueue-mt.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  std::uniform_int_distribution<> global_distr(1, 10);

  auto queue = std::make_unique<dmn::Dmn_BlockingQueue_Mt<int>>();

  int count1{};
  int count2{};
  int count3{};
  int count4{};
  int count5{};
  int count6{};
  auto cons1 = std::make_unique<dmn::Dmn_Proc>(
      "cons1", [&count1, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] auto val = queue->pop();
            count1++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  auto cons2 = std::make_unique<dmn::Dmn_Proc>(
      "cons2", [&count2, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] auto val = queue->pop();
            count2++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  auto cons3 = std::make_unique<dmn::Dmn_Proc>(
      "cons3", [&count3, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] auto val = queue->pop();
            count3++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  auto cons4 = std::make_unique<dmn::Dmn_Proc>(
      "cons4", [&count4, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] auto val = queue->pop();
            count4++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  auto cons5 = std::make_unique<dmn::Dmn_Proc>(
      "cons5", [&count5, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] auto val = queue->pop();
            count5++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  auto cons6 = std::make_unique<dmn::Dmn_Proc>(
      "cons6", [&count6, &queue, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());

        try {
          while (true) {
            [[maybe_unused]] auto val = queue->pop();
            count6++;

            int pause = global_distr(local_gen);
            std::this_thread::sleep_for(std::chrono::microseconds(pause));
          }
        } catch (...) {
        }
      });

  cons1->exec();
  cons2->exec();
  cons3->exec();
  cons4->exec();
  cons5->exec();
  cons6->exec();

  std::this_thread::sleep_for(std::chrono::seconds(5));

  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < 10000; i++) {
    queue->push(i);
  }

  auto endSending = std::chrono::high_resolution_clock::now();
  auto durationSending =
      std::chrono::duration_cast<std::chrono::microseconds>(endSending - start);

  size_t size = queue->waitForEmpty();
  EXPECT_TRUE((10000 == size));

  auto endProcessing = std::chrono::high_resolution_clock::now();
  auto durationProcessing =
      std::chrono::duration_cast<std::chrono::microseconds>(endProcessing -
                                                            start);

  std::this_thread::sleep_for(std::chrono::microseconds(10000));
  queue = {};

  cons1->wait();
  cons2->wait();
  cons3->wait();
  cons4->wait();
  cons5->wait();
  cons6->wait();

  cons1 = {};
  cons2 = {};
  cons3 = {};
  cons4 = {};
  cons5 = {};
  cons6 = {};

  std::cout << "**** total duration: " << durationSending.count() << ", "
            << durationProcessing.count() << ", count:" << count1 << ", "
            << count2 << ", " << count3 << ", " << count4 << ", " << count5
            << ", " << count6 << "\n";

  EXPECT_TRUE((count1 + count2 + count3 + count4 + count5 + count6) == 10000);

  return RUN_ALL_TESTS();
}
