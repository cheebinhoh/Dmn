/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-interface.hpp
 * @brief Thread-safe FIFO blocking queue interfac for passing items between
 * threads.
 */

#ifndef DMN_BLOCKINGQUEUE_INTERFACE_HPP_
#define DMN_BLOCKINGQUEUE_INTERFACE_HPP_

namespace dmn {

template <typename T> class Dmn_BlockingQueue_Interface {
public:
  virtual ~Dmn_BlockingQueue_Interface() = default;

  virtual T pop() = 0;
  virtual std::vector<T> pop(std::size_t count, long timeout = 0) = 0;
  virtual std::optional<T> popNoWait() = 0;

  virtual void push(T &&item) = 0;
  virtual void push(T &item, bool move = true) = 0;

  virtual std::uint64_t waitForEmpty() = 0;

protected:
  virtual void stop() = 0;
};

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_INTERFACE_HPP_
