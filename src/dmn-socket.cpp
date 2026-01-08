/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-socket.hpp
 * @brief Socket-based implementation of the Dmn_Io interface.
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
    throw std::runtime_error("Error in read socket: " +
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
    throw std::runtime_error("Error in setsockopt: SO_SOCKET, SO_BROADCAST: " +
                             std::system_category().message(errno));
  }

  if (!m_write_only &&
      bind(
          m_fd,
          reinterpret_cast<const struct sockaddr *>(
              &servaddr), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
          sizeof(servaddr)) < 0) {
    throw std::runtime_error("Error in bind(" + std::to_string(m_port_no) +
                             "): " + std::system_category().message(errno));
  }
}

Dmn_Socket::~Dmn_Socket() {
  if (-1 != m_fd) {
    close(m_fd);
  }
}

auto Dmn_Socket::read() -> std::optional<std::string> {
  std::array<char, BUFSIZ> buf{};

  const ssize_t n_read = recv(m_fd, buf.data(), sizeof(buf), MSG_WAITALL);
  if (n_read < 0 || n_read == 0) {
    return {};
  }

  std::string string(std::span<char>(buf).data(), n_read);

  return string;
}

void Dmn_Socket::write(std::string &item) {
  const char *buf{item.c_str()};
  const size_t n_read{item.size()};
  size_t n_write{};

  /* FIXME: it might be effectiveto store socket_addr as member value per object
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
  std::string moved_item = std::move(item);

  write(moved_item);
}

} // namespace dmn
