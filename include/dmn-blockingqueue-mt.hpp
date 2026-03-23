/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-mt.hpp
 * @brief Thread-safe FIFO blocking queue interface for passing items between
 * threads.
 *
 * Overview
 * --------
 * Dmn_BlockingQueue_Mt<T> is a thread-safe FIFO queue implementation intended
 * for use by producer and consumer threads. Key behavioral properties:
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

#ifndef DMN_BLOCKINGQUEUE_MT_HPP_
#define DMN_BLOCKINGQUEUE_MT_HPP_

#include "dmn-blockingqueue.hpp"

#include "dmn-inflight-guard.hpp"
#include "dmn-proc.hpp"
#include "dmn-util.hpp"

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
#include <utility>
#include <vector>

namespace dmn {

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string>
class Dmn_BlockingQueue_Mt
    : public Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<T>, T>,
      private Dmn_Inflight_Guard<> {
  friend class Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<T>, T>;

  using Inflight_Guard_Ticket = std::unique_ptr<Dmn_Inflight_Guard<>::Ticket>;

public:
  using Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<T>, T>::isShutdown;
  using Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<T>, T>::pop;
  using Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<T>, T>::push;

  Dmn_BlockingQueue_Mt();
  Dmn_BlockingQueue_Mt(std::initializer_list<T> list);
  virtual ~Dmn_BlockingQueue_Mt() noexcept;

  Dmn_BlockingQueue_Mt(const Dmn_BlockingQueue_Mt<T> &obj) = delete;
  Dmn_BlockingQueue_Mt<T> &
  operator=(const Dmn_BlockingQueue_Mt<T> &obj) = delete;
  Dmn_BlockingQueue_Mt(Dmn_BlockingQueue_Mt<T> &&obj) = delete;
  Dmn_BlockingQueue_Mt<T> &operator=(Dmn_BlockingQueue_Mt<T> &&obj) = delete;

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
   * A value of 0 means wait forever.
   *
   * @return Vector of items (size == count on success without timeout, or
   * between 1 and count if a timeout occurred after at least one item was
   * produced).
   */
  virtual auto pop(size_t count, long timeout = 0) -> std::vector<T> override;

  /**
   * @brief Signal all waiting threads to wake up and return.
   *
   * Sets the m_shutdown_flag and notifies all condition variables so
   * that threads blocked in pop(), pop(count, timeout), or
   * waitForEmpty() can observe the shutdown flag and return cleanly.
   * This method is called by Dmn_Pipe's destructor via the protected
   * interface.
   */
  virtual void shutdown() override;

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   * items that have passed through the queue.
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
   * @brief Return true if the queue is stop (m_shutdown_flag is true), or false
   * otherwise. The method is delegation of Dmn_Inflight_Guard module.
   *
   * @return true or false that the queue is shutdown.
   */
  auto isInflightGuardClosed() -> bool override;

  /**
   * @brief Internal helper that optionally blocks waiting for an item.
   *
   * @param wait If true, block until an item is available; otherwise return
   * std::nullopt immediately if empty.
   *
   * @return optional value popped from the front of the queue.
   */
  virtual auto popOptional(bool wait) -> std::optional<T> override;

  /**
   * @brief Wrapper call to pushImpl to copy and enqueue the item into the
   * queue.
   *
   * @param item The item to be enqueued.
   */
  void pushCopy(const T &item) override;

  /**
   * @brief Wrapper call to pushImpl to move and enqueue the item into the
   * queue.
   *
   * @param item The item to be enqueued.
   */
  void pushMove(T &&item) override;

private:
  static void cleanup_thunk_inflight(void *arg);

  template <class U> void pushImpl(U &&item);

  std::deque<T> m_queue{};
  std::mutex m_mutex{};
  std::condition_variable m_empty_cond{};
  std::condition_variable m_not_empty_cond{};
  uint64_t m_push_count{};
  uint64_t m_pop_count{};
}; // class Dmn_BlockingQueue_Mt

template <typename T> Dmn_BlockingQueue_Mt<T>::Dmn_BlockingQueue_Mt() {}

template <typename T>
Dmn_BlockingQueue_Mt<T>::Dmn_BlockingQueue_Mt(std::initializer_list<T> list)
    : Dmn_BlockingQueue_Mt{} {
  for (auto data : list) {
    this->push(data);
  }
}

template <typename T>
Dmn_BlockingQueue_Mt<T>::~Dmn_BlockingQueue_Mt() noexcept try {
  if (!isShutdown()) {
    shutdown();
  }

  // make sure that all api call within the inflight guard exits.
  this->waitForEmptyInflight();
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T>
void Dmn_BlockingQueue_Mt<T>::cleanup_thunk_inflight(void *arg) {
  auto *ticket_sp = static_cast<Inflight_Guard_Ticket *>(arg);

  (*ticket_sp).reset();
}

template <typename T>
auto Dmn_BlockingQueue_Mt<T>::isInflightGuardClosed() -> bool {
  return isShutdown();
}

template <typename T> void Dmn_BlockingQueue_Mt<T>::pushCopy(const T &item) {
  pushImpl(item); // copy
}

template <typename T> void Dmn_BlockingQueue_Mt<T>::pushMove(T &&item) {
  pushImpl(std::move(item)); // move
}

/**
 * @brief Internal enqueue helper with perfect forwarding.
 *
 * - For rvalues: moves into the deque (one construction)
 * - For lvalues: copies into the deque
 *
 * If you still want custom “move_if_noexcept” behavior for lvalues, keep that
 * logic at the public overload level (for example in pushCopy/pushMove)
 * before delegating to pushImpl.
 */
template <typename T>
template <class U>
void Dmn_BlockingQueue_Mt<T>::pushImpl(U &&item) {
  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Mt<T>::cleanup_thunk_inflight,
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

template <typename T> void Dmn_BlockingQueue_Mt<T>::shutdown() {
  Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<T>, T>::shutdown();

  m_empty_cond.notify_all();
  m_not_empty_cond.notify_all();
}

template <typename T> auto Dmn_BlockingQueue_Mt<T>::waitForEmpty() -> uint64_t {
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
auto Dmn_BlockingQueue_Mt<T>::pop(size_t count, long timeout)
    -> std::vector<T> {
  std::vector<T> ret{};

  assert(count > 0);

  auto inflightTicket = this->enterInflightGate();

  std::unique_lock<std::mutex> lock(m_mutex);

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Mt<T>::cleanup_thunk_inflight,
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
      // do nothing and fetch zero
    } else {
      // do nothing and fetch whatever we have
    }
  } else {
    m_not_empty_cond.wait(lock, [this, count] {
      return m_queue.size() >= count || this->isInflightGuardClosed();
    });
  }

  // Collect up to 'count' items (moved out).
  while (count > 0 && (!m_queue.empty())) {
    ret.push_back(std::move_if_noexcept(m_queue.front()));
    m_queue.pop_front();
    ++m_pop_count;

    count--;
  }

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
auto Dmn_BlockingQueue_Mt<T>::popOptional(bool wait) -> std::optional<T> {
  std::unique_lock<std::mutex> lock(m_mutex);

  std::optional<T> ret{};

  auto inflightTicket = this->enterInflightGate();

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Mt<T>::cleanup_thunk_inflight,
                        &inflightTicket);

  Dmn_Proc::testcancel();

  if (m_queue.empty()) {
    if (wait) {
      // Block until an item is available. This wait is a cancellation point.
      m_not_empty_cond.wait(lock, [this] {
        return !m_queue.empty() || this->isInflightGuardClosed();
      });

      if (!this->isInflightGuardClosed()) {
        ret = std::move_if_noexcept(m_queue.front());
        m_queue.pop_front();
        ++m_pop_count;
      }
    }
  } else {
    ret = std::move_if_noexcept(m_queue.front());
    m_queue.pop_front();
    ++m_pop_count;
  }

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

#endif // DMN_BLOCKINGQUEUE_MT_HPP_
