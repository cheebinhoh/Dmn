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
 *    The call may block until data becomes available. The meaning of
 *    std::nullopt is implementation-specific; it may indicate end-of-stream,
 *    shutdown, timeout, or an error. Callers must consult the concrete
 *    implementation's contract.
 *
 *  - write(const T &item): Takes a const lvalue reference. This overload does
 *    not take ownership of the provided object; implementations SHOULD copy the
 *    value if they need to retain or change it.
 *
 *  - write(T &&item): Takes an rvalue reference. Implementations SHOULD move
 *    from the item when possible to avoid unnecessary copies.
 *
 *  - shutdown(): Provides a hook that concrete I/O subclasses can use to
 *    perform shutdown procedures and free resources that would otherwise leak
 *    (such as a Kafka consumer thread).
 *
 * Thread-safety:
 *  - The interface itself does not mandate any concurrency guarantees. If an
 *    implementation is safe to call from multiple threads concurrently, it
 *    MUST document those guarantees.
 */

#ifndef DMN_IO_HPP_
#define DMN_IO_HPP_

#include <optional>
#include <utility>
#include <vector>

namespace dmn {

/**
 * @brief Transport-agnostic interface for reading and writing values of type T.
 *
 * @tparam T The item type exchanged through this interface.
 *
 * Concrete implementations may represent files, pipes, network sockets,
 * message queues, or other data sources and sinks.  See the file-level
 * documentation for the full semantics of each method.
 */
template <typename T> class Dmn_Io {
public:
  virtual ~Dmn_Io() noexcept { shutdown(); }

  /**
   * @brief Read and return the next available item.
   *
   * The call may block until data is available. The meaning of std::nullopt
   * is implementation-specific and must be defined by subclasses.
   *
   * @return optional<T> containing the next item, or std::nullopt on
   *         the implementation-specific no-value condition.
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
  virtual void write(const T &item) = 0;

  /**
   * @brief Write (move) an item to the sink.
   *
   * The rvalue overload; implementations SHOULD move from @p item to
   * avoid an unnecessary copy.
   *
   * @param item The item to write (may be moved from).
   */
  virtual void write(T &&item) = 0;

  /**
   * @brief Shut down the I/O object and prevent further use to facilitate
   *        object teardown.
   */
  virtual void shutdown() {}
};

} // namespace dmn

#endif // DMN_IO_HPP_
