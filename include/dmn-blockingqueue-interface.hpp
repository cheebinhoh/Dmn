/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-interface.hpp
 * @brief Thread-safe FIFO blocking queue interface for passing items between
 * threads.
 *
 * Design pattern
 * - Proxy    : the blocking queue interface to both dmn-blockingqueue and
 *              dmn-blockingqueue-lf (lock-free).
 * - Bridge   : the blocking queue interface is abstracted from the underlying
 *              implementation (mutex lock or lock-free).
 *
 * Move and copy behavior
 * ----------------------
 * - push(T&&): Accepts an rvalue and enqueues it with move semantics. Callers
 *   typically use `push(std::move(item))` on an lvalue to request moving.
 *
 * - push(T&): Accepts a non-const lvalue and enqueues it by copy. If you want
 *   to move from an lvalue, call `push(std::move(item))` so that the rvalue
 *   overload is selected instead.
 *
 * - push(const T&): Accepts a const lvalue and always enqueues a copy of the
 *   provided item.
 */

#ifndef DMN_BLOCKINGQUEUE_INTERFACE_HPP_
#define DMN_BLOCKINGQUEUE_INTERFACE_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace dmn {

template <typename T> class Dmn_BlockingQueue_Interface {
public:
  virtual ~Dmn_BlockingQueue_Interface() = default;

  /**
   * @brief Remove and return the front item, waiting until an item is
   *        available.
   *
   * @details
   * If the queue is empty, this call waits (by yielding) until either:
   * - an item is pushed, in which case that item is removed and returned, or
   * - shutdown begins, then the method throws an exception.
   *
   * @return The dequeued item.
   *
   * @throws std::runtime_error
   *         If shutdown begins while waiting and no item is returned.
   */
  auto pop() -> T;

  /**
   * @brief Remove and return up to @p count items, optionally waiting.
   *
   * @param count   Maximum number of items to pop.
   * @param timeout Optional timeout in milliseconds. A value of 0 indicates
   *                an implementation-defined behavior (such as wait
   *                indefinitely).
   *
   * @return A vector containing the dequeued items (possibly fewer than
   *         @p count, depending on availability and timeout semantics).
   */
  virtual auto pop(std::size_t count, long timeout = 0) -> std::vector<T> = 0;

  /**
   * @brief Attempt to pop a single item without waiting.
   *
   * @return The dequeued item, or std::nullopt if the queue was empty or
   *         shutdown has detached the queue.
   */
  auto popNoWait() -> std::optional<T>;

  /**
   * @brief Copy and enqueue the lvalue item into the tail of the queue.
   *
   * @param item The lvalue item to be enqueued.
   */
  void push(T &item);

  /**
   * @brief Copy and enqueue the const lvalue item into the tail of the queue.
   *
   * @param item The const lvalue item to be enqueued.
   */
  void push(const T &item);

  /**
   * @brief Move and enqueue the rvalue item into the tail of the queue.
   *
   * @param item The rvalue item to be enqueued.
   */
  void push(T &&item);

  virtual std::uint64_t waitForEmpty() = 0;

protected:
  virtual void push(T &item, bool move) = 0;
  virtual auto popOptional(bool wait) -> std::optional<T> = 0;

  virtual void stop() = 0;
};

template <typename T> auto Dmn_BlockingQueue_Interface<T>::pop() -> T {
  auto data = popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename T>
auto Dmn_BlockingQueue_Interface<T>::popNoWait() -> std::optional<T> {
  return popOptional(false);
}

template <typename T> void Dmn_BlockingQueue_Interface<T>::push(T &&item) {
  push(item, true);
}

template <typename T> void Dmn_BlockingQueue_Interface<T>::push(const T &item) {
  T copied = item;

  push(copied, false);
}

template <typename T> void Dmn_BlockingQueue_Interface<T>::push(T &item) {
  push(item, false);
}

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_INTERFACE_HPP_
