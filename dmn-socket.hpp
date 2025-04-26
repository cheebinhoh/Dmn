/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_SOCKET_HPP_

#define DMN_SOCKET_HPP_

#include <optional>
#include <string>
#include <string_view>

#include "dmn-io.hpp"

namespace dmn {

class Socket : public Io<std::string> {
public:
  Socket(std::string_view ip4, int port_no, bool write_only = false);
  virtual ~Socket();

  Socket(const Socket &obj) = delete;
  const Socket &operator=(const Socket &obj) = delete;
  Socket(Socket &&obj) = delete;
  Socket &operator=(Socket &&obj) = delete;

  std::optional<std::string> read() override;
  void write(std::string &item) override;
  void write(std::string &&item) override;

private:
  /**
   * data members for constructor to instantiate the object.
   */
  std::string m_ip4{};
  int m_port_no{};
  bool m_write_only{};

  /**
   * data members for internal logic.
   */
  int m_fd{-1};
};

} // namespace dmn

#endif // DMN_SOCKET_HPP_
