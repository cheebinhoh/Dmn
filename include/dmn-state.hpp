/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-state.hpp
 * @brief Generic state machine with stepper triggered by clients.
 */

#ifndef DMN_STATE_HPP_

#define DMN_STATE_HPP_

#include <cassert>
#include <functional>
#include <string>
#include <string_view>

namespace dmn {

class Dmn_State {
public:
  explicit Dmn_State(std::string_view name);
  virtual ~Dmn_State() noexcept;

  Dmn_State(const Dmn_State &obj) = delete;
  Dmn_State &operator=(const Dmn_State &obj) = delete;
  Dmn_State(Dmn_State &&obj) = delete;
  Dmn_State &operator=(Dmn_State &&obj) = delete;

private:
  const std::string m_name{}; ///< Human-readable name for diagnostics/logging.
};

} // namespace dmn

#endif // DMN_STATE_HPP_
