/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-interface.hpp
 * @brief Thread-safe FIFO blocking queue interface for passing items between
 * threads.
 *
 * Design pattern
 * --------------
 * - Proxy    : the blocking queue interface to both dmn-blockingqueue and
 *              dmn-blockingqueue-lf (lock-free).
 * - Bridge   : the blocking queue interface is abstracted from the underlying
 *              implementation (mutex lock or lock-free).
 *
 * Static polymorphism
 * -------------------
 * We use Curiously Recurring Template Pattern (CRTP) to achieve static
 * polymorphism that effectively offsetting the runtime overheadof vtable lookup
 * by moving the function dispatch to compile time.
 *
 * Move and copy behavior
 * ----------------------
 * - push(const T&): Accepts an lvalue (const or non-const) and enqueues it
 *   by copy. Passing a plain lvalue such as `push(item)` will bind to this
 *   overload and copy the item into the queue.
 *
 * - push(T&&): Accepts an rvalue and enqueues it using move semantics when
 *   possible. Concrete implementations may employ strategies such as
 *   `std::move_if_noexcept`, which can fall back to copying for types with
 *   throwing move constructors. To move from an lvalue, call
 *   `push(std::move(item))` so that this rvalue overload is selected.
 */

#ifndef DMN_BLOCKINGQUEUE_INTERFACE_HPP_
#define DMN_BLOCKINGQUEUE_INTERFACE_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dmn {

template <typename Derived, typename T> class Dmn_BlockingQueue_Interface {
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
   * @brief Enqueue an rvalue item into the tail of the queue, preferring move
   *        semantics.
   *
   * @param item The rvalue item to be enqueued. Implementations may
   *        internally fall back to copying (e.g., when using
   *        std::move_if_noexcept for types with throwing move constructors).
   */
  virtual void push(T &&item) final;

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   *        items that have passed through the queue.
   *
   * @return The total number of items that have been passed through the queue.
   */
  virtual auto waitForEmpty() -> std::uint64_t = 0;

  // The following virtual methods are implemented by the subclasses.
  virtual auto popOptional(bool wait) -> std::optional<T> = 0;

  virtual void pushCopy(const T &item) = 0;

  virtual void pushMove(T &&item) = 0;

  virtual void shutdown();

  virtual auto isShutdown() -> bool {
    return m_shutdown_flag.test(std::memory_order_acquire);
  }

  std::atomic_flag m_shutdown_flag{};
};

template <typename Derived, typename T>
auto Dmn_BlockingQueue_Interface<Derived, T>::pop() -> T {
  auto data = static_cast<Derived *>(this)->popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename Derived, typename T>
auto Dmn_BlockingQueue_Interface<Derived, T>::popNoWait() -> std::optional<T> {
  return static_cast<Derived *>(this)->popOptional(false);
}

template <typename Derived, typename T>
void Dmn_BlockingQueue_Interface<Derived, T>::push(T &&item) {
  static_cast<Derived *>(this)->pushMove(std::move(item));
}

template <typename Derived, typename T>
void Dmn_BlockingQueue_Interface<Derived, T>::push(const T &item) {
  static_cast<Derived *>(this)->pushCopy(item);
}

template <typename Derived, typename T>
void Dmn_BlockingQueue_Interface<Derived, T>::shutdown() {
  m_shutdown_flag.test_and_set(std::memory_order_release);
}

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_INTERFACE_HPP_
