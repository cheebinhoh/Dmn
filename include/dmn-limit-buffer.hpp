/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-limit-buffer.hpp
 * @brief Bounded, thread-safe FIFO queue with blocking push/pop semantics.
 *
 * This header defines Dmn_LimitBuffer<T>, a thread-safe, limited-capacity
 * FIFO queue built on top of Dmn_Buffer<T>. The buffer provides:
 *  - A maximum capacity set at construction time. Attempts to push when the
 *    buffer is full will block the caller until space becomes available.
 *  - Blocking pop() that waits until an item is available and returns it.
 *  - popNoWait() which returns std::nullopt immediately if the buffer is
 *    empty (non-blocking).
 *  - push overloads that prefer move semantics when available; push(T&&) will
 *    use move-if-noexcept and forward to the internal push implementation.
 *  - size() to return the current number of stored items (snapshot).
 *  - waitForEmpty() delegates to Dmn_Buffer<T>::waitForEmpty().
 *
 * Notes:
 *  - Dmn_LimitBuffer<T> privately inherits from Dmn_Buffer<T> and reuses its
 *    internal storage/semantics for push/pop operations.
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

#include "dmn-buffer.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T> class Dmn_LimitBuffer : private Dmn_Buffer<T> {
public:
  explicit Dmn_LimitBuffer(size_t capacity = 1);
  virtual ~Dmn_LimitBuffer();

  Dmn_LimitBuffer(const Dmn_LimitBuffer<T> &obj) = delete;
  const Dmn_LimitBuffer<T> &operator=(const Dmn_LimitBuffer<T> &obj) = delete;
  Dmn_LimitBuffer(Dmn_LimitBuffer<T> &&obj) = delete;
  Dmn_LimitBuffer<T> &&operator=(Dmn_LimitBuffer<T> &&obj) = delete;

  /**
   * @brief The method will pop and return front item from the queue or the
   *        caller is blocked waiting if the queue is empty.
   *
   * @return front item of the queue
   */
  auto pop() -> T override;

  /**
   * @brief Return true or false if the m_size is zero.
   *
   * @return True if m_size is 0 or false otherwise.
   */
  auto empty() -> bool;

  /**
   * @brief The method will pop and return front item from the queue or the
   *        std::nullopt if the queue is empty.
   *
   * @return optional item from the front of the queue
   */
  auto popNoWait() -> std::optional<T> override;

  /**
   * @brief The method will push the item into queue using move semantics
   *        unless noexcept is false. The caller is blocked waiting if the
   *        queue is full.
   *
   * @param item The item to be pushed into queue
   */
  void push(T &&item) override;

  /**
   * @brief The method will push the item into queue using move semantics if
   *        move is t true. The caller is blocked waiting if the queue is full.
   *
   * @param item The item to be pushed into queue
   * @param move True if use move semantic or false otherwise
   */
  void push(T &item, bool move = true) override;

  /**
   * @brief The method returns the number of items held in the queue now.
   *
   * @return The number of items held in the queue now
   */
  auto size() -> size_t;

  /**
   * @brief The method will put the client on blocking wait until
   *        the queue is empty, it returns number of items that
   *        were passed through the queue in total.
   *
   * @return The number of items that were passed through the queue
   *         in total
   */
  auto waitForEmpty() -> size_t override;

private:
  /**
   * @brief The method will pop front item from the queue and return it
   *        or block waiting for item if the queue is empty and wait is
   *        true.
   *
   * @param wait The caller is blocked waiting for item if queue is empty
   *             and wait is true, otherwise returning std::nullopt
   *
   * @return optional value from front item of the queue
   */
  auto popOptional(bool wait) -> std::optional<T> override;

private:
  /**
   * data members for constructor to instantiate the object.
   */
  size_t m_max_capacity{1};

  /**
   * data members for internal logic.
   */
  size_t m_size{0};
  std::mutex m_mutex{};
  std::condition_variable m_pop_cond{};
  std::condition_variable m_push_cond{};
}; // class Dmn_LimitBuffer

template <typename T>
Dmn_LimitBuffer<T>::Dmn_LimitBuffer(size_t capacity)
    : m_max_capacity(capacity) {}

template <typename T> Dmn_LimitBuffer<T>::~Dmn_LimitBuffer() {}

template <typename T> auto Dmn_LimitBuffer<T>::pop() -> T {
  return *popOptional(true);
}

template <typename T> auto Dmn_LimitBuffer<T>::empty() -> bool {
  return this->size() <= 0;
}

template <typename T> auto Dmn_LimitBuffer<T>::popNoWait() -> std::optional<T> {
  return popOptional(false);
}

template <typename T> void Dmn_LimitBuffer<T>::push(T &&item) {
  T moved_item = std::move_if_noexcept(item);

  push(moved_item, true);
}

template <typename T> void Dmn_LimitBuffer<T>::push(T &item, bool move) {
  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  m_push_cond.wait(lock, [this] { return m_size < m_max_capacity; });

  Dmn_Buffer<T>::push(item, move);
  ++m_size;

  m_pop_cond.notify_all();
}

template <typename T> auto Dmn_LimitBuffer<T>::size() -> size_t {
  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  return m_size;
}

template <typename T> auto Dmn_LimitBuffer<T>::waitForEmpty() -> size_t {
  return Dmn_Buffer<T>::waitForEmpty();
}

template <typename T>
auto Dmn_LimitBuffer<T>::popOptional(bool wait) -> std::optional<T> {
  std::optional<T> val{};

  std::unique_lock<std::mutex> lock(m_mutex);

  Dmn_Proc::testcancel();

  val = Dmn_Buffer<T>::popOptional(wait);
  m_size--;

  m_push_cond.notify_all();

  return val; // val is local variable, hence rvalue and hence move semantic
              // by default for efficient copy.
}

} // namespace dmn

#endif // DMN_LIMITBUFFER_HPP_
