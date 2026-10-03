/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue-lf-wait-empty.cpp
 * @brief Test lock-free queue drain observation and shutdown.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "dmn-blockingqueue-lf.hpp"

namespace {

class ObservableBlockingQueue final : public dmn::Dmn_BlockingQueue_Lf<int> {
public:
  auto nextInflightEntryFuture() { return m_inflight_entry.get_future(); }

  void signalNextInflightEntry() {
    m_signal_inflight_entry.store(true, std::memory_order_release);
  }

protected:
  auto enterInflightGuardFnc() -> uint64_t override {
    const auto epoch_index =
        dmn::Dmn_BlockingQueue_Lf<int>::enterInflightGuardFnc();

    if (m_signal_inflight_entry.exchange(false, std::memory_order_acq_rel)) {
      m_inflight_entry.set_value();
    }

    return epoch_index;
  }

private:
  std::atomic_bool m_signal_inflight_entry{};
  std::promise<void> m_inflight_entry{};
};

} // namespace

TEST(DmnBlockingQueueLfWaitForEmptyTest, WaitsForConcurrentConsumerToDrain) {
  ObservableBlockingQueue queue{};
  queue.push(42);

  auto inflight_entry = queue.nextInflightEntryFuture();
  queue.signalNextInflightEntry();

  std::promise<uint64_t> drained_count_promise{};
  auto drained_count = drained_count_promise.get_future();
  std::thread wait_thread{[&queue, &drained_count_promise] {
    drained_count_promise.set_value(queue.waitForEmpty());
  }};

  if (inflight_entry.wait_for(std::chrono::seconds(5)) !=
      std::future_status::ready) {
    queue.pop();
    wait_thread.join();
    FAIL() << "waitForEmpty did not acquire an in-flight ticket";

    return;
  }

  EXPECT_EQ(queue.pop(), 42);
  wait_thread.join();
  EXPECT_EQ(drained_count.get(), 1);
}

TEST(DmnBlockingQueueLfWaitForEmptyTest, ReturnsWhenShutdownBegins) {
  ObservableBlockingQueue queue{};
  queue.push(42);

  auto inflight_entry = queue.nextInflightEntryFuture();
  queue.signalNextInflightEntry();

  std::promise<uint64_t> drained_count_promise{};
  auto drained_count = drained_count_promise.get_future();
  std::thread wait_thread{[&queue, &drained_count_promise] {
    drained_count_promise.set_value(queue.waitForEmpty());
  }};

  if (inflight_entry.wait_for(std::chrono::seconds(5)) !=
      std::future_status::ready) {
    queue.shutdown();
    wait_thread.join();
    FAIL() << "waitForEmpty did not acquire an in-flight ticket";

    return;
  }

  queue.shutdown();
  wait_thread.join();
  EXPECT_EQ(drained_count.get(), 1);
  EXPECT_EQ(queue.waitForEmpty(), 1);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
