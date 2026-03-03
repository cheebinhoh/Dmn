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
#include <string>
#include <thread>
#include <vector>

#include "dmn-lf-blockingqueue.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  using namespace std::string_literals;

  auto queue = std::make_unique<dmn::Dmn_Lf_BlockingQueue<int>>();

  std::atomic_flag proc1Stop{};
  auto proc1 = std::make_unique<dmn::Dmn_Proc>("proc1", [&queue, &proc1Stop]() {
    std::this_thread::sleep_for(std::chrono::seconds(2));

    for (int i = 0; i < 1000; i++) {
      queue->push(i);
      dmn::Dmn_Proc::yield();
    }

    proc1Stop.test_and_set(std::memory_order_release);
    proc1Stop.notify_all();
  });

  std::atomic_flag proc2Stop{};
  auto proc2 = std::make_unique<dmn::Dmn_Proc>("proc2", [&queue, &proc2Stop]() {
    std::this_thread::sleep_for(std::chrono::seconds(2));

    for (int i = 0; i < 1000; i++) {
      queue->push(i);
      dmn::Dmn_Proc::yield();
    }

    proc2Stop.test_and_set(std::memory_order_release);
    proc2Stop.notify_all();
  });

  auto start = std::chrono::high_resolution_clock::now();

  proc1->exec();
  proc2->exec();

  while (!proc1Stop.test(std::memory_order_acquire)) {
    proc1Stop.wait(false, std::memory_order_acquire);
  }

  while (!proc2Stop.test(std::memory_order_acquire)) {
    proc2Stop.wait(false, std::memory_order_acquire);
  }

  auto endSending = std::chrono::high_resolution_clock::now();
  auto durationSending =
      std::chrono::duration_cast<std::chrono::microseconds>(endSending - start);

  std::cout << "count: " << queue->debugPrint()
            << ", time: " << durationSending.count() << "\n";

  proc1 = {};
  proc2 = {};

  return RUN_ALL_TESTS();
}
