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

  virtual auto read() -> std::optional<T> = 0;

  virtual auto read([[maybe_unused]] size_t count,
                    [[maybe_unused]] long timeout = 0) -> std::vector<T> {
    return {};
  }

  virtual void write(T &item) = 0;

  virtual void write(T &&item) = 0;
};

} // namespace dmn

#endif // DMN_IO_HPP_
