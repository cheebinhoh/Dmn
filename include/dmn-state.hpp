/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-state.hpp
 * @brief A caller-driven state machine composed of callback functions.
 *
 * Clients register one or more callbacks and advance the machine by calling
 * runNext(). Each callback receives the state machine so it can repeat,
 * select another state, advance sequentially, or end execution.
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
 * @brief A finite-state machine advanced explicitly by its caller.
 *
 * Register callbacks with setStateFnc(), then call runNext() until it returns
 * false. Each call executes at most one callback. A callback remains selected
 * for the next call unless it calls setNext(), setNext(int), or setEnd().
 *
 * Initialization runs automatically before the first callback. Finalization
 * runs automatically as soon as a callback selects the end. Neither lifecycle
 * operation requires a separate call from the client.
 *
 * A machine with no callbacks converts to false. Calling runNext() on an empty
 * machine is valid: it initializes and finalizes the machine, then returns
 * false without invoking a callback. Calling runNext() after finalization
 * returns false unless a derived beforeRunNext() hook rejects the call.
 *
 * Usage example:
 * @code
 * Dmn_State state{"example"};
 * state.setStateFnc([](Dmn_State &current) {
 *   // First state work.
 *   current.setNext();
 * });
 * state.setStateFnc([](Dmn_State &current) {
 *   // Second state work.
 *   current.setEnd();
 * });
 *
 * while (state.runNext())
 *   ;
 * @endcode
 */
class Dmn_State {
  using FncType = std::function<void(Dmn_State &s)>;

public:
  /**
   * @brief Construct an empty state machine.
   *
   * @param name Human-readable name for diagnostics.
   */
  explicit Dmn_State(std::string_view name);

  /**
   * @brief Destroy the state machine.
   */
  virtual ~Dmn_State() noexcept;

  Dmn_State(const Dmn_State &obj) = delete;            ///< non-copyable
  Dmn_State &operator=(const Dmn_State &obj) = delete; ///< non-copyable
  Dmn_State(Dmn_State &&obj) = delete;                 ///< non-movable
  Dmn_State &operator=(Dmn_State &&obj) = delete;      ///< non-movable

  /**
   * @brief Select the end of the state machine.
   *
   * When called from a state callback that completes normally, finalization
   * occurs before the current runNext() call returns. Otherwise, the next
   * runNext() call finalizes the machine without invoking a user callback.
   */
  void setEnd();

  /**
   * @brief Select which user state the next runNext() call will execute.
   *
   * This selection applies after initialization has completed. Initialization
   * always selects the first user state.
   *
   * @param index With N configured user states, values 1 through N select a
   * callback. N+1 selects the end of the machine. Zero is
   * reserved for internal initialization.
   * @throws std::out_of_range if index is outside 1 through N+1.
   */
  void setNext(int index);

  /**
   * @brief Select the next sequential user state.
   *
   * Initialization always selects the first user state. After initialization,
   * calling this from the last user state selects the end of the machine.
   */
  void setNext();

  /**
   * @brief Add a user-state callback or replace an existing one.
   *
   * @param fnc Callback to execute when this state is selected.
   * @param index With N callbacks currently configured, pass 0 (the default)
   * or N+1 to append a callback. Pass 1 through N to replace the
   * callback at that state.
   * @throws std::invalid_argument if @p fnc is empty.
   * @throws std::logic_error if called from a running state callback.
   * @throws std::out_of_range if index is negative or greater than N+1.
   *
   * State numbers start at 1. Zero means "append" only in this method and
   * cannot be selected with setNext().
   *
   * Callback configuration and execution are not thread-safe. Do not modify
   * callback registration from a state callback or concurrently with
   * runNext().
   */
  void setStateFnc(FncType fnc, int index = 0);

  /**
   * @brief Report whether internal initialization has run.
   *
   * @return true after runNext() initializes the machine before its first
   * user-state callback.
   */
  auto isInitialized() -> bool;

  /**
   * @brief Report whether internal finalization has run.
   *
   * @return true after runNext() reaches the end of the machine.
   */
  auto isFinalized() -> bool;

  /**
   * @brief Report whether at least one user-state callback is configured.
   *
   * @return true when the machine contains a user-provided callback.
   */
  bool hasStateFncs() const noexcept;

  /**
   * @brief Execute the currently selected user-state callback.
   *
   * On the first call, initialization runs before the callback. If the
   * callback selects the end by calling setEnd() or by advancing past the last
   * state, finalization runs before this method returns.
   *
   * If no callbacks are configured, this method initializes and finalizes the
   * machine without invoking a callback.
   *
   * @return true when another callback can be executed; false after
   * finalization. A base Dmn_State returns false after finalization; derived
   * classes may reject the call from beforeRunNext().
   *
   * Callback exceptions propagate without rolling back the selected state or
   * any transition the callback already made. If initialization's transition
   * hook throws, initialization remains pending and can be retried.
   *
   * @throws std::logic_error if called recursively from a state callback.
   */
  auto runNext() -> bool;

  /**
   * @brief Report whether the machine contains callbacks and is not finalized.
   *
   * A false result can mean either that no callback is configured or that the
   * machine has finalized. Use isFinalized() to distinguish those cases.
   */
  explicit operator bool() const noexcept {
    return hasStateFncs() && !m_finalized;
  }

  /** @brief Return the logical complement of operator bool(). */
  bool operator!() const noexcept { return !static_cast<bool>(*this); }

protected:
  /**
   * @brief Perform the initialization used by runNext().
   *
   * Derived classes normally do not need to call this directly.
   *
   * @param s Reference to the state object being initialized.
   */
  void init(Dmn_State &s);

  /**
   * @brief Perform the finalization used by runNext().
   *
   * Derived classes normally do not need to call this directly.
   *
   * @param s Reference to the state object being finalized.
   */
  void finalize(Dmn_State &s);

  /**
   * @brief Validate an impending setStateFnc() operation.
   *
   * Derived classes may override this hook to reject configuration changes,
   * for example after execution starts. The default implementation permits
   * the operation.
   */
  virtual void beforeSetStateFnc();

  /**
   * @brief Validate an impending setNext() operation.
   *
   * Derived classes may override this hook to restrict who may select a
   * transition. The default implementation permits the operation.
   */
  virtual void beforeSetNext();

  /**
   * @brief Validate an impending setEnd() operation.
   *
   * Derived classes may override this hook to restrict who may end the
   * machine. The default implementation permits the operation.
   */
  virtual void beforeSetEnd();

  /**
   * @brief Validate an impending runNext() operation.
   *
   * Derived classes may override this hook to restrict where execution may
   * occur. The default implementation permits the operation.
   */
  virtual void beforeRunNext();

private:
  const std::string m_name{}; ///< Human-readable name for diagnostics/logging.

  /**
   * @brief Next state selector.
   *
   * Semantics:
   *  - 0  => initialization is pending; not a valid setNext() argument
   *  - 1..m_states.size()-1 => selected user state
   *  - m_states.size() => finalization is pending
   */
  int m_next{};

  /**
   * @brief State functors.
   *
   * Slot 0 is a placeholder that keeps user-state indices 1-based. User
   * callbacks occupy slots 1..m_states.size()-1.
   */
  std::vector<FncType> m_states{};

  bool m_initialized{};          ///< true when init() has run
  bool m_finalized{};            ///< true when finalize() has run
  bool m_runningStateCallback{}; ///< true while a user callback is executing
};

} // namespace dmn

#endif // DMN_STATE_HPP_
