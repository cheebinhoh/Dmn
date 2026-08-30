/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-state.cpp
 * @brief Generic State machine wrapper and API that clients can drive
 *        the state machine to execute different states.
 *
 * The Dmn_State class stores a sequence of state functors and provides a
 * a small API for initializing, advancing, and finalizing a state machine.
 * States are represented by functors of type std::function<void(Dmn_State&)>.
 */

#include "dmn-state.hpp"

#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dmn {

Dmn_State::Dmn_State(std::string_view name) : m_name{name} {
  m_states.emplace_back(
      std::bind(&Dmn_State::init, this, std::placeholders::_1));
}

Dmn_State::~Dmn_State() {}

void Dmn_State::init([[maybe_unused]] Dmn_State &s) {
  m_initialized = true;

  setNext(1); // either end of state or user provided first state
}

void Dmn_State::finalize([[maybe_unused]] Dmn_State &s) { m_finalized = true; }

auto Dmn_State::isInitialized() -> bool { return m_initialized; }

auto Dmn_State::isFinalized() -> bool { return m_finalized; }

auto Dmn_State::runNext() -> bool {
  // preferred assertion: use an explicit cast so it always compiles
  assert(static_cast<bool>(*this) && "runNext called after finalize");

  // runtime check (assert disappears in release; use this if you need it
  // always)
  if (!static_cast<bool>(*this)) {
    // handle error: return false, throw, log, etc.
    return false;
  }

  assert(m_next <= static_cast<int>(m_states.size()));
  if (m_next < 0) {
    finalize(*this);
  } else if (m_next >= static_cast<int>(m_states.size())) {
    finalize(*this);
  } else {
    auto &fn = m_states[m_next];
    fn(*this);
  }

  return static_cast<bool>(*this);
}

void Dmn_State::setEnd() { m_next = static_cast<int>(m_states.size()); }

void Dmn_State::setNext(int index) {
  assert(index >= 0 && index <= static_cast<int>(m_states.size()));

  m_next = index;
}

void Dmn_State::setNext() {
  assert(m_next >= 0 && m_next < static_cast<int>(m_states.size()));
  m_next++;
}

void Dmn_State::setStateFnc(FncType fnc, int index) {
  if (index < 0) {
    throw std::out_of_range("setStateFnc: index must be >= 0");
  }

  const int n = static_cast<int>(m_states.size());
  if (index == n || index == 0) {
    // append the next step (must be exactly the next index)
    m_states.emplace_back(std::move(fnc));
  } else if (index < n) {
    // overwrite an existing (non-zero) step
    m_states[index] = std::move(fnc);
  } else {
    // index > n -> skipping steps is not allowed
    throw std::out_of_range("setStateFnc: cannot skip steps; index too large");
  }
}

} // namespace dmn
