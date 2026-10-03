/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-socket.hpp
 * @brief Socket-based implementation of the Dmn_Io interface.
 *
 * @details
 * This header declares Dmn_Socket, a thin wrapper around a BSD-style
 * IPv4 UDP datagram socket that implements the Dmn_Io<std::string> interface.
 * The class provides read and write operations for sending and receiving
 * std::string messages through a network endpoint. It is intended for
 * simple synchronous socket I/O and does not provide its own threading,
 * buffering beyond the std::string storage, or non-blocking/evented
 * semantics — those responsibilities belong to the caller.
 *
 * Notes:
 *  - The constructor takes an IPv4 address (as text) and a port number.
 *  - The optional 'write_only' flag can be used when the instance is
 *    only required to send data (the implementation may avoid setting
 *    up read-specific resources in that case).
 *  - read() returns std::nullopt when recv() returns zero or an error.
 *  - write(...) methods send the provided string over the socket; their
 *    semantics and error handling are implementation-defined but documented
 *    here for callers to expect possible exceptions or logging on failure.
 */

#ifndef DMN_SOCKET_HPP_
#define DMN_SOCKET_HPP_

#include "dmn-io.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace dmn {

/**
 * @class Dmn_Socket
 * @brief Implements a socket-based Dmn_Io for std::string messages.
 *
 * @details
 * Provides a synchronous socket interface that conforms to the
 * Dmn_Io<std::string> contract. The class manages a single file
 * descriptor (m_fd) for a UDP socket. Read mode binds to the supplied address
 * and port; writes send datagrams to the supplied IPv4 address and port.
 *
 * Thread-safety: Instances are NOT inherently thread-safe. Synchronize
 * access externally if multiple threads share an instance.
 *
 * Lifetime/ownership: The socket file descriptor is owned by the object
 * and closed in the destructor. Copy and move operations are deleted to
 * avoid accidental sharing of the descriptor.
 */
class Dmn_Socket : public Dmn_Io<std::string> {
public:
  using Dmn_Io<std::string>::write;

  /**
   * @brief Construct a Dmn_Socket using the given address and port.
   *
   * @param ip4 IPv4 address as a string (e.g. "127.0.0.1").
   * @param port_no UDP port number.
   * @param write_only If true, the instance may skip read-specific setup;
   * caller guarantees no calls to read() in that mode.
   *
   * @throws std::runtime_error on failure to create, configure, or bind the
   * socket.
   */
  Dmn_Socket(std::string_view ip4, int port_no, bool write_only = false);

  /**
   * @brief Destroy the Dmn_Socket and close the underlying socket.
   *
   * Closes the socket and releases its resources.
   */
  virtual ~Dmn_Socket() noexcept;

  /* Non-copyable and non-movable: owning the socket FD prohibits copying. */
  Dmn_Socket(const Dmn_Socket &obj) = delete;
  Dmn_Socket &operator=(const Dmn_Socket &obj) = delete;
  Dmn_Socket(Dmn_Socket &&obj) = delete;
  Dmn_Socket &operator=(Dmn_Socket &&obj) = delete;

  /**
   * @brief Read data from the socket.
   *
   * @return std::optional<std::string> containing the received datagram, or
   * std::nullopt when recv() returns zero or an error.
   *
   * @note The exact boundary semantics (message delimiting, framing) are
   * implementation-specific. Callers should consult the implementation
   * or use an application-level protocol to delimit messages.
   */
  auto read() -> std::optional<std::string> override;

  /**
   * @brief Write a string to the socket.
   *
   * @param item The string to write. This overload accepts a const lvalue
   * reference and will typically copy the contents as-is.
   *
   * @note On partial writes or errors, behavior is implementation-defined:
   * the method may retry, throw, or log and return. Callers should
   * not assume atomicity of large writes unless the implementation
   * documents it.
   */
  void write(const std::string &item) override;

  /**
   * @brief Write a string to the socket using move semantics.
   *
   * @param item The string to write. This overload delegates to the
   * const-reference overload and does not move from the argument.
   */
  void write(std::string &&item) override;

private:
  /**
   * Data provided by the caller at construction time.
   */
  std::string m_ip4{}; ///< IPv4 address as text (e.g., "192.0.2.1")
  int m_port_no{};     ///< UDP port number
  bool m_write_only{}; ///< If true, socket is used only for sending

  /**
   * Internal runtime state.
   */
  int m_fd{-1}; ///< Underlying socket file descriptor (-1 if not opened)
};

} // namespace dmn

#endif // DMN_SOCKET_HPP_
