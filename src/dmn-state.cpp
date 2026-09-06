/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-state.cpp
 * @brief Generic State machine wrapper and API that clients can drive
 *        the state machine to execute different states.
 *
 * Each runNext() call executes at most one user-provided state callback.
 * Initialization before the first callback and finalization after terminal
 * selection are handled internally by the same call.
 */

#include "dmn-state.hpp"

#include <cassert>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dmn {

Dmn_State::Dmn_State(std::string_view name) : m_name{name} {
  m_states.emplace_back();
}

Dmn_State::~Dmn_State() {}

void Dmn_State::init([[maybe_unused]] Dmn_State &s) {
  m_initialized = true;

  setNext(1); // Select the first user state; runNext() detects an empty list.
}

void Dmn_State::finalize([[maybe_unused]] Dmn_State &s) { m_finalized = true; }

void Dmn_State::beforeSetStateFnc() {}

void Dmn_State::beforeSetNext() {}

void Dmn_State::beforeSetEnd() {}

void Dmn_State::beforeRunNext() {}

auto Dmn_State::isInitialized() -> bool { return m_initialized; }

auto Dmn_State::isFinalized() -> bool { return m_finalized; }

bool Dmn_State::hasStateFncs() const noexcept { return m_states.size() > 1; }

auto Dmn_State::runNext() -> bool {
  beforeRunNext();

  assert(!m_finalized && "runNext called after finalize");

  if (m_finalized) {
    return false;
  }

  assert(m_next <= static_cast<int>(m_states.size()));

  // A previously selected terminal state takes precedence over initialization.
  // This preserves cancellation behavior for machines that never started.
  if (m_next >= static_cast<int>(m_states.size())) {
    finalize(*this);
  } else if (!m_initialized) {
    init(*this);
  }

  if (m_finalized) {
    return false;
  }

  // Initialization selects the first user state. An empty machine therefore
  // proceeds directly to finalization without exposing either internal step.
  if (m_next >= static_cast<int>(m_states.size())) {
    finalize(*this);

    return false;
  }

  assert(m_next > 0 && "state index 0 is reserved for initialization");
  auto &fn = m_states[m_next];
  fn(*this);

  if (m_next >= static_cast<int>(m_states.size())) {
    finalize(*this);
  }

  return static_cast<bool>(*this);
}

void Dmn_State::setEnd() {
  beforeSetEnd();
  m_next = static_cast<int>(m_states.size());
}

void Dmn_State::setNext(int index) {
  beforeSetNext();

  if (index <= 0 || index > static_cast<int>(m_states.size())) {
    throw std::out_of_range(
        "setNext: index must select a user state or the end");
  }

  m_next = index;
}

void Dmn_State::setNext() {
  beforeSetNext();
  assert(m_next >= 0 && m_next < static_cast<int>(m_states.size()));
  m_next++;
}

void Dmn_State::setStateFnc(FncType fnc, int index) {
  beforeSetStateFnc();

  if (index < 0) {
    throw std::out_of_range("setStateFnc: index must be >= 0");
  }

  const int n = static_cast<int>(m_states.size());
  if (index == n || index == 0) {
    // The reserved slot makes n the next 1-based user-state index.
    m_states.emplace_back(std::move(fnc));
  } else if (index < n) {
    // Valid nonzero indices below n identify existing user states.
    m_states[index] = std::move(fnc);
  } else {
    // User-state indices must remain contiguous.
    throw std::out_of_range("setStateFnc: cannot skip steps; index too large");
  }
}

} // namespace dmn
