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

class Dmn_Socket : public Dmn_Io<std::string> {
public:
  Dmn_Socket(std::string_view ip4, int portno, bool writeOnly = false);
  ~Dmn_Socket();
  std::optional<std::string> read() override;
  void write(std::string &item) override;
  void write(std::string &&item) override;

private:
  /**
   * data members for constructor to instantiate the object.
   */
  std::string m_ip4{};
  int m_portno{};
  bool m_writeOnly{};

  /**
   * data members for internal logic.
   */
  int m_fd{-1};
};

} // namespace dmn

#endif // DMN_SOCKET_HPP_
