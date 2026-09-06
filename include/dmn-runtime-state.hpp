/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.hpp
 * @brief Asynchronous state-machine execution and lifetime management.
 *
 * @author Chee Bin HOH
 * @date 2026-08-31
 *
 * Overview
 * --------
 * This header runs @ref Dmn_State callbacks on the process-wide
 * @ref Dmn_Runtime_Manager thread. Internal initialization and finalization
 * occur in the same runtime dispatch as the surrounding user-state work and
 * are not scheduled separately.
 * Clients create a @ref Dmn_Runtime_State through
 * @ref Dmn_Runtime_State_Manager, configure it with the inherited
 * @ref Dmn_State API or @ref setRuntimeStateFnc(), and submit it with
 * @ref Dmn_Runtime_State::run().
 *
 * Ownership and Lifetime
 * ----------------------
 * State handles are shared pointers. After a successful submission, the
 * manager retains an owning handle until the state completes, fails, or is
 * cancelled. A client may therefore release its handle without invalidating
 * queued or running work.
 *
 * Thread Safety
 * -------------
 * Runtime lifecycle operations and runtime-specific status queries are
 * synchronized. Configure inherited @ref Dmn_State callbacks and transitions
 * from one client thread before submission. During execution, inherited base
 * lifecycle queries should only be read from a callback or after the
 * completion future is ready.
 *
 * User-state callbacks, onStarted(), onCompleted(), and onFailed() execute on
 * the runtime thread. onCancelled() executes in whichever thread publishes
 * cancellation: normally the runtime thread for queued work, but the caller's
 * thread when cancellation completes before successful submission. Blocking
 * wait and shutdown operations are prohibited from the runtime thread to
 * avoid deadlock.
 */

#ifndef DMN_RUNTIME_STATE_HPP_
#define DMN_RUNTIME_STATE_HPP_

#include "dmn-runtime.hpp"
#include "dmn-singleton.hpp"
#include "dmn-state.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace dmn {

/**
 * @class Dmn_Runtime_State
 * @brief A runtime-managed state machine instance.
 *
 * Dmn_Runtime_State subclasses Dmn_State and adds asynchronous runtime
 * ownership semantics: a client obtains a shared_ptr handle from the
 * manager, configures state callbacks using the inherited @ref Dmn_State API
 * or the runtime-aware @ref setRuntimeStateFnc() helper, then calls
 * run() to schedule execution on the global runtime thread.
 *
 * Lifecycle
 * ---------
 * A state is configured, successfully submitted at most once, and then
 * completes, fails, or is cancelled. Cancellation is cooperative: it does not
 * interrupt a callback whose runtime dispatch has started. Completion is
 * published through a @c std::shared_future<void>, which supports multiple
 * waiters.
 *
 * The inherited @ref Dmn_State configuration API remains the client-facing way
 * to install state callbacks before submission. After a successful @ref run,
 * external calls to @ref Dmn_State::setStateFnc, @ref Dmn_State::setNext, and
 * @ref Dmn_State::setEnd throw @c std::logic_error. Manager-driven execution
 * still permits state callbacks to call @ref setNext or @ref setEnd from
 * inside the currently executing user-state callback. External @ref runNext
 * calls are rejected; only the manager may advance the machine.
 *
 * For callbacks that need runtime-only APIs such as @ref isCancelled(), use
 * @ref setRuntimeStateFnc(). It adapts a callback that takes
 * @c Dmn_Runtime_State & into the underlying @ref Dmn_State callback storage
 * while preserving the base API for compatibility.
 *
 * The inherited Dmn_State boolean conversion describes whether configured
 * base-state work remains; it is not a runtime terminal-status query. Use
 * isCompleted(), isFailed(), isCancelled(), or getFuture() to observe the
 * runtime lifecycle.
 */
class Dmn_Runtime_State
    : public Dmn_State,
      public std::enable_shared_from_this<Dmn_Runtime_State> {
  friend class Dmn_Runtime_State_Manager;

public:
  /**
   * @brief Callback invoked when a runtime dispatch throws.
   *
   * The callback receives the exception captured by the runtime job. This
   * includes exceptions from onStarted() and user-state callbacks.
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

  /* Configuration */

  /**
   * @brief Runtime-aware state callback type.
   *
   * Use this when the callback needs runtime-state APIs such as
   * @ref isCancelled(), while still participating in the same underlying
   * state-machine execution model.
   */
  using RuntimeStateFnc = std::function<void(Dmn_Runtime_State &state)>;

  /**
   * @brief Add a runtime-aware user-state callback or replace an existing one.
   * @param fnc Callback invoked with the runtime-managed state object.
   * @param index With N callbacks currently configured, pass 0 (the default)
   *              or N+1 to append a callback. Pass 1 through N to replace the
   *              callback at that state.
   * @throws std::out_of_range if index is negative or greater than N+1.
   *
   * This is a convenience wrapper over the inherited @ref Dmn_State API. It
   * adapts a callback taking @c Dmn_Runtime_State & into the underlying
   * storage used by @ref Dmn_State::setStateFnc() and uses the same indexing
   * rules.
   *
   * @throws std::logic_error if called after successful submission.
   * @throws std::out_of_range for an invalid index.
   */
  void setRuntimeStateFnc(RuntimeStateFnc fnc, int index = 0);

  /* Execution control */

  /**
   * @brief Schedule this state handle for runtime execution.
   *
   * @param priority Job priority to use when enqueuing (maps to
   * Dmn_Runtime_Job::Priority).
   * @param delay If non-zero, the first runtime dispatch is scheduled via
   * addTimedJob() after this delay. Later dispatches are posted immediately.
   * @param onError Optional error callback forwarded to the runtime job. The
   *                type matches Dmn_Runtime_Job::OnErrorFncType.
   * @return true if the state was successfully queued; false for an already
   *         terminal, cancelled, active, or unconfigured state, or when the
   *         manager has shut down.
   *
   * Notes:
   * - run() is one-shot for a given handle: the first successful call enqueues
   *   the state; subsequent calls return false.
   * - A failed enqueue does not consume the one allowed successful
   *   submission, so the caller may retry.
   * - Calling run() from inside the runtime async thread is disallowed and
   *   throws std::runtime_error.
   *
   * @throws std::bad_weak_ptr if this object is not owned by a
   *         @c std::shared_ptr.
   * @throws std::runtime_error if called from the runtime async thread.
   * @throws Any exception raised by the runtime scheduler while enqueuing.
   *         The state remains eligible for another submission attempt.
   */
  bool
  run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
      const std::chrono::steady_clock::duration &delay =
          std::chrono::steady_clock::duration::zero(),
      OnErrorFnc onError = {});

  /**
   * @brief Request cooperative cancellation of this state.
   *
   * The cancellation is idempotent and thread-safe. It does not preempt a
   * user-state callback whose runtime dispatch has started. A callback that
   * needs cooperative early exit should prefer @ref setRuntimeStateFnc() so it
   * can query @ref isCancelled() directly.
   *
   * A state cancelled before submission becomes terminal immediately. A
   * submitted state becomes terminal when the runtime observes the request.
   * Pre-submission cancellation invokes onCancelled() in the calling thread;
   * cancellation of submitted work normally invokes it in the runtime thread.
   */
  void cancel();

  /* Completion waiting */

  /**
   * @brief Return a shared_future that becomes ready when the state reaches
   *        a terminal condition (completed/failed/cancelled).
   *
   * The shared_future is available immediately after the state is created
   * so callers may register waiters before run() is called.
   *
   * @return A copyable completion future. Calling @c get() on it rethrows a
   *         user-state callback failure.
   */
  std::shared_future<void> getFuture();

  /**
   * @brief Block until the state reaches a terminal condition.
   *
   * This method does not rethrow a captured execution failure. Call
   * getFuture().get() when the failure must be observed.
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
   *
   * This method does not rethrow a captured execution failure. Call
   * getFuture().get() when the failure must be observed.
   *
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

  /** @brief Return whether a user-state callback failed. */
  bool isFailed() const;

  /** @brief Return whether the state is queued or has started but not ended. */
  bool isRunning() const;

protected:
  /**
   * @brief Lifecycle hooks for derived implementations.
   *
   * Subclasses may override these to observe state lifecycle transitions. The
   * default implementations are no-ops. Completion is published before a
   * terminal hook runs, so a waiting client may resume while that hook is
   * executing. Overrides of terminal hooks must not throw; their outcome has
   * already been published, and an exception can interrupt manager cleanup.
   * An exception from onStarted() is handled as a state failure before a user
   * callback runs.
   */
  /** @brief Called once when the first runtime dispatch begins. */
  virtual void onStarted();

  /**
   * @brief Called in the runtime thread after successful completion is
   *        published.
   */
  virtual void onCompleted();

  /**
   * @brief Called in the runtime thread after a failure is published.
   * @param ep Exception raised by the failed user-state callback.
   */
  virtual void onFailed(std::exception_ptr ep);

  /**
   * @brief Called after cancellation is published.
   *
   * This runs in the caller's thread for cancellation before submission and
   * normally in the runtime thread for submitted work.
   */
  virtual void onCancelled();

private:
  enum class Terminal_State { kCompleted, kFailed, kCancelled };

  /** @brief Begin a runtime dispatch and invoke @ref onStarted exactly once. */
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

  /**
   * @brief Execute one user state with manager-only execution access.
   *
   * The base operation also performs pending initialization and terminal
   * finalization in this call.
   */
  bool runNextManaged();

  /** @brief Force terminal selection with manager-only execution access. */
  void setEndManaged();

  /** @brief Enter a manager-authorized runtime dispatch. */
  void enterInternalExecution(bool permitRunNext);

  /** @brief Leave a manager-authorized runtime dispatch. */
  void leaveInternalExecution() noexcept;

  void beforeSetStateFnc() override;
  void beforeSetNext() override;
  void beforeSetEnd() override;
  void beforeRunNext() override;

  mutable std::mutex m_mutex{}; ///< Protects lifecycle state and completion.
  std::promise<void> m_completionPromise{}; ///< Fulfilled on terminal state.
  std::shared_future<void>
      m_completionSharedFuture{}; ///< Copyable completion notification.
  std::exception_ptr m_failure{}; ///< Captured dispatch failure.
  bool m_queued{};                ///< True after successful submission setup.
  bool m_running{};   ///< True after the first runtime dispatch until terminal.
  bool m_started{};   ///< Ensures onStarted runs once.
  bool m_completed{}; ///< Terminal normal-completion marker.
  bool m_failed{};    ///< Terminal failure marker.
  bool m_cancelled{}; ///< Cancellation requested or terminal.
  bool m_terminal{};  ///< Guards one-time completion publication.
  unsigned int m_internalExecutionDepth{}; ///< Non-zero only while the manager
                                           ///< drives a dispatch and its
                                           ///< callback-controlled transitions.
  std::thread::id
      m_internalExecutionThread{}; ///< Thread authorized to change transitions.
  bool m_runNextPermitted{}; ///< Consumed when the manager starts one step.
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
   * cooperatively; an executing user callback may finish before publishing
   * cancellation, while queued states finalize without running another
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
   * @brief Retain and submit a state for its first or subsequent dispatch.
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
   * @brief Advance a state and repost or terminally release it.
   *
   * @param state Non-owning state handle captured by a runtime job.
   * @param priority Priority to reuse when posting the next dispatch.
   * @param onError Runtime error callback for the next dispatch.
   *
   * A dispatch executes at most one user callback; cancellation or a
   * previously selected end may execute none.
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
