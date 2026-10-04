/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-blockingqueue-lf-stress.cpp
 * @brief Stress lock-free queue reclamation, shutdown, and element failures.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "dmn-blockingqueue-lf.hpp"

namespace {

constexpr std::size_t kProducerPushLimit = 256;
constexpr std::size_t kMinimumOperationsBeforeShutdown = 16;
constexpr auto kOperationStartTimeout = std::chrono::seconds(30);

class ObservableBlockingQueue final : public dmn::Dmn_BlockingQueue_Lf<int> {
public:
  auto waitForInflightEntries(std::size_t count) -> bool {
    std::unique_lock<std::mutex> lock(m_mutex);

    return m_entries_changed.wait_for(
        lock, std::chrono::seconds(5),
        [this, count] { return m_inflight_entries >= count; });
  }

protected:
  auto enterInflightGuardFnc() -> uint64_t override {
    const auto epoch_index =
        dmn::Dmn_BlockingQueue_Lf<int>::enterInflightGuardFnc();

    {
      std::lock_guard<std::mutex> lock(m_mutex);
      ++m_inflight_entries;
    }

    m_entries_changed.notify_all();

    return epoch_index;
  }

private:
  std::mutex m_mutex{};
  std::condition_variable m_entries_changed{};
  std::size_t m_inflight_entries{};
};

struct ThrowingValue {
  static inline std::atomic_bool s_throw_copy_assignment{};
  static inline std::atomic_bool s_throw_copy_constructor{};
  static inline std::atomic_bool s_throw_move_assignment{};

  ThrowingValue() = default;
  explicit ThrowingValue(int value) : m_value{value} {}

  ThrowingValue(const ThrowingValue &other) : m_value{other.m_value} {
    if (s_throw_copy_constructor.load(std::memory_order_relaxed)) {
      throw std::runtime_error("copy construction failed");
    }
  }

  ThrowingValue(ThrowingValue &&other) noexcept(false)
      : m_value{other.m_value} {}

  auto operator=(const ThrowingValue &other) -> ThrowingValue & {
    if (s_throw_copy_assignment.load(std::memory_order_relaxed)) {
      throw std::runtime_error("copy assignment failed");
    }

    m_value = other.m_value;

    return *this;
  }

  auto operator=(ThrowingValue &&other) noexcept(false) -> ThrowingValue & {
    if (s_throw_move_assignment.load(std::memory_order_relaxed)) {
      throw std::runtime_error("move assignment failed");
    }

    m_value = other.m_value;

    return *this;
  }

  int m_value{};
};

} // namespace

TEST(DmnBlockingQueueLfStressTest, ReusesEpochBucketsAcrossManyOperations) {
  dmn::Dmn_BlockingQueue_Lf<std::uint64_t> queue{};

  constexpr std::uint64_t kOperationCount = 60000;
  for (std::uint64_t i = 0; i < kOperationCount; ++i) {
    queue.push(i);
    EXPECT_EQ(queue.pop(), i);
  }

  EXPECT_EQ(queue.waitForEmpty(), kOperationCount);
}

TEST(DmnBlockingQueueLfStressTest, ShutdownRacesWithConcurrentPushAndPop) {
  for (int iteration = 0; iteration < 10; ++iteration) {
    ObservableBlockingQueue queue{};
    std::atomic_bool start{};
    std::atomic_size_t pushed{};
    std::atomic_size_t popped{};
    std::atomic_bool unexpected_exception{};

    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (int thread_index = 0; thread_index < 2; ++thread_index) {
      producers.emplace_back(
          [&queue, &start, &pushed, &unexpected_exception, thread_index] {
            while (!start.load(std::memory_order_acquire)) {
              std::this_thread::yield();
            }

            try {
              int value = thread_index;
              for (std::size_t i = 0; i < kProducerPushLimit; ++i) {
                queue.push(value++);
                pushed.fetch_add(1, std::memory_order_relaxed);
              }
            } catch (const std::runtime_error &) {
            } catch (...) {
              unexpected_exception.store(true, std::memory_order_relaxed);
            }
          });

      consumers.emplace_back([&queue, &start, &popped, &unexpected_exception] {
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }

        try {
          while (true) {
            [[maybe_unused]] auto value = queue.pop();
            popped.fetch_add(1, std::memory_order_relaxed);
          }
        } catch (const std::runtime_error &) {
        } catch (...) {
          unexpected_exception.store(true, std::memory_order_relaxed);
        }
      });
    }

    start.store(true, std::memory_order_release);

    const auto deadline =
        std::chrono::steady_clock::now() + kOperationStartTimeout;
    while ((pushed.load(std::memory_order_relaxed) <
                kMinimumOperationsBeforeShutdown ||
            popped.load(std::memory_order_relaxed) <
                kMinimumOperationsBeforeShutdown) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }

    const auto pushes_before_shutdown = pushed.load(std::memory_order_relaxed);
    const auto pops_before_shutdown = popped.load(std::memory_order_relaxed);

    queue.shutdown();

    for (auto &producer : producers) {
      producer.join();
    }

    for (auto &consumer : consumers) {
      consumer.join();
    }

    EXPECT_GE(pushes_before_shutdown, kMinimumOperationsBeforeShutdown)
        << "successful pushes before shutdown: " << pushes_before_shutdown
        << ", pops: " << pops_before_shutdown;
    EXPECT_GE(pops_before_shutdown, kMinimumOperationsBeforeShutdown)
        << "successful pops before shutdown: " << pops_before_shutdown
        << ", pushes: " << pushes_before_shutdown;
    EXPECT_FALSE(unexpected_exception.load(std::memory_order_relaxed));
    EXPECT_LE(popped.load(std::memory_order_relaxed),
              pushed.load(std::memory_order_relaxed));
  }
}

TEST(DmnBlockingQueueLfStressTest, ShutdownReleasesMultipleBlockingPopWaiters) {
  ObservableBlockingQueue queue{};
  constexpr std::size_t kWaiterCount = 6;
  std::atomic_size_t interrupted_waiters{};
  std::atomic_bool unexpected_exception{};
  std::vector<std::thread> waiters;

  for (std::size_t i = 0; i < kWaiterCount; ++i) {
    waiters.emplace_back([&queue, &interrupted_waiters, &unexpected_exception] {
      try {
        [[maybe_unused]] auto value = queue.pop();
      } catch (const std::runtime_error &) {
        interrupted_waiters.fetch_add(1, std::memory_order_relaxed);
      } catch (...) {
        unexpected_exception.store(true, std::memory_order_relaxed);
      }
    });
  }

  const auto all_waiters_entered = queue.waitForInflightEntries(kWaiterCount);
  queue.shutdown();

  for (auto &waiter : waiters) {
    waiter.join();
  }

  EXPECT_TRUE(all_waiters_entered);
  EXPECT_EQ(interrupted_waiters.load(std::memory_order_relaxed), kWaiterCount);
  EXPECT_FALSE(unexpected_exception.load(std::memory_order_relaxed));
}

TEST(DmnBlockingQueueLfStressTest, ThrowingPushAndPopKeepQueueUsable) {
  ThrowingValue::s_throw_copy_assignment.store(false,
                                               std::memory_order_relaxed);
  ThrowingValue::s_throw_copy_constructor.store(false,
                                                std::memory_order_relaxed);
  ThrowingValue::s_throw_move_assignment.store(false,
                                               std::memory_order_relaxed);
  dmn::Dmn_BlockingQueue_Lf<ThrowingValue> queue{};

  ThrowingValue copy_assignment_failure{1};
  ThrowingValue::s_throw_copy_assignment.store(true, std::memory_order_relaxed);
  EXPECT_THROW(queue.push(copy_assignment_failure), std::runtime_error);
  ThrowingValue::s_throw_copy_assignment.store(false,
                                               std::memory_order_relaxed);
  EXPECT_EQ(queue.waitForEmpty(), 0);

  ThrowingValue::s_throw_move_assignment.store(true, std::memory_order_relaxed);
  EXPECT_THROW(queue.push(ThrowingValue{2}), std::runtime_error);
  ThrowingValue::s_throw_move_assignment.store(false,
                                               std::memory_order_relaxed);
  EXPECT_EQ(queue.waitForEmpty(), 0);

  queue.push(ThrowingValue{2});
  ThrowingValue::s_throw_copy_constructor.store(true,
                                                std::memory_order_relaxed);
  EXPECT_THROW(queue.pop(), std::runtime_error);
  ThrowingValue::s_throw_copy_constructor.store(false,
                                                std::memory_order_relaxed);
  EXPECT_EQ(queue.waitForEmpty(), 1);

  queue.push(ThrowingValue{3});
  const auto value = queue.pop();
  EXPECT_EQ(value.m_value, 3);
  EXPECT_EQ(queue.waitForEmpty(), 2);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
