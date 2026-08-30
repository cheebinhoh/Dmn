/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-state.cpp
 * @brief Generic state machine with stepper triggered by clients.
 *
 * This module provides a C++ wrapper and object for a generic state machine
 * with stepper api to be triggerred by clients.
 */

#include "dmn-state.hpp"

#include <cassert>
#include <functional>
#include <string>
#include <string_view>

namespace dmn {

Dmn_State::Dmn_State(std::string_view name) : m_name{name} {}

Dmn_State::~Dmn_State() {}

} // namespace dmn
