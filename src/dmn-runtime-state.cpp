/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.cpp
 * @brief Implementation of the runtime state manager singleton.
 *
 * This translation unit provides construction of Dmn_Runtime_State_Manager
 * and client state handles. Runtime-managed state execution is added in
 * subsequent implementation phases.
 */

#include "dmn-runtime-state.hpp"

#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dmn {

/**
 * @brief Initialize the singleton manager's diagnostic name.
 *
 * Runtime state scheduling and manager-side ownership are deliberately not
 * initialized in this creation-only implementation phase.
 *
 * @param name Human-readable manager name for diagnostics.
 */
Dmn_Runtime_State_Manager::Dmn_Runtime_State_Manager(std::string_view name)
    : m_name{name} {}

/**
 * @brief Destroy the runtime state manager before scheduling is implemented.
 */
Dmn_Runtime_State_Manager::~Dmn_Runtime_State_Manager() {}

/**
 * @brief Construct a client-owned runtime state handle.
 *
 * Manager retention begins only after a future scheduling phase successfully
 * queues the state for execution.
 *
 * @param name Human-readable state name used for diagnostics.
 * @return A newly constructed runtime-managed state handle.
 */
DmnRuntimeStatePtr
Dmn_Runtime_State_Manager::createState(std::string_view name) {
  return std::make_shared<Dmn_Runtime_State>(name);
}

Dmn_Runtime_State::Dmn_Runtime_State(std::string_view name) : Dmn_State{name} {}

Dmn_Runtime_State::~Dmn_Runtime_State() {}

void Dmn_Runtime_State::onStarted() {}

void Dmn_Runtime_State::onCompleted() {}

void Dmn_Runtime_State::onFailed(std::exception_ptr ep) { (void)ep; }

void Dmn_Runtime_State::onCancelled() {}

} // namespace dmn
