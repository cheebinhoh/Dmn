/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue-perf-2.cpp
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

#include "dmn-blockingqueue.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  std::uniform_int_distribution<> global_distr(1, 10);

  auto queue = std::make_unique<dmn::Dmn_BlockingQueue<int>>();

  std::atomic_flag proc1Stop{};
  auto proc1 = std::make_unique<dmn::Dmn_Proc>(
      "proc1", [&queue, &proc1Stop, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());
        std::this_thread::sleep_for(std::chrono::seconds(2));

        for (int i = 0; i < 10000; i++) {
          queue->push(i);

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }

        proc1Stop.test_and_set(std::memory_order_release);
        proc1Stop.notify_all();
      });

  std::atomic_flag proc2Stop{};
  auto proc2 = std::make_unique<dmn::Dmn_Proc>(
      "proc2", [&queue, &proc2Stop, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());
        std::this_thread::sleep_for(std::chrono::seconds(2));

        for (int i = 0; i < 10000; i++) {
          queue->push(i);

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }

        proc2Stop.test_and_set(std::memory_order_release);
        proc2Stop.notify_all();
      });

  std::atomic_flag proc3Stop{};
  auto proc3 = std::make_unique<dmn::Dmn_Proc>(
      "proc3", [&queue, &proc3Stop, &global_distr]() {
        std::mt19937 local_gen(std::random_device{}());
        std::this_thread::sleep_for(std::chrono::seconds(2));

        for (int i = 0; i < 10000; i++) {
          queue->push(i);

          int pause = global_distr(local_gen);
          std::this_thread::sleep_for(std::chrono::microseconds(pause));
        }

        proc3Stop.test_and_set(std::memory_order_release);
        proc3Stop.notify_all();
      });

  auto start = std::chrono::high_resolution_clock::now();

  proc1->exec();
  proc2->exec();
  proc3->exec();

  while (!proc1Stop.test(std::memory_order_acquire)) {
    proc1Stop.wait(false, std::memory_order_acquire);
  }

  while (!proc2Stop.test(std::memory_order_acquire)) {
    proc2Stop.wait(false, std::memory_order_acquire);
  }

  while (!proc3Stop.test(std::memory_order_acquire)) {
    proc3Stop.wait(false, std::memory_order_acquire);
  }

  auto endSending = std::chrono::high_resolution_clock::now();
  auto durationSending =
      std::chrono::duration_cast<std::chrono::microseconds>(endSending - start);

  std::cout << "time: " << durationSending.count() << "\n";

  proc1->wait();
  proc2->wait();

  proc1 = {};
  proc2 = {};

  return RUN_ALL_TESTS();
}
