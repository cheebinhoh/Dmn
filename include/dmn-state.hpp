/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-state.hpp
 * @brief Generic State machine wrapper and API that clients can drive
 *        the state machine to execute different states.
 *
 * The Dmn_State class stores a sequence of state functors and provides a
 * a small API for initializing, advancing, and finalizing a state machine.
 * States are represented by functors of type std::function<void(Dmn_State&)>.
 */

#ifndef DMN_STATE_HPP_
#define DMN_STATE_HPP_

#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dmn {

/**
 * @class Dmn_State
 * @brief Compact generic finite-state-machine helper.
 *
 * Each state is a functor callable with the Dmn_State instance; the machine
 * stores these functors and uses m_next to select which to run next.
 *
 * Usage example:
 * @code
 * Dmn_State s("example");
 * s.setStateFnc(step1, 1);
 * s.setStateFnc(step2, 2);
 * while (s.runNext()) // drive the machine
 *   ;
 * @endcode
 */
class Dmn_State {
  using FncType = std::function<void(Dmn_State &s)>;

public:
  /**
   * @brief Construct a Dmn_State with a human-readable name.
   * @param name Human-readable name for diagnostics/logging.
   */
  explicit Dmn_State(std::string_view name);

  /**
   * @brief Virtual destructor to allow clean subclassing.
   *
   * noexcept to avoid throwing during stack unwinding.
   */
  virtual ~Dmn_State() noexcept;

  Dmn_State(const Dmn_State &obj) = delete;            ///< non-copyable
  Dmn_State &operator=(const Dmn_State &obj) = delete; ///< non-copyable
  Dmn_State(Dmn_State &&obj) = delete;                 ///< non-movable
  Dmn_State &operator=(Dmn_State &&obj) = delete;      ///< non-movable

  /**
   * @brief Mark the state machine to end (finalize) after the current step.
   */
  void setEnd();

  /**
   * @brief Set the next state by 1-based index into the configured states.
   * @param index 1..m_states.size() selects a user state.
   *
   * @note If index is out of range the implementation may assert or throw.
   */
  void setNext(int index);

  /**
   * @brief Convenience: set the next state to the next sequential state.
   *
   * Advances m_next by one (subject to bounds and configured states).
   */
  void setNext();

  /**
   * @brief Set the functor for a state slot.
   * @param fnc The functor to be called for the state step.
   * @param index If 1..m_states.size(), place fnc at that slot; if 0,
   *              behavior is implementation-specific (commonly used to set
   *              the "current/next" state).
   */
  void setStateFnc(FncType fnc, int index = 0);

  /**
   * @brief Check whether the machine has been initialized.
   * @return true if initialization has occurred.
   */
  auto isInitialized() -> bool;

  /**
   * @brief Check whether the machine has been finalized.
   * @return true if the machine has completed/finalized.
   */
  auto isFinalized() -> bool;

  /**
   * @brief Execute the next state step.
   * @return true if the state machine remains active after running the step;
   *         false when it has finalized/stopped.
   */
  auto runNext() -> bool;

  /// conversion to bool: true when NOT finalized
  explicit operator bool() const noexcept { return !m_finalized; }

  /// optional complement for clarity
  bool operator!() const noexcept { return m_finalized; }

protected:
  /**
   * @brief Perform internal initialization. Intended for internal use or
   *        subclasses that need to hook into init behavior.
   * @param s Reference to the state object being initialized.
   */
  void init(Dmn_State &s);

  /**
   * @brief Perform internal finalization/cleanup. Intended for internal use
   *        or subclasses that need to hook into finalize behavior.
   * @param s Reference to the state object being finalized.
   */
  void finalize(Dmn_State &s);

private:
  const std::string m_name{}; ///< Human-readable name for diagnostics/logging.

  /**
   * @brief Next state selector.
   *
   * Semantics:
   *  - 0  => initialization step (no user state)
   *  - <0 => finalize / terminated
   *  - >0 => 1-based index into m_states (user-provided states)
   */
  int m_next{};

  /**
   * @brief State functors.
   *
   * Conceptually 1-based: user states occupy slots 1..m_states.size(). Index 0
   * is reserved/unused by the vector storage.
   */
  std::vector<FncType> m_states{};

  bool m_initialized{}; ///< true when init() has run
  bool m_finalized{};   ///< true when finalize() has run
};

} // namespace dmn

#endif // DMN_STATE_HPP_
