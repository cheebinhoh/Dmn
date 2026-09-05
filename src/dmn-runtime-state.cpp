/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.cpp
 * @brief Implementation of the runtime state manager singleton.
 *
 * This translation unit currently provides the construction boundary for
 * Dmn_Runtime_State_Manager. Runtime-managed state execution is added in
 * subsequent implementation phases.
 */

#include "dmn-runtime-state.hpp"

#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dmn {

/**
 * @brief Initialize the singleton manager's diagnostic name.
 *
 * Runtime state scheduling and ownership are deliberately not initialized in
 * this construction-only implementation phase.
 *
 * @param name Human-readable manager name for diagnostics.
 */
Dmn_Runtime_State_Manager::Dmn_Runtime_State_Manager(std::string_view name)
    : m_name{name} {}

/**
 * @brief Destroy the construction-only runtime state manager.
 */
Dmn_Runtime_State_Manager::~Dmn_Runtime_State_Manager() {}

} // namespace dmn
