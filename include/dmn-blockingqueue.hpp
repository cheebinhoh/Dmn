/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue.hpp
 * @brief Thread-safe FIFO blocking queue for passing items between threads.
 *
 * Overview
 * --------
 * Dmn_BlockingQueue<T> is a thread-safe FIFO queue implementation intended for
 * use by producer and consumer threads. Key behavioral properties:
 *  - push operations are non-blocking (they will not wait for consumers).
 *  - pop operations may block waiting for data (either indefinitely or with a
 *    timeout, depending on the API used).
 *
 * Synchronization and semantics
 * -----------------------------
 * - A mutex (m_mutex) protects the internal std::deque<T> m_queue and
 *   associated counters (m_push_count, m_pop_count).
 *
 * - Three condition variables are used:
 *   - m_not_empty_cond: signalled on push; used by the multi-item timed pop
 *     pop(count, timeout) to wait until the queue has at least one item or the
 *     target number of items.
 *   - m_empty_cond: signalled when the queue becomes empty; used by
 *     waitForEmpty() to wait until all outstanding items have been consumed.
 *
 * - Counters:
 *   - m_push_count: incremented on every successful push. This is useful for
 *     accounting how many items have entered the buffer in total.
 *   - m_pop_count: incremented on every successful pop. waitForEmpty() asserts
 *     that m_pop_count == m_push_count when returning.
 *
 * Blocking and timeout semantics
 * ------------------------------
 * - pop(): Blocks until at least one item is available, then returns that
 *   item.
 *
 * - popNoWait(): Non-blocking pop that returns std::nullopt if the queue is
 *   empty.
 *
 * - pop(count, timeout):
 *   This function attempts to retrieve up to `count` items. Its blocking and
 *   return behavior:
 *     1. If the queue already contains >= count items, it returns exactly
 *        `count` items immediately.
 *     2. If the queue contains 0 items, it blocks:
 *        - If timeout == 0: blocks indefinitely until at least `count` items
 *          become available (returns exactly `count`).
 *        - If timeout > 0: waits up to `timeout` microseconds for items.
 *          * If enough items are available before timeout, returns exactly
 *            `count` items.
 *          * If the timeout expires and the queue contains at least 1 item,
 *            returns however many items are currently available (between 1
 *            and `count`).
 *          * If the timeout expires and the queue is still empty, returns
 *            no item.
 *
 *   Note: The timeout is interpreted as a maximum time to wait for the full
 *   `count` items (measured from the first blocking wait inside the call).
 *   A zero timeout value means "wait forever".
 *
 * Implementation notes
 * --------------------
 * - The queue uses an unbounded std::deque<T>. There is no programmatic
 *   limit beyond available memory and the semantics of the stored type T.
 *
 * - The class deletes copy and move constructors / assignment operators to
 *   ensure unique ownership of the synchronization primitives and avoid
 *   incidental sharing between objects.
 */

#ifndef DMN_BLOCKINGQUEUE_HPP_
#define DMN_BLOCKINGQUEUE_HPP_

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmn-proc.hpp"
#include "dmn-util.hpp"

#include "dmn-blockingqueue-interface.hpp"
#include "dmn-inflight-guard.hpp"

namespace dmn {

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string>
class Dmn_BlockingQueue : public Dmn_BlockingQueue_Interface<T>,
                          private Dmn_Inflight_Guard<> {
public:
  using Dmn_BlockingQueue_Interface<T>::pop;
  using Dmn_BlockingQueue_Interface<T>::push;

  Dmn_BlockingQueue();
  Dmn_BlockingQueue(std::initializer_list<T> list);
  virtual ~Dmn_BlockingQueue() noexcept;

  Dmn_BlockingQueue(const Dmn_BlockingQueue<T> &obj) = delete;
  Dmn_BlockingQueue<T> &operator=(const Dmn_BlockingQueue<T> &obj) = delete;
  Dmn_BlockingQueue(Dmn_BlockingQueue<T> &&obj) = delete;
  Dmn_BlockingQueue<T> &operator=(Dmn_BlockingQueue<T> &&obj) = delete;

  /**
   * @brief Pop multiple items from the queue with optional timeout semantics.
   *
   * Detailed semantics:
   * - count > 0 is required (asserted).
   * - If the queue has >= count items, this returns exactly count items.
   * - If the queue is empty:
   *   - timeout == 0: wait indefinitely for count items (return exactly count).
   *   - timeout > 0: wait up to timeout microseconds for items.
   *     * If timeout expires and there is at least one item, return 1..count
   *       items (the current queue size).
   *     * If timeout expires and the queue is still empty, the function returns
   *       no item.
   *
   * The returned vector contains moved items removed from the queue.
   *
   * @param count   Number of desired items (must be > 0).
   * @param timeout Timeout in microseconds for waiting for the full count.
   *                A value of 0 means wait forever.
   * @return Vector of items (size == count on success without timeout, or
   *         between 1 and count if a timeout occurred after at least one item
   *         was produced).
   */
  virtual auto pop(size_t count, long timeout = 0) -> std::vector<T> override;

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   *        items that have passed through the queue.
   *
   * This blocks until the queue is empty and asserts that all pushed items
   * have been popped (m_pop_count == m_push_count) before returning the
   * inbound count.
   *
   * @return The total number of items that have been passed through the queue.
   */
  virtual auto waitForEmpty() -> uint64_t override;

protected:
  /**
   * @brief Internal helper that optionally blocks waiting for an item.
   *
   * @param wait If true, block until an item is available; otherwise return
   *             std::nullopt immediately if empty.
   * @return optional value popped from the front of the queue.
   */
  virtual auto popOptional(bool wait) -> std::optional<T> override;

  /**
   * @brief Wrapper call to pushImpl to copy and enqueue the item into the
   *        queue.
   *
   * @param item The item to be enqueued.
   */
  void pushCopy(const T &item) override;

  /**
   * @brief Wrapper call to pushImpl to move and enqueue the item into the
   *        queue.
   *
   * @param item The item to be enqueued.
   */
  void pushMove(T &&item) override;

  /**
   * @brief Signal all waiting threads to wake up and return.
   *
   * Sets the m_shutdown_flag and notifies all condition variables so
   * that threads blocked in pop(), pop(count, timeout), or
   * waitForEmpty() can observe the shutdown flag and return cleanly.
   * This method is called by Dmn_Pipe's destructor via the protected
   * interface.
   */
  virtual void stop() override;

  /**
   * @brief Return true if the queue is stop (m_shutdown_flag is true), or false
   *        otherwise. The method is delegation of Dmn_Inflight_Guard module.
   *
   * @return true or false that the queue is shutdown.
   */
  auto isInflightGuardClosed() -> bool override;

private:
  static void cleanup_thunk_inflight(void *arg);

  template <class U> void pushImpl(U &&item);

  std::deque<T> m_queue{};
  std::mutex m_mutex{};
  std::condition_variable m_empty_cond{}; // signalled when queue becomes empty
  std::condition_variable
      m_not_empty_cond{}; // signalled on push (multi-pop timed wait)
  uint64_t m_push_count{};
  uint64_t m_pop_count{};
  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_BlockingQueue

template <typename T> Dmn_BlockingQueue<T>::Dmn_BlockingQueue() {}

template <typename T>
Dmn_BlockingQueue<T>::Dmn_BlockingQueue(std::initializer_list<T> list)
    : Dmn_BlockingQueue{} {
  for (auto data : list) {
    this->push(data);
  }
}

template <typename T> Dmn_BlockingQueue<T>::~Dmn_BlockingQueue() noexcept try {
  stop();

  // make sure that all api call within the inflight guard exits.
  this->waitForEmptyInflight();
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T>
void Dmn_BlockingQueue<T>::cleanup_thunk_inflight(void *arg) {
  auto *ticket_sp =
      static_cast<std::unique_ptr<Dmn_Inflight_Guard<>::Ticket> *>(arg);

  (*ticket_sp).reset();
}

template <typename T>
auto Dmn_BlockingQueue<T>::isInflightGuardClosed() -> bool {
  return m_shutdown_flag.test(std::memory_order_acquire);
}

template <typename T> void Dmn_BlockingQueue<T>::pushCopy(const T &item) {
  pushImpl(item); // copy
}

template <typename T> void Dmn_BlockingQueue<T>::pushMove(T &&item) {
  pushImpl(std::move(item)); // move
}

/**
 * @brief Internal enqueue helper with perfect forwarding.
 *
 * - For rvalues: moves into the deque (one construction)
 * - For lvalues: copies into the deque
 *
 * If you still want the old “move_if_noexcept” behavior for lvalues, keep that
 * logic at the public overload level (see push(T&, bool)).
 */
template <typename T>
template <class U>
void Dmn_BlockingQueue<T>::pushImpl(U &&item) {
  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_queue.emplace_back(std::forward<U>(item));
  ++m_push_count;

  lock.unlock();

  // Notify multi-pop waiters first (they use m_none_empty_cond).
  m_not_empty_cond.notify_one();

  DMN_PROC_CLEANUP_POP(0);
}

template <typename T> void Dmn_BlockingQueue<T>::stop() {
  m_shutdown_flag.test_and_set(std::memory_order_release);

  m_empty_cond.notify_all();
  m_not_empty_cond.notify_all();
}

template <typename T> auto Dmn_BlockingQueue<T>::waitForEmpty() -> uint64_t {
  uint64_t inbound_count{};

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_empty_cond.wait(lock, [this] {
    return m_queue.empty() || this->isInflightGuardClosed();
  });

  assert(m_pop_count == m_push_count);
  inbound_count = m_pop_count;

  return inbound_count;
}

template <typename T>
auto Dmn_BlockingQueue<T>::pop(size_t count, long timeout) -> std::vector<T> {
  std::vector<T> ret{};

  assert(count > 0);

  auto inflightTicket = this->enterInflightGate();

  std::unique_lock<std::mutex> lock(m_mutex);

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  Dmn_Proc::testcancel();

  // Wait until there are at least 'count' items OR a timeout occurs with at
  // least one available item.
  if (timeout > 0) {
    if (m_not_empty_cond.wait_for(
            lock, std::chrono::microseconds(timeout), [this, count] {
              return m_queue.size() >= count || this->isInflightGuardClosed();
            })) {
      // do nothing and we have what we want
    } else if (m_queue.empty()) {
      return {};
    } else {
      // do nothing and fetch whatever we have
    }
  } else {
    m_not_empty_cond.wait(lock, [this, count] {
      return m_queue.size() >= count || this->isInflightGuardClosed();
    });
  }

  if (this->isInflightGuardClosed()) {
    return ret;
  }

  // Collect up to 'count' items (moved out).
  do {
    ret.push_back(std::move_if_noexcept(m_queue.front()));
    m_queue.pop_front();
    ++m_pop_count;

    count--;
  } while (count > 0 && (!m_queue.empty()));

  // If queue became empty as a result of this pop, notify waitForEmpty()
  // waiters.
  bool empty = m_queue.empty();
  lock.unlock();

  if (empty) {
    m_empty_cond.notify_all();
  } else {
    m_not_empty_cond.notify_one();
  }

  DMN_PROC_CLEANUP_POP(0);

  return ret;
}

template <typename T>
auto Dmn_BlockingQueue<T>::popOptional(bool wait) -> std::optional<T> {
  std::unique_lock<std::mutex> lock(m_mutex);

  std::optional<T> ret{};

  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  Dmn_Proc::testcancel();

  if (m_queue.empty()) {
    if (!wait) {
      return {};
    }

    // Block until an item is available. This wait is a cancellation point.
    m_not_empty_cond.wait(lock, [this] {
      return !m_queue.empty() || this->isInflightGuardClosed();
    });
  }

  if (this->isInflightGuardClosed()) {
    return {};
  }

  ret = std::move_if_noexcept(m_queue.front());
  m_queue.pop_front();

  ++m_pop_count;

  bool empty = m_queue.empty();

  lock.unlock();

  // Notify waiters waiting for the queue to become empty.
  if (empty) {
    m_empty_cond.notify_all();
  } else {
    m_not_empty_cond.notify_one();
  }

  DMN_PROC_CLEANUP_POP(0);

  return ret;
} // method popOptional()

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_HPP_
