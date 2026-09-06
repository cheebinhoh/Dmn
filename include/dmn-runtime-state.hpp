/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.hpp
 * @brief Runtime-scheduled finite-state-machine execution and lifetime
 *        management.
 *
 * @author Chee Bin HOH
 * @date 2026-08-31
 *
 * Overview
 * --------
 * This header combines @ref Dmn_State with @ref Dmn_Runtime_Manager to execute
 * state-machine steps asynchronously on the process-wide runtime thread.
 * Clients create a @ref Dmn_Runtime_State through
 * @ref Dmn_Runtime_State_Manager, configure it with the inherited
 * @ref Dmn_State API, and submit it with Dmn_Runtime_State::run().
 *
 * Ownership and Lifetime
 * ----------------------
 * State handles are shared pointers. Once a state is submitted, the manager
 * retains an owning handle until it reaches a terminal outcome, ensuring queued
 * and running work cannot access a destroyed state. Implementations should use
 * shared_from_this() only after a state is owned by a @c std::shared_ptr.
 *
 * Thread Safety
 * -------------
 * Public lifecycle operations and state inspection are thread-safe. State
 * functors and lifecycle hooks execute in the runtime async thread. Blocking
 * wait and shutdown operations are prohibited from that thread to avoid
 * deadlock.
 */

#ifndef DMN_RUNTIME_STATE_HPP_
#define DMN_RUNTIME_STATE_HPP_

#include "dmn-runtime.hpp"
#include "dmn-singleton.hpp"
#include "dmn-state.hpp"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace dmn {

/**
 * @class Dmn_Runtime_State
 * @brief A runtime-managed state machine instance.
 *
 * Dmn_Runtime_State subclasses Dmn_State and adds asynchronous runtime
 * ownership semantics: a client obtains a shared_ptr handle from the
 * manager, configures state functors using the same Dmn_State API, then
 * calls run() to schedule execution on the global runtime thread.
 *
 * Lifecycle
 * ---------
 * A state is configured, submitted once, and then completes, fails, or is
 * cancelled. Cancellation is cooperative: it prevents subsequent state steps
 * but does not interrupt a functor already executing. Completion is published
 * through a @c std::shared_future<void>, which supports multiple waiters.
 */
class Dmn_Runtime_State
    : public Dmn_State,
      public std::enable_shared_from_this<Dmn_Runtime_State> {
  friend class Dmn_Runtime_State_Manager;

public:
  /**
   * @brief Callback invoked by the runtime when a state step throws.
   *
   * The callback receives the exception captured by the runtime job.
   */
  using OnErrorFnc = Dmn_Runtime_Job::OnErrorFncType;

  /**
   * @brief Construct a runtime-managed state object with a human-readable name.
   * @param name Human-readable name used for diagnostics.
   */
  explicit Dmn_Runtime_State(std::string_view name);

  /**
   * @brief Destroy the state after all owning handles have been released.
   */
  virtual ~Dmn_Runtime_State() noexcept;

  Dmn_Runtime_State(const Dmn_Runtime_State &) = delete;
  Dmn_Runtime_State &operator=(const Dmn_Runtime_State &) = delete;
  Dmn_Runtime_State(Dmn_Runtime_State &&) = delete;
  Dmn_Runtime_State &operator=(Dmn_Runtime_State &&) = delete;

  /* Execution control */

  /**
   * @brief Request cooperative cancellation of this state.
   *
   * The cancellation is idempotent and thread-safe. It does NOT preempt a
   * currently-running functor unless that functor explicitly observes the
   * cancel flag via isCancelled(). The runtime job must check isCancelled()
   * and call setEnd() prior to invoking runNext() if cancellation is set.
   *
   * A state cancelled before submission becomes terminal immediately. A
   * submitted state becomes terminal when the runtime observes the request.
   */
  void cancel();

  /**
   * @brief Schedule this state handle for runtime execution.
   *
   * @param priority Job priority to use when enqueuing (maps to
   * Dmn_Runtime_Job::Priority).
   * @param delay If non-zero, the first job is scheduled via addTimedJob()
   * after this delay. Later state steps are posted immediately.
   * @param onError Optional error callback forwarded to the runtime job. The
   *                type matches Dmn_Runtime_Job::OnErrorFncType.
   * @return true if the state was successfully queued; false for an already
   *         terminal, cancelled, active, or unconfigured state, or when the
   *         manager has shut down.
   *
   * Notes:
   * - run() is one-shot for a given handle: the first successful call enqueues
   *   the state; subsequent calls return false.
   * - Calling run() from inside the runtime async thread is disallowed and
   *   throws std::runtime_error.
   *
   * @throws std::bad_weak_ptr if this object is not owned by a
   *         @c std::shared_ptr.
   * @throws std::runtime_error if called from the runtime async thread.
   */
  bool
  run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
      const std::chrono::steady_clock::duration &delay =
          std::chrono::steady_clock::duration::zero(),
      OnErrorFnc onError = {});

  /* Completion waiting */

  /**
   * @brief Return a shared_future that becomes ready when the state reaches
   *        a terminal condition (completed/failed/cancelled).
   *
   * The shared_future is available immediately after the state is created
   * so callers may register waiters before run() is called.
   *
   * @return A copyable completion future. Calling @c get() on it rethrows a
   *         state-step failure.
   */
  std::shared_future<void> getFuture();

  /**
   * @brief Block until the state reaches a terminal condition.
   *
   * Calling wait() from the runtime async thread is disallowed and throws
   * std::runtime_error. See getFuture() for async waiting.
   *
   * @throws std::runtime_error if called from the runtime async thread.
   */
  void wait();

  /**
   * @brief Block until the state is terminal or the timeout expires.
   * @param timeout Maximum duration to wait.
   * @return true if terminal observed before timeout, false otherwise.
   * @throws std::runtime_error if called from the runtime async thread.
   */
  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout);

  /* State inspection */

  /**
   * @brief Return whether cancellation was requested or completed.
   */
  bool isCancelled() const;

  /** @brief Return whether the state completed successfully. */
  bool isCompleted() const;

  /** @brief Return whether a state step failed. */
  bool isFailed() const;

  /** @brief Return whether the state is queued or executing a step. */
  bool isRunning() const;

protected:
  /**
   * @brief Restrict state-machine advancement to this type and its manager.
   *
   * Derived classes may use @ref runNext to implement specialized execution,
   * but clients cannot drive a runtime-managed state directly.
   */
  using Dmn_State::runNext;

  /**
   * @brief Lifecycle hooks for derived implementations.
   *
   * Subclasses may override these to observe state lifecycle transitions. The
   * default implementations are no-ops.
   */
  /** @brief Called once before the first state step executes. */
  virtual void onStarted();

  /** @brief Called after normal terminal completion is published. */
  virtual void onCompleted();

  /**
   * @brief Called after a state-step failure is published.
   * @param ep Exception raised by the failed state step.
   */
  virtual void onFailed(std::exception_ptr ep);

  /** @brief Called after cancellation is published as terminal. */
  virtual void onCancelled();

private:
  enum class Terminal_State { kCompleted, kFailed, kCancelled };

  /** @brief Begin a runtime step and invoke @ref onStarted exactly once. */
  bool beginStep();

  /**
   * @brief Publish one terminal outcome and invoke its corresponding hook.
   * @param terminalState Outcome to publish.
   * @param failure Exception to associate with a failed outcome.
   *
   * @note A pending cancellation request takes precedence over normal
   *       completion so shutdown cannot publish conflicting terminal states.
   */
  void complete(Terminal_State terminalState, std::exception_ptr failure = {});
  /** @brief Clear submission state after the manager declines or cannot queue
   * it. */
  void resetQueuedAfterSubmission();

  mutable std::mutex m_mutex{}; ///< Protects lifecycle state and completion.
  std::promise<void> m_completionPromise{}; ///< Fulfilled on terminal state.
  std::shared_future<void>
      m_completionSharedFuture{}; ///< Copyable completion notification.
  std::exception_ptr m_failure{}; ///< Captured state-function failure.
  bool m_queued{};                ///< True after successful submission setup.
  bool m_running{}; ///< True after the first state step begins until terminal.
  bool m_started{}; ///< Ensures onStarted runs once.
  bool m_completed{}; ///< Terminal normal-completion marker.
  bool m_failed{};    ///< Terminal failure marker.
  bool m_cancelled{}; ///< Cancellation requested or terminal.
  bool m_terminal{};  ///< Guards one-time completion publication.
};

/**
 * @brief Shared ownership handle for a runtime-managed state.
 *
 * The manager returns this handle from createState() and retains an additional
 * handle after successful submission until the state becomes terminal. Retain
 * this handle to configure, submit, inspect, or await the state.
 */
using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

/**
 * @class Dmn_Runtime_State_Manager
 * @brief Singleton manager for runtime-managed states.
 *
 * The manager is created through its inherited @ref createInstance factory.
 * It creates state handles and retains submitted states until a terminal
 * outcome, thereby preventing pending runtime work from outliving its state.
 */
class Dmn_Runtime_State_Manager
    : public Dmn_Singleton<Dmn_Runtime_State_Manager> {
  friend class Dmn_Singleton<Dmn_Runtime_State_Manager>;

public:
  /**
   * @brief Destroy the runtime state manager singleton.
   *
   * The destructor does not drain pending runtime work. Call @ref shutdown
   * while the runtime main loop remains active before releasing the manager.
   */
  virtual ~Dmn_Runtime_State_Manager() noexcept;

  Dmn_Runtime_State_Manager(const Dmn_Runtime_State_Manager &obj) = delete;
  Dmn_Runtime_State_Manager &
  operator=(const Dmn_Runtime_State_Manager &obj) = delete;
  Dmn_Runtime_State_Manager(Dmn_Runtime_State_Manager &&obj) = delete;
  Dmn_Runtime_State_Manager &
  operator=(Dmn_Runtime_State_Manager &&obj) = delete;

  /**
   * @brief Create a client-owned runtime state handle.
   *
   * The manager does not retain the returned handle until run() successfully
   * queues the state for execution. This factory remains available after
   * @ref shutdown, but such a handle cannot be submitted.
   *
   * @param name Human-readable state name used for diagnostics.
   * @return A newly constructed state owned by the caller.
   */
  DmnRuntimeStatePtr createState(std::string_view name = "");

  /**
   * @brief Cancel submitted states and wait for the runtime to drain them.
   *
   * Shutdown is idempotent. It permanently rejects new state submissions:
   * handles created after shutdown remain configurable, but @ref
   * Dmn_Runtime_State::run returns false. Submitted states are cancelled
   * cooperatively; an executing user step may finish before publishing
   * cancellation, while queued steps finalize without running another
   * user-defined callback.
   *
   * This method waits without holding the manager mutex until every state
   * retained when shutdown began has reached a terminal outcome. The runtime
   * main loop must remain active so queued work can observe cancellation.
   *
   * @throws std::runtime_error if called from the runtime async thread.
   */
  void shutdown();

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
  friend class Dmn_Runtime_State;

  /**
   * @brief Retain and submit a state for its first or subsequent step.
   *
   * The retained handle guarantees state lifetime until @ref releaseState.
   * @return true when the job was accepted; false if shutdown rejects an
   *         initial submission.
   */
  bool enqueueState(DmnRuntimeStatePtr state,
                    Dmn_Runtime_Job::Priority priority,
                    const std::chrono::steady_clock::duration &delay,
                    Dmn_Runtime_State::OnErrorFnc onError);

  /**
   * @brief Execute one state step and repost or terminally release it.
   *
   * @param state Non-owning state handle captured by a runtime job.
   * @param priority Priority to reuse when posting the next step.
   * @param onError Runtime error callback for the next step.
   */
  void executeStateStep(std::weak_ptr<Dmn_Runtime_State> state,
                        Dmn_Runtime_Job::Priority priority,
                        Dmn_Runtime_State::OnErrorFnc onError);

  /**
   * @brief Release the manager's retained handle after terminal completion.
   * @param state State whose manager-owned handle is removed.
   */
  void releaseState(const Dmn_Runtime_State *state);

  std::mutex m_pendingStatesMutex{}; ///< Protects retained state handles.
  std::unordered_map<const Dmn_Runtime_State *, DmnRuntimeStatePtr>
      m_pendingStates{}; ///< States retained while queued or running.
  bool m_shutdown{}; ///< Protected by m_pendingStatesMutex; rejects new runs.
  std::string m_name{}; ///< Human-readable manager name for diagnostics.
};

template <typename Rep, typename Period>
bool Dmn_Runtime_State::wait_for(
    const std::chrono::duration<Rep, Period> &timeout) {
  if (Dmn_Runtime_Manager<>::createInstance()->isRunInAsyncThread()) {
    throw std::runtime_error(
        "Dmn_Runtime_State::wait_for cannot run in the runtime async thread");
  }

  return getFuture().wait_for(timeout) == std::future_status::ready;
}

} // namespace dmn

#endif // DMN_RUNTIME_STATE_HPP_
