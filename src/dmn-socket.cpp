/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-socket.cpp
 * @brief Implementation of Dmn_Socket — a UDP (SOCK_DGRAM) socket
 * that implements the Dmn_Io<std::string> interface.
 *
 * The constructor validates the IPv4/port configuration before creating an
 * AF_INET/SOCK_DGRAM socket, enables SO_BROADCAST, and binds unless
 * write_only is true. If socket configuration or binding fails after creation,
 * it closes the descriptor before rethrowing.
 *
 * read() receives one complete UDP datagram. Empty datagrams are returned as
 * empty strings; receive errors and truncated datagrams throw
 * std::system_error. The receive buffer is BUFSIZ bytes, and oversized
 * datagrams are consumed and reported as message_size rather than returned
 * partially.
 *
 * write() reconstructs the destination sockaddr_in from the stored
 * address/port on every call and uses sendto() to transmit the
 * string. The rvalue overload delegates to the lvalue overload without moving
 * from the string.
 *
 * The destination address is rebuilt on every write() call. Wildcard-bound
 * and port-zero sockets have no configured destination and reject writes.
 * For write-heavy workloads, caching the sockaddr_in as a member would reduce
 * per-call overhead (see FIXME in write()).
 */

#include "dmn-socket.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace dmn {

Dmn_Socket::Dmn_Socket(std::string_view ip4, int port_no, bool write_only)
    : m_ip4{ip4}, m_port_no{port_no}, m_write_only{write_only} {
  if (port_no < 0 || port_no > std::numeric_limits<uint16_t>::max() ||
      (write_only && port_no == 0)) {
    throw std::invalid_argument("Dmn_Socket: invalid UDP port");
  }

  if (m_ip4.empty() && write_only) {
    throw std::invalid_argument(
        "Dmn_Socket: write-only sockets require a destination IPv4 address");
  }

  constexpr int broadcast{1};
  struct sockaddr_in servaddr {};
  const int type{SOCK_DGRAM};

  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(static_cast<uint16_t>(m_port_no));

  if (m_ip4.empty()) {
    servaddr.sin_addr.s_addr = INADDR_ANY;
  } else if (inet_pton(AF_INET, m_ip4.c_str(), &servaddr.sin_addr) != 1) {
    throw std::invalid_argument("Dmn_Socket: invalid IPv4 address: " + m_ip4);
  }

  m_fd = socket(AF_INET, type, 0);
  if (m_fd < 0) {
    throw std::system_error(errno, std::system_category(),
                            "Dmn_Socket: socket");
  }

  try {
    if (setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &broadcast,
                   sizeof(broadcast)) < 0) {
      throw std::system_error(errno, std::system_category(),
                              "Dmn_Socket: setsockopt SO_BROADCAST");
    }

    if (!m_write_only &&
        bind(
            m_fd,
            reinterpret_cast<const struct sockaddr *>(
                &servaddr), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            sizeof(servaddr)) < 0) {
      throw std::system_error(errno, std::system_category(),
                              "Dmn_Socket: bind");
    }
  } catch (...) {
    close(m_fd);
    m_fd = -1;

    throw;
  }
}

Dmn_Socket::~Dmn_Socket() noexcept {
  if (-1 != m_fd) {
    close(m_fd);
  }
}

auto Dmn_Socket::read() -> std::optional<std::string> {
  std::array<char, BUFSIZ> buf{};
  struct iovec iov {
    .iov_base = buf.data(), .iov_len = buf.size(),
  };
  struct msghdr msg {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;

  ssize_t n_read{};
  do {
    n_read = recvmsg(m_fd, &msg, 0);
  } while (n_read < 0 && errno == EINTR);

  if (n_read < 0) {
    throw std::system_error(errno, std::system_category(),
                            "Dmn_Socket: recvmsg");
  }

  if ((msg.msg_flags & MSG_TRUNC) != 0) {
    throw std::system_error(std::make_error_code(std::errc::message_size),
                            "Dmn_Socket: received UDP datagram exceeds BUFSIZ");
  }

  return std::string(buf.data(), static_cast<size_t>(n_read));
}

void Dmn_Socket::write(const std::string &item) {
  if (m_ip4.empty() || m_port_no == 0) {
    throw std::logic_error(
        "Dmn_Socket: socket has no configured datagram destination");
  }

  const char *buf{item.c_str()};
  const size_t n_read{item.size()};
  ssize_t n_write{};

  /* FIXME: it might be effective to store the socket address (sockaddr_in)
   *        as a member value per object to avoid reconstructing it on every
   *        write call.
   */
  struct sockaddr_in servaddr {};
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(m_port_no);
  if (inet_pton(AF_INET, m_ip4.c_str(), &servaddr.sin_addr) != 1) {
    throw std::invalid_argument("Dmn_Socket: invalid IPv4 destination: " +
                                m_ip4);
  }

  n_write = sendto(
      m_fd, buf, n_read, 0, /* FIXME: temporary for macOS */
      reinterpret_cast<const struct sockaddr *>(
          &servaddr), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      sizeof(servaddr));
  if (n_write < 0 || static_cast<size_t>(n_write) != n_read) {
    const int error = n_write < 0 ? errno : EMSGSIZE;

    throw std::system_error(error, std::system_category(),
                            "Dmn_Socket: sendto");
  }
}

void Dmn_Socket::write(std::string &&item) { write(std::as_const(item)); }

} // namespace dmn
