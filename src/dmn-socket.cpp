/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-socket.cpp
 * @brief Implementation of Dmn_Socket — a UDP (SOCK_DGRAM) socket
 *        that implements the Dmn_Io<std::string> interface.
 *
 * The constructor creates an AF_INET/SOCK_DGRAM socket, enables the
 * SO_BROADCAST socket option, and optionally binds to the supplied
 * port (read mode). When write_only is true the bind step is skipped.
 *
 * read() calls recv() with MSG_WAITALL and returns std::nullopt on
 * error or when the peer closes the connection (n_read <= 0).
 *
 * write() reconstructs the destination sockaddr_in from the stored
 * address/port on every call and uses sendto() to transmit the
 * string. The rvalue overload simply moves the string into a local
 * variable and delegates to the lvalue overload.
 *
 * Note: The destination address is rebuilt on every write() call.
 * For write-heavy workloads, caching the sockaddr_in as a member
 * would reduce per-call overhead (see FIXME in write()).
 */

#include "dmn-socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dmn {

Dmn_Socket::Dmn_Socket(std::string_view ip4, int port_no, bool write_only)
    : m_ip4{ip4}, m_port_no{port_no}, m_write_only{write_only} {
  constexpr int broadcast{1};
  struct sockaddr_in servaddr{};
  const int type{SOCK_DGRAM};

  m_fd = socket(AF_INET, type, 0);
  if (m_fd < 0) {
    throw std::runtime_error("Error creating socket: " +
                             std::system_category().message(errno));
  }

  memset(&servaddr, 0, sizeof(servaddr));

  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(m_port_no);
  if (m_ip4.empty()) {
    servaddr.sin_addr.s_addr = INADDR_ANY;
  } else {
    inet_pton(AF_INET, m_ip4.c_str(), &servaddr.sin_addr);
  }

  if (setsockopt(m_fd, SOL_SOCKET, SO_BROADCAST, &broadcast,
                 sizeof(broadcast)) < 0) {
    throw std::runtime_error("Error in setsockopt: SOL_SOCKET, SO_BROADCAST: " +
                             std::system_category().message(errno));
  }

  if (!m_write_only &&
      bind(
          m_fd,
          reinterpret_cast<const struct sockaddr *>(
              &servaddr), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          sizeof(servaddr)) < 0) {
    throw std::runtime_error("Error in bind(" + std::to_string(m_port_no) +
                             ") : " + std::system_category().message(errno));
  }
}

Dmn_Socket::~Dmn_Socket() {
  if (-1 != m_fd) {
    close(m_fd);
  }
}

auto Dmn_Socket::read() -> std::optional<std::string> {
  std::array<char, BUFSIZ> buf{};

  // Block until data arrives or the socket is closed/errored.
  const ssize_t n_read = recv(m_fd, buf.data(), sizeof(buf), MSG_WAITALL);
  if (n_read < 0 || n_read == 0) {
    // EOF or error — signal end-of-stream to the caller.
    return {};
  }

  std::string string(std::span<char>(buf).data(), n_read);

  return string;
}

void Dmn_Socket::write(std::string &item) {
  const char *buf{item.c_str()};
  const size_t n_read{item.size()};
  size_t n_write{};

  /* FIXME: it might be effective to store the socket address (sockaddr_in)
   *        as a member value per object to avoid reconstructing it on every
   *        write call.
   */
  struct sockaddr_in servaddr{};
  memset(&servaddr, 0, sizeof(servaddr));
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(m_port_no);
  if (m_ip4.empty()) {
    servaddr.sin_addr.s_addr = INADDR_ANY;
  } else {
    inet_pton(AF_INET, m_ip4.c_str(), &servaddr.sin_addr);
  }

  n_write = sendto(
      m_fd, buf, n_read, 0, /* FIXME: temporary for macOS */
      reinterpret_cast<const struct sockaddr *>(
          &servaddr), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
      sizeof(servaddr));
  if (n_write != n_read) {
    throw std::runtime_error("Error in sendto: " +
                             std::system_category().message(errno));
  }
}

void Dmn_Socket::write(std::string &&item) {
  // Move into a named local so the lvalue overload can be reused.
  std::string moved_item = std::move(item);

  write(moved_item);
}

} // namespace dmn
