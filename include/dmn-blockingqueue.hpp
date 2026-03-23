/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue.hpp
 * @brief Thread-safe FIFO blocking queue interface for passing items between
 * threads.
 *
 * Design pattern
 * --------------
 * - Degenerate Bridge : the blocking queue interface is abstracted from the
 *   underlying implementation (mutex lock or lock-free), this is not a
 *   full-fledge bridge as written in gang of 5, but simplified version with
 *   CRTP template with degenerate bridge where abstraction and implemention
 *   are in one hierarchy tree.
 *
 * Static polymorphism
 * -------------------
 * We use Curiously Recurring Template Pattern (CRTP) to achieve static
 * polymorphism that effectively offsetting the runtime overhead of vtable
 * lookup by moving the function dispatch to compile time.
 *
 * Move and copy behavior
 * ----------------------
 * - push(const T&): Accepts a const lvalue and enqueues it by copy. Passing
 *   a plain lvalue such as `push(item)` will bind to this overload and copy the
 *   item into the queue.
 *
 * - push(T&&): Accepts an rvalue and enqueues it using move semantics when
 *   possible. Concrete implementations may employ strategies such as
 *   `std::move_if_noexcept`, which can fall back to copying for types with
 *   throwing move constructors. To move from an lvalue, call
 *   `push(std::move(item))` so that this rvalue overload is selected.
 */

#ifndef DMN_BLOCKINGQUEUE_HPP_
#define DMN_BLOCKINGQUEUE_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dmn {

/**
 * @brief The Dmn_BlockingQueue are top tier class for both the abstraction
 * and implementor classes in the bridge design pattern with support of CRTP
 * to achieve static polymorphism.
 *
 * @details
 * - There are a set of primitive methods to be overridden by concrete
 *   implementation subclass, and those methods are marked as virtual without
 *   final. Those methods are defined in this class with CRTP to achieve
 *   static polymorphism to call to subclass overridden methods. Those are
 *   methods in the implementaion tree in the Bridge design pattern.
 *
 * - There are a set of composite methods that are marked as final and
 *   implemented in term of primitive methods. Those are methods in the
 *   the abstraction tree in Bridge design pattern.
 */
template <typename Derived, typename T> class Dmn_BlockingQueue {
public:
  virtual ~Dmn_BlockingQueue() = default;

  /**
   * @brief Remove and return the front item, waiting until an item is
   * available.
   *
   * @details
   * If the queue is empty, this call blocks or waits until either:
   * - an item is pushed, in which case that item is removed and returned, or
   * - shutdown begins, then the method throws an exception.
   *
   * @return The dequeued item.
   *
   * @throws std::runtime_error If shutdown begins while waiting and no item is
   * returned.
   */
  virtual auto pop() -> T final;

  /**
   * @brief Remove and return up to @p count items, optionally waiting.
   *
   * @param count   Maximum number of items to pop.
   * @param timeout Optional timeout in milliseconds. A value of 0 indicates
   * an implementation-defined behavior (such as wait indefinitely).
   *
   * @return A vector containing the dequeued items (possibly fewer than @p
   * count, depending on availability and timeout semantics).
   */
  virtual auto pop(std::size_t count, long timeout = 0) -> std::vector<T>;

  /**
   * @brief Attempt to pop a single item without waiting.
   *
   * @return The dequeued item, or std::nullopt if the queue was empty or
   * shutdown has detached the queue.
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
   * semantics.
   *
   * @param item The rvalue item to be enqueued. Implementations may
   * internally fall back to copying (e.g., when using std::move_if_noexcept for
   * types with throwing move constructors).
   */
  virtual void push(T &&item) final;

  /**
   * @brief Flag the m_shutdown flag and shutdown the object to prevent further
   * use prior to initializing the teardown.
   */
  virtual void shutdown();

  /**
   * @brief Caller blocks for the queue to be empty and returns the number of
   * items that have been processed through the queue.
   *
   * @return The number of items that have been processed through the queue.
   */
  virtual auto waitForEmpty() -> std::uint64_t {
    return static_cast<Derived *>(this)->waitForEmpty();
  }

protected:
  virtual auto isShutdown() -> bool {
    return m_shutdown_flag.test(std::memory_order_acquire);
  }

  // The following virtual methods are implemented by the subclasses and
  // they form the concrete details for the methods called by clients.
  virtual auto popOptional(bool wait) -> std::optional<T> {
    return static_cast<Derived *>(this)->popOptional(wait);
  }

  virtual void pushCopy(const T &item) {
    static_cast<Derived *>(this)->pushCopy(item);
  }

  virtual void pushMove(T &&item) {
    static_cast<Derived *>(this)->pushMove(std::move(item));
  }

private:
  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_BlockingQueue

template <typename Derived, typename T>
inline auto Dmn_BlockingQueue<Derived, T>::pop(std::size_t count, long timeout)
    -> std::vector<T> {
  return static_cast<Derived *>(this)->pop(count, timeout);
}

template <typename Derived, typename T>
inline auto Dmn_BlockingQueue<Derived, T>::pop() -> T {
  auto data = static_cast<Derived *>(this)->popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename Derived, typename T>
inline auto Dmn_BlockingQueue<Derived, T>::popNoWait() -> std::optional<T> {
  return static_cast<Derived *>(this)->popOptional(false);
}

template <typename Derived, typename T>
inline void Dmn_BlockingQueue<Derived, T>::push(const T &item) {
  static_cast<Derived *>(this)->pushCopy(item);
}

template <typename Derived, typename T>
inline void Dmn_BlockingQueue<Derived, T>::push(T &&item) {
  static_cast<Derived *>(this)->pushMove(std::move(item));
}

template <typename Derived, typename T>
inline void Dmn_BlockingQueue<Derived, T>::shutdown() {
  m_shutdown_flag.test_and_set(std::memory_order_release);
}

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_HPP_
