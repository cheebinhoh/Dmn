/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue-shutdown.cpp
 * @brief Test mutex blocking queue shutdown behavior.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <vector>

#include "dmn-blockingqueue-mt.hpp"

namespace {

class ObservableBlockingQueue final : public dmn::Dmn_BlockingQueue_Mt<int> {
public:
  auto popAdmissionFuture() { return m_pop_admitted.get_future(); }

  void signalNextPopAdmission() {
    m_admission_check_count.store(0, std::memory_order_relaxed);
    m_signal_admission.store(true, std::memory_order_release);
  }

protected:
  auto isInflightGuardClosed() -> bool override {
    const auto is_closed =
        dmn::Dmn_BlockingQueue_Mt<int>::isInflightGuardClosed();

    // The guard checks closed state again after incrementing the active count.
    if (m_signal_admission.load(std::memory_order_acquire) &&
        m_admission_check_count.fetch_add(1, std::memory_order_relaxed) == 1) {
      m_pop_admitted.set_value();
      m_signal_admission.store(false, std::memory_order_release);
    }

    return is_closed;
  }

private:
  std::atomic_size_t m_admission_check_count{};
  std::atomic_bool m_signal_admission{};
  std::promise<void> m_pop_admitted{};
};

} // namespace

TEST(DmnBlockingQueueMtShutdownTest,
     ReturnsPartialBatchWhenShutdownWakesAdmittedPop) {
  ObservableBlockingQueue queue{};
  queue.push(10);
  queue.push(20);

  auto pop_admission = queue.popAdmissionFuture();
  queue.signalNextPopAdmission();

  std::promise<std::vector<int>> pop_result_promise{};
  auto pop_result = pop_result_promise.get_future();
  std::thread pop_thread{[&queue, &pop_result_promise] {
    try {
      pop_result_promise.set_value(queue.pop(3));
    } catch (...) {
      pop_result_promise.set_exception(std::current_exception());
    }
  }};

  if (pop_admission.wait_for(std::chrono::seconds(5)) !=
      std::future_status::ready) {
    queue.shutdown();
    pop_thread.join();
    FAIL() << "bulk pop did not acquire an in-flight ticket";

    return;
  }

  queue.shutdown();
  pop_thread.join();

  const auto result = pop_result.get();
  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0], 10);
  EXPECT_EQ(result[1], 20);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
