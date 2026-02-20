/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-io.hpp
 * @brief Generic IO interface used by the Dmn library.
 *
 * Dmn_Io<T> declares a minimal, transport-agnostic interface for reading and
 * writing values of type T. Implementations can represent files, pipes,
 * network sockets, message queues, or other data sources/sinks.
 *
 * Semantics:
 *  - read(): Returns the next available item wrapped in std::optional<T>.
 *    The call is expected to block until data becomes available in the
 *    normal case. If the underlying data source reaches end-of-stream
 *    (for example EOF or a closed pipe) the function returns std::nullopt to
 *    signal that no further data will be delivered. Concrete implementations
 *    may use timeouts or non-blocking strategies when appropriate, but callers
 *    should rely on std::nullopt to detect end-of-stream.
 *
 *  - write(T &item): Takes an lvalue reference. This overload does not take
 *    ownership of the provided object; implementations SHOULD copy the value
 *    if they need to retain it.
 *
 *  - write(T &&item): Takes an rvalue reference. Implementations SHOULD move
 *    from the item when possible to avoid unnecessary copies.
 *
 * Thread-safety:
 *  - The interface itself does not mandate any concurrency guarantees. If an
 *    implementation is safe to call from multiple threads concurrently, it
 *    MUST document those guarantees.
 */

#ifndef DMN_IO_HPP_
#define DMN_IO_HPP_

#include <optional>
#include <vector>

namespace dmn {

template <typename T> class Dmn_Io {
public:
  virtual ~Dmn_Io() noexcept = default;

  /**
   * @brief Read and return the next available item.
   *
   * The call blocks until data is available. Returns std::nullopt to
   * signal end-of-stream (e.g. EOF, closed pipe, or unrecoverable
   * error); callers should treat std::nullopt as a permanent stop
   * condition and cease further reads.
   *
   * @return optional<T> containing the next item, or std::nullopt on
   *         end-of-stream.
   */
  virtual auto read() -> std::optional<T> = 0;

  /**
   * @brief Read up to @p count items, optionally waiting up to
   *        @p timeout microseconds.
   *
   * The default implementation returns an empty vector. Concrete
   * subclasses may override this to provide bulk-read semantics
   * consistent with Dmn_BlockingQueue::pop(count, timeout).
   *
   * @param count   Maximum number of items to return (must be > 0).
   * @param timeout Maximum wait time in microseconds; 0 means wait
   *                indefinitely.
   * @return Vector of up to @p count items (possibly fewer on
   *         timeout).
   */
  virtual auto read([[maybe_unused]] size_t count,
                    [[maybe_unused]] long timeout = 0) -> std::vector<T> {
    return {};
  }

  /**
   * @brief Write (copy) an item to the sink.
   *
   * The lvalue overload; implementations SHOULD copy @p item if they
   * need to retain it beyond the call.
   *
   * @param item The item to write.
   */
  virtual void write(T &item) = 0;

  /**
   * @brief Write (move) an item to the sink.
   *
   * The rvalue overload; implementations SHOULD move from @p item to
   * avoid an unnecessary copy.
   *
   * @param item The item to write (may be moved from).
   */
  virtual void write(T &&item) = 0;
};

} // namespace dmn

#endif // DMN_IO_HPP_
