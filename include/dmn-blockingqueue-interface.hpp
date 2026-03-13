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
#include <vector>

namespace dmn {

template <typename T> class Dmn_BlockingQueue_Interface {
public:
  virtual ~Dmn_BlockingQueue_Interface() = default;

  virtual T pop() = 0;
  virtual std::vector<T> pop(std::size_t count, long timeout = 0) = 0;
  virtual std::optional<T> popNoWait() = 0;

  virtual void push(T &&item) { push(item, true); }

  void push(const T &item) {
    T copied = item;

    push(copied, false);
  }

  void push(T &item) { push(item, true); }

  virtual std::uint64_t waitForEmpty() = 0;

protected:
  virtual void push(T &item, bool move) = 0;

  virtual void stop() = 0;
};

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_INTERFACE_HPP_
