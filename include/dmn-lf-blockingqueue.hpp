/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue.hpp
 * @brief Thread-safe FIFO blocking queue for passing items between threads.
 *
 * Overview
 * --------
 * Dmn_Lf_BlockingQueue<T> is a thread-safe FIFO queue implementation intended
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
 */

#ifndef DMN_LF_BUFFER_HPP_
#define DMN_LF_BUFFER_HPP_

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace dmn {

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string> class Dmn_Lf_BlockingQueue {
public:
  Dmn_Lf_BlockingQueue();
  Dmn_Lf_BlockingQueue(std::initializer_list<T> list);
  virtual ~Dmn_Lf_BlockingQueue() noexcept;

  Dmn_Lf_BlockingQueue(const Dmn_Lf_BlockingQueue<T> &obj) = delete;
  const Dmn_Lf_BlockingQueue<T> &
  operator=(const Dmn_Lf_BlockingQueue<T> &obj) = delete;
  Dmn_Lf_BlockingQueue(const Dmn_Lf_BlockingQueue<T> &&obj) = delete;
  Dmn_Lf_BlockingQueue<T> &operator=(Dmn_Lf_BlockingQueue<T> &&obj) = delete;

protected:
private:
}; // class Dmn_Lf_BlockingQueue

template <typename T> Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue() {}

template <typename T>
Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue(std::initializer_list<T> list)
    : Dmn_Lf_BlockingQueue{} {}

template <typename T>
Dmn_Lf_BlockingQueue<T>::~Dmn_Lf_BlockingQueue() noexcept try {
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

} // namespace dmn

#endif // DMN_LF_BUFFER_HPP_
