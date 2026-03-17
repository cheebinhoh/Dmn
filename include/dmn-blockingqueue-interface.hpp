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
 * - push(const T&): Accepts an lvalue (const or non-const) and enqueues it
 *   by copy. Passing a plain lvalue such as `push(item)` will bind to this
 *   overload and copy the item into the queue.
 *
 * - push(T&&): Accepts an rvalue and enqueues it with move semantics. To
 *   move from an lvalue, call `push(std::move(item))` so that this rvalue
 *   overload is selected.
 */

#ifndef DMN_BLOCKINGQUEUE_INTERFACE_HPP_
#define DMN_BLOCKINGQUEUE_INTERFACE_HPP_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
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
   * If the queue is empty, this call blocks or waits until either:
   * - an item is pushed, in which case that item is removed and returned, or
   * - shutdown begins, then the method throws an exception.
   *
   * @return The dequeued item.
   *
   * @throws std::runtime_error
   *         If shutdown begins while waiting and no item is returned.
   */
  virtual auto pop() -> T final;

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
  virtual auto popNoWait() -> std::optional<T> final;

  /**
   * @brief Copy and enqueue the const lvalue item into the tail of the queue.
   *
   * @param item The const lvalue item to be enqueued.
   */
  virtual void push(const T &item) final;

  /**
   * @brief Move and enqueue the rvalue item into the tail of the queue.
   *
   * @param item The rvalue item to be enqueued.
   */
  virtual void push(T &&item) final;

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   *        items that have passed through the queue.
   *
   * @return The total number of items that have been passed through the queue.
   */
  virtual auto waitForEmpty() -> std::uint64_t = 0;

protected:
  virtual auto popOptional(bool wait) -> std::optional<T> = 0;

  virtual void pushCopy(const T &item) = 0;

  virtual void pushMove(T &&item) = 0;

  /**
   * @brief Legacy hook that dispatches to pushCopy or pushMove based on @p
   * move.
   *
   * @param item The item to be enqueued.
   * @param move If true, enqueue by move; otherwise, enqueue by copy.
   */
  virtual void push(T &item, bool move);

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
  pushMove(std::move(item));
}

template <typename T> void Dmn_BlockingQueue_Interface<T>::push(const T &item) {
  pushCopy(item);
}

template <typename T>
void Dmn_BlockingQueue_Interface<T>::push(T &item, bool move) {
  if (move) {
    pushMove(std::move(item));
  } else {
    pushCopy(item);
  }
}

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_INTERFACE_HPP_
