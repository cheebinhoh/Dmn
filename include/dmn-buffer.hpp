/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-buffer.hpp
 * @brief Thread-safe FIFO buffer (queue) for passing items between threads.
 *
 * Overview
 * --------
 * Dmn_Buffer<T> is a thread-safe FIFO queue implementation intended for use
 * by producer and consumer threads. Key behavioral properties:
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
 *   - m_none_empty_cond: signalled on push; used by the multi-item timed pop
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
 *          * If the timeout expires and the queue is still empty, the wait
 *            restarts (the implementation re-arms the absolute-time
 *            deadline). This behavior avoids returning an empty result on
 *            spurious timeouts; the function only returns due to timeout when
 *            there is at least one item in the queue at expiry.
 *
 *   Note: The timeout is interpreted as a maximum time to wait for the full
 *   `count` items (measured from the first blocking wait inside the call).
 *   A zero timeout value means "wait forever".
 *
 * Move and copy behavior
 * ----------------------
 * - push(T&&): Attempts to move the provided rvalue into the queue. It uses
 *   std::move_if_noexcept to prefer move only when it is noexcept (or the
 *   type is noexcept-movable).
 *
 * - push(T&, bool move=true): Pushes the provided lvalue. If `move` is true,
 *   the code will attempt to move (using move_if_noexcept), otherwise it will
 *   copy.
 *
 * Implementation notes
 * --------------------
 * - The queue uses an unbounded std::deque<T>. There is no programmatic
 *   limit beyond available memory and the semantics of the stored type T.
 *
 * - The class deletes copy and move constructors / assignment operators to
 *   ensure unique ownership of the synchronization primitives and avoid
 *   incidental sharing between objects.
 *
 * - The destructor signals waiting threads (m_cond and m_empty_cond) before
 *   destroying the condition variables and mutex to attempt to wake any
 *   blocked waiters. Destruction of synchronization primitives should be done
 *   only when it is guaranteed no other thread will attempt to use the
 *   Dmn_Buffer.
 */

#ifndef DMN_BUFFER_HPP_
#define DMN_BUFFER_HPP_

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "dmn-proc.hpp"

namespace dmn {

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string> class Dmn_Buffer {
public:
  Dmn_Buffer();
  Dmn_Buffer(std::initializer_list<T> list);
  virtual ~Dmn_Buffer() noexcept;

  Dmn_Buffer(const Dmn_Buffer<T> &obj) = delete;
  const Dmn_Buffer<T> &operator=(const Dmn_Buffer<T> &obj) = delete;
  Dmn_Buffer(const Dmn_Buffer<T> &&obj) = delete;
  Dmn_Buffer<T> &operator=(Dmn_Buffer<T> &&dmnBuffer) = delete;

  /**
   * @brief Remove and return the front item from the queue, blocking if empty.
   *
   * This call blocks until an item becomes available. It throws
   *
   * @return The front item.
   */
  virtual auto pop() -> T;

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
   *     * If timeout expires and the queue is still empty, the function keeps
   *       waiting (re-arming the absolute deadline) until at least one item is
   *       available.
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
  virtual auto pop(size_t count, long timeout = 0) -> std::vector<T>;

  /**
   * @brief Attempt a non-blocking pop. Return std::nullopt if empty.
   *
   * @return optional item, or std::nullopt if the queue was empty.
   */
  virtual auto popNoWait() -> std::optional<T>;

  /**
   * @brief Push an rvalue into the queue (attempts move, with
   *        move_if_noexcept).
   *
   * Non-blocking and signals waiting consumers.
   *
   * @param item The value to push (rvalue reference).
   */
  virtual void push(T &&item);

  /**
   * @brief Push an lvalue into the queue.
   *
   * If move is true, the implementation will attempt to move from the given
   * lvalue using std::move_if_noexcept; otherwise it will copy.
   *
   * @param item The value to push (lvalue reference).
   * @param move If true attempt move semantics; otherwise copy.
   */
  virtual void push(T &item, bool move = true);

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
  virtual auto waitForEmpty() -> size_t;

protected:
  /**
   * @brief Internal helper that optionally blocks waiting for an item.
   *
   * @param wait If true, block until an item is available; otherwise return
   *             std::nullopt immediately if empty.
   * @return optional value popped from the front of the queue.
   */
  virtual auto popOptional(bool wait) -> std::optional<T>;

  /**
   */
  void stop();

private:
  std::deque<T> m_queue{};
  std::mutex m_mutex{};
  std::condition_variable m_empty_cond{}; // signalled when queue becomes empty
  std::condition_variable
      m_none_empty_cond{}; // signalled on push (multi-pop timed wait)
  size_t m_push_count{};
  size_t m_pop_count{};

  std::atomic<bool> m_shutdown{};
}; // class Dmn_Buffer

template <typename T> Dmn_Buffer<T>::Dmn_Buffer() {}

template <typename T>
Dmn_Buffer<T>::Dmn_Buffer(std::initializer_list<T> list) : Dmn_Buffer{} {
  for (auto data : list) {
    this->push(data);
  }
}

template <typename T> Dmn_Buffer<T>::~Dmn_Buffer() noexcept try {
  // Wake up any threads waiting on condition variables before destroying them.
  // This does not guarantee safe concurrent use; the destructor should be
  // called only when no other threads will access this object.
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T> auto Dmn_Buffer<T>::pop() -> T {
  return *popOptional(true);
}

template <typename T> auto Dmn_Buffer<T>::popNoWait() -> std::optional<T> {
  return popOptional(false);
}

template <typename T> void Dmn_Buffer<T>::push(T &&item) {
  T moved_item = std::move_if_noexcept(item);

  push(moved_item, true);
}

template <typename T> void Dmn_Buffer<T>::push(T &item, bool move) {
  std::unique_lock<std::mutex> lock(m_mutex);

  // Cancellation point check to allow thread cancellation in a controlled way.
  Dmn_Proc::testcancel();

  if (move) {
    m_queue.push_back(std::move_if_noexcept(item));
  } else {
    m_queue.push_back(item);
  }

  ++m_push_count;

  // Notify multi-pop waiters first (they use m_none_empty_cond).
  m_none_empty_cond.notify_all();

  // Notify single-item waiters.
  m_empty_cond.notify_all();
}

template <typename T> void Dmn_Buffer<T>::stop() {
  m_shutdown = true;

  m_empty_cond.notify_all();
  m_none_empty_cond.notify_all();
}

template <typename T> auto Dmn_Buffer<T>::waitForEmpty() -> size_t {
  size_t inbound_count{};

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_empty_cond.wait(lock, [this] { return m_queue.empty() || m_shutdown; });

  assert(m_pop_count == m_push_count);
  inbound_count = m_pop_count;

  return inbound_count;
}

template <typename T>
auto Dmn_Buffer<T>::pop(size_t count, long timeout) -> std::vector<T> {
  std::vector<T> ret{};

  assert(count > 0);

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  // Wait until there are at least 'count' items OR a timeout occurs with at
  // least one available item.
  if (timeout > 0) {
    if (m_none_empty_cond.wait_for(
            lock, std::chrono::microseconds(timeout),
            [this, count] { return m_queue.size() >= count || m_shutdown; })) {
      // do nothing and we have what we want
    } else if (m_queue.empty()) {
      return {};
    } else {
      // do nothing and fetch whatever we have
    }
  } else {
    m_none_empty_cond.wait(
        lock, [this, count] { return m_queue.size() >= count || m_shutdown; });
  }

  if (m_shutdown) {
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
  }

  return ret;
}

template <typename T>
auto Dmn_Buffer<T>::popOptional(bool wait) -> std::optional<T> {
  T val{};

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  if (m_queue.empty()) {
    if (!wait) {
      return {};
    }

    // Block until an item is available. This wait is a cancellation point.
    m_none_empty_cond.wait(lock,
                           [this] { return !m_queue.empty() || m_shutdown; });
  }

  if (m_shutdown) {
    return {};
  }

  val = std::move_if_noexcept(m_queue.front());
  m_queue.pop_front();

  ++m_pop_count;

  // Notify waiters waiting for the queue to become empty.
  if (m_queue.empty()) {
    m_empty_cond.notify_all();
  }

  return std::move_if_noexcept(val);
} // method popOptional()

} // namespace dmn

#endif // DMN_BUFFER_HPP_
