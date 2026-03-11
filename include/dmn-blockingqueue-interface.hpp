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
 * - Iterator : it abstracts the underlying implementation of aggregate object
 *              (queue).
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

  virtual void push(T &&item) = 0;
  virtual void push(T &item, bool move = true) = 0;

  virtual void push(const T &item) final {
    T copied = item;

    push(std::move(copied));
  }

  virtual std::uint64_t waitForEmpty() = 0;

protected:
  virtual void stop() = 0;
};

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_INTERFACE_HPP_
