/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
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
 *  - read() returns an engaged optional for every received datagram, including
 *    zero-length datagrams. Receive errors and datagrams larger than BUFSIZ
 *    are reported by throwing std::system_error.
 *  - Addresses must be valid IPv4 literals. Read-mode sockets allow port 0
 *    for OS-assigned binding and an empty address for wildcard binding;
 *    write-only sockets require a destination address and nonzero port.
 *    Writing through a wildcard-bound or port-zero socket is rejected because
 *    neither has a configured datagram destination.
 *  - write(...) throws std::system_error if sendto() fails or sends a
 *    different number of bytes than requested.
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
 * Thread-safety: Instances are not thread-safe. Callers must externally
 * serialize all operations if an instance is shared across threads.
 *
 * Lifetime/ownership: The socket file descriptor is owned by the object
 * and closed in the destructor. The inherited Dmn_Io::shutdown() is a no-op
 * and does not interrupt a blocked read. Callers must arrange for every
 * operation to finish and join threads using the socket before destruction;
 * closing the descriptor is not a cross-thread read-cancellation mechanism.
 * Copy and move operations are deleted to avoid accidental sharing of the
 * descriptor.
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
   * @throws std::invalid_argument for an invalid address/port combination.
   * @throws std::system_error if creating, configuring, or binding the socket
   * fails.
   */
  Dmn_Socket(std::string_view ip4, int port_no, bool write_only = false);

  /**
   * @brief Destroy the Dmn_Socket and close the underlying socket.
   *
   * Closes the socket and releases its resources. No other thread may be
   * using the socket when destruction begins.
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
   * @return An engaged optional containing the received datagram. A
   * zero-length datagram contains an empty string.
   * @throws std::system_error if receiving fails or the datagram exceeds the
   * BUFSIZ receive buffer.
   *
   * @note This call may block indefinitely. The inherited shutdown() does not
   * wake it; the caller must arrange for the reading thread to finish before
   * destroying the socket.
   *
   * @note UDP preserves datagram boundaries. Any framing within the payload is
   * application-defined; the maximum datagram size accepted by this adapter
   * is BUFSIZ bytes.
   */
  auto read() -> std::optional<std::string> override;

  /**
   * @brief Write a string to the socket.
   *
   * @param item The string to write. This overload accepts a const lvalue
   * reference and will typically copy the contents as-is.
   *
   * @throws std::system_error if sending fails or does not send the complete
   * datagram.
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
