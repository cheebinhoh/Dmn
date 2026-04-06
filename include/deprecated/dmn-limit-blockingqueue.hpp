/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-limit-blockingqueue.hpp
 * @brief Bounded, thread-safe FIFO queue with blocking push/pop semantics.
 *
 * This header defines Dmn_Limit_BlockingQueue<T>, a thread-safe,
 * limited-capacity FIFO queue built on top of Dmn_BlockingQueue<T>. The buffer
 * provides:
 *  - A maximum capacity set at construction time. Attempts to push when the
 *    buffer is full will block the caller until space becomes available.
 *  - Blocking pop() that waits until an item is available and returns it.
 *  - popNoWait() which returns std::nullopt immediately if the buffer is
 *    empty (non-blocking).
 *  - push overloads that prefer move semantics when available; push(T&&) will
 *    use move-if-noexcept and forward to the internal push implementation.
 *  - size() to return the current number of stored items (snapshot).
 *  - waitForEmpty() delegates to Dmn_BlockingQueue<T>::waitForEmpty().
 *
 * Notes:
 *  - Dmn_Limit_BlockingQueue<T> privately inherits from Dmn_BlockingQueue<T>
 * and reuses its internal storage/semantics for push/pop operations.
 *  - All operations aim for O(1) behavior with respect to queue operations.
 *  - The size() method returns a snapshot and is protected by the mutex to
 *    ensure a consistent view.
 */

#ifndef DMN_LIMITBUFFER_HPP_
#define DMN_LIMITBUFFER_HPP_

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "dmn-blockingqueue.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T>
class Dmn_Limit_BlockingQueue : private Dmn_BlockingQueue<T> {
public:
  /** @brief Construct a bounded blocking queue with the given maximum capacity.
   * @param capacity Maximum number of items the queue may hold simultaneously.
   */
  explicit Dmn_Limit_BlockingQueue(size_t capacity = 1);

  /// @brief Destroy the queue; frees internal resources.
  virtual ~Dmn_Limit_BlockingQueue();

  Dmn_Limit_BlockingQueue(const Dmn_Limit_BlockingQueue<T> &obj) = delete;
  Dmn_Limit_BlockingQueue<T> &
  operator=(const Dmn_Limit_BlockingQueue<T> &obj) = delete;
  Dmn_Limit_BlockingQueue(Dmn_Limit_BlockingQueue<T> &&obj) = delete;
  Dmn_Limit_BlockingQueue<T> &&
  operator=(Dmn_Limit_BlockingQueue<T> &&obj) = delete;

  /**
   * @brief Pop and return the front item, blocking until one is available.
   *
   * @return The front item of the queue.
   */
  auto pop() -> T;

  /**
   * @brief Return @c true if the queue contains no items.
   *
   * @return @c true when @c m_size is 0, @c false otherwise.
   */
  auto empty() -> bool;

  /**
   * @brief Pop and return the front item without blocking.
   *
   * @return An optional containing the front item, or @c std::nullopt if
   *         the queue is empty.
   */
  auto popNoWait() -> std::optional<T>;

  /**
   * @brief Enqueue an rvalue item, preferring move semantics.
   *
   * Blocks if the queue is at maximum capacity until space becomes available.
   *
   * @param item The item to enqueue (moved when the move constructor is
   *             noexcept).
   */
  void push(T &&item);

  /**
   * @brief Enqueue an lvalue item, optionally using move semantics.
   *
   * Blocks if the queue is at maximum capacity until space becomes available.
   *
   * @param item The item to enqueue.
   * @param move If @c true, move @p item into the queue; otherwise copy it.
   */
  void push(T &item, bool move = true);

  /**
   * @brief Return a snapshot of the current number of items in the queue.
   *
   * @return The number of items currently held in the queue.
   */
  auto size() -> size_t;

  /**
   * @brief Block until the queue is empty and return the total number of items
   *        that have passed through it.
   *
   * @return The cumulative number of items that have been pushed and popped.
   */
  auto waitForEmpty() -> uint64_t override;

private:
  /**
   * @brief Internal pop helper that optionally blocks waiting for an item.
   *
   * @param wait If @c true, block until an item is available; if @c false,
   *             return @c std::nullopt immediately when the queue is empty.
   *
   * @return An optional containing the front item, or @c std::nullopt.
   */
  auto popOptional(bool wait) -> std::optional<T> override;

private:
  size_t m_max_capacity{1};          ///< Maximum number of items the queue may hold.
  size_t m_size{0};                   ///< Current number of items in the queue.
  std::mutex m_mutex{};               ///< Protects all access to @c m_size and the underlying storage.
  std::condition_variable m_pop_cond{};  ///< Signalled when a new item is available for consumers.
  std::condition_variable m_push_cond{}; ///< Signalled when a slot becomes available for producers.
}; // class Dmn_Limit_BlockingQueue

template <typename T>
Dmn_Limit_BlockingQueue<T>::Dmn_Limit_BlockingQueue(size_t capacity)
    : m_max_capacity(capacity) {}

template <typename T> Dmn_Limit_BlockingQueue<T>::~Dmn_Limit_BlockingQueue() {}

template <typename T> auto Dmn_Limit_BlockingQueue<T>::pop() -> T {
  return *popOptional(true);
}

template <typename T> auto Dmn_Limit_BlockingQueue<T>::empty() -> bool {
  return this->size() <= 0;
}

template <typename T>
auto Dmn_Limit_BlockingQueue<T>::popNoWait() -> std::optional<T> {
  return popOptional(false);
}

template <typename T> void Dmn_Limit_BlockingQueue<T>::push(T &&item) {
  T moved_item = std::move_if_noexcept(item);

  push(moved_item, true);
}

template <typename T>
void Dmn_Limit_BlockingQueue<T>::push(T &item, bool move) {
  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_push_cond.wait(lock, [this] { return m_size < m_max_capacity; });

  Dmn_BlockingQueue<T>::push(item, move);
  ++m_size;

  lock.unlock();
  m_pop_cond.notify_all();
}

template <typename T> auto Dmn_Limit_BlockingQueue<T>::size() -> size_t {
  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  return m_size;
}

template <typename T>
auto Dmn_Limit_BlockingQueue<T>::waitForEmpty() -> uint64_t {
  return Dmn_BlockingQueue<T>::waitForEmpty();
}

template <typename T>
auto Dmn_Limit_BlockingQueue<T>::popOptional(bool wait) -> std::optional<T> {
  std::optional<T> val{};

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  val = Dmn_BlockingQueue<T>::popOptional(wait);
  m_size--;

  lock.unlock();
  m_push_cond.notify_all();

  return val; // val is local variable, hence rvalue and hence move semantic
              // by default for efficient copy.
}

} // namespace dmn

#endif // DMN_LIMITBUFFER_HPP_
