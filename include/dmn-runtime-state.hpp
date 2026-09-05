/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.hpp
 * @brief Public API for the Runtime State Manager: a runtime-owned state
 *        execution manager that composes the existing dmn-runtime scheduler
 *        with the dmn-state finite-state helper.
 *
 * @author Chee Bin HOH
 * @date 2026-08-31
 *
 * This header declares the public types used by clients to create and manage
 * runtime-managed state machine instances. The current implementation phase
 * supports construction of Dmn_Runtime_State_Manager only. Dmn_Runtime_State
 * lifecycle and scheduling operations remain declarations for later phases;
 * clients must not use them until their implementations and tests are added.
 */

#ifndef DMN_RUNTIME_STATE_HPP_
#define DMN_RUNTIME_STATE_HPP_

#include "dmn-runtime.hpp"
#include "dmn-singleton.hpp"
#include "dmn-state.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace dmn {

/**
 * @class Dmn_Runtime_State
 * @brief A runtime-managed state machine instance.
 *
 * Dmn_Runtime_State will subclass Dmn_State and add asynchronous runtime
 * ownership semantics: a client obtains a shared_ptr handle from the
 * manager, configures state functors using the same Dmn_State API, then
 * calls run() to schedule execution on the global runtime thread.
 *
 * Planned behavior:
 * - createState() returns std::shared_ptr<Dmn_Runtime_State> — the manager
 *   also holds a shared_ptr while the state is queued or running to
 *   guarantee lifetime.
 * - run(priority, delay, onError) enqueues the state for execution; it
 *   returns true when enqueue succeeded and false otherwise. The optional
 *   onError callback uses Dmn_Runtime_Job::OnErrorFncType and is forwarded
 *   into the runtime job.
 * - cancel() is cooperative: runtime tasks must observe isCancelled() and
 *   call setEnd() before runNext() when cancelled.
 * - wait(), wait_for(), and getFuture() provide blocking and non-blocking
 *   completion waiting. getFuture() returns a std::shared_future<void> so
 *   multiple waiters are supported.
 * - Calling run() or wait() from inside the runtime async thread is
 *   disallowed: implementations MUST throw std::runtime_error when detected.
 */
class Dmn_Runtime_State : public Dmn_State {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;
  using OnErrorFnc =
      Dmn_Runtime_Job::OnErrorFncType; // std::function<void(std::exception_ptr
                                       // &)>

  /**
   * @brief Construct a runtime-managed state object with a human-readable name.
   * @param name Human-readable name used for diagnostics.
   */
  explicit Dmn_Runtime_State(std::string_view name);

  /**
   * @brief Virtual destructor. Implementation should ensure safe teardown.
   */
  virtual ~Dmn_Runtime_State() noexcept;

  /* State configuration (inherited semantics from Dmn_State) */

  /**
   * @brief Set the functor for a state slot.
   * @param fnc The functor to be called for the state step.
   * @param index 1-based index for the state slot (0 uses default behavior).
   *
   * This method preserves Dmn_State semantics and allows clients to define
   * the machine steps.
   */
  void setStateFnc(FncType fnc, int index = 0);

  /**
   * @brief Set the next user state by index (1-based).
   * @param index 1..m_states.size() selects the next state.
   */
  void setNext(int index);

  /**
   * @brief Convenience: set the next state to the sequential next slot.
   */
  void setNext();

  /**
   * @brief Mark the machine to finalize after the current step.
   */
  void setEnd();

  /* Lifecycle API */

  /**
   * @brief Schedule this state handle for runtime execution.
   *
   * @param priority Job priority to use when enqueuing (maps to
   * Dmn_Runtime_Job::Priority).
   * @param delay If non-zero, the job is scheduled via addTimedJob() after this
   * delay.
   * @param onError Optional error callback forwarded to the runtime job. The
   *                type matches Dmn_Runtime_Job::OnErrorFncType.
   * @return true if the state was successfully queued; false if enqueue
   *         failed (already terminal, duplicate run, invalid configuration).
   *
   * Notes:
   * - run() is one-shot for a given handle: the first successful call enqueues
   *   the state; subsequent calls return false.
   * - Calling run() from inside the runtime async thread is disallowed and
   *   throws std::runtime_error.
   */
  bool
  run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
      const std::chrono::steady_clock::duration &delay =
          std::chrono::steady_clock::duration::zero(),
      OnErrorFnc onError = {});

  /**
   * @brief Request cooperative cancellation of this state.
   *
   * The cancellation is idempotent and thread-safe. It does NOT preempt a
   * currently-running functor unless that functor explicitly observes the
   * cancel flag via isCancelled(). The runtime job must check isCancelled()
   * and call setEnd() prior to invoking runNext() if cancellation is set.
   */
  void cancel();

  /**
   * @brief Block until the state reaches a terminal condition.
   *
   * Calling wait() from the runtime async thread is disallowed and throws
   * std::runtime_error. See getFuture() for async waiting.
   */
  void wait();

  /**
   * @brief Block until terminal or timeout.
   * @return true if terminal observed before timeout, false otherwise.
   */
  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout);

  /**
   * @brief Return a shared_future that becomes ready when the state reaches
   *        a terminal condition (completed/failed/cancelled).
   *
   * The shared_future is available immediately after the state is created
   * so callers may register waiters before run() is called.
   */
  std::shared_future<void> getFuture();

  /**
   * @brief Introspection helpers.
   *
   * isRunning(): queued or actively running inside the manager.
   * isCompleted(): terminal success.
   * isFailed(): terminal failure.
   * isCancelled(): cancellation requested.
   */
  bool isRunning() const;
  bool isCompleted() const;
  bool isFailed() const;
  bool isCancelled() const;

protected:
  /**
   * @brief Lifecycle hooks for derived implementations.
   *
   * Subclasses may override these to observe state lifecycle transitions. The
   * default implementations are no-ops.
   */
  virtual void onStarted();
  virtual void onCompleted();
  virtual void onFailed(std::exception_ptr ep);
  virtual void onCancelled();

private:
  // Implementation details are intentionally private and placed in the
  // corresponding .cpp. Refer to the specification for the concurrency and
  // lifecycle invariants the implementation must satisfy.
};

/**
 * @class Dmn_Runtime_State_Manager
 * @brief Singleton manager for runtime-managed states.
 *
 * The current implementation supports singleton construction only. State
 * creation and runtime-managed execution are added in later phases.
 */
class Dmn_Runtime_State_Manager
    : public Dmn_Singleton<Dmn_Runtime_State_Manager> {
  friend class Dmn_Singleton<Dmn_Runtime_State_Manager>;

public:
  /**
   * @brief Destroy the runtime state manager singleton.
   *
   * The Phase-1 destructor releases only manager construction state.
   * Runtime-managed state shutdown is introduced in a later phase.
   */
  virtual ~Dmn_Runtime_State_Manager() noexcept;

  Dmn_Runtime_State_Manager(const Dmn_Runtime_State_Manager &obj) = delete;
  Dmn_Runtime_State_Manager &
  operator=(const Dmn_Runtime_State_Manager &obj) = delete;
  Dmn_Runtime_State_Manager(Dmn_Runtime_State_Manager &&obj) = delete;
  Dmn_Runtime_State_Manager &
  operator=(Dmn_Runtime_State_Manager &&obj) = delete;

protected:
  /**
   * @brief Construct the process-wide runtime state manager.
   *
   * Construction is restricted to @ref Dmn_Singleton. The optional name is
   * retained for diagnostics; callers normally use the inherited
   * @c createInstance() factory without arguments.
   *
   * @param name Human-readable manager name for diagnostics.
   */
  Dmn_Runtime_State_Manager(std::string_view name = "");

private:
  std::string m_name{}; ///< Human-readable manager name for diagnostics.
};

} // namespace dmn

#endif // DMN_RUNTIME_STATE_HPP_
