/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.hpp
 * @brief Public API for the Runtime State Engine: a runtime-owned state
 *        execution engine that composes the existing dmn-runtime scheduler
 *        with the dmn-state finite-state helper.
 *
 * @author Chee Bin HOH
 * @date 2026-08-31
 *
 * This header declares the public types used by clients to create and
 * manage runtime-managed state machine instances. Implementation details
 * are intentionally omitted from the header; refer to the implementation
 * (.cpp) and specification documents for full behavior.
 */

#ifndef DMN_RUNTIME_STATE_HPP_
#define DMN_RUNTIME_STATE_HPP_

#include "dmn-runtime.hpp"
#include "dmn-state.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>

namespace dmn {

/**
 * @class Dmn_Runtime_State
 * @brief A runtime-managed state machine instance.
 *
 * Dmn_Runtime_State subclasses Dmn_State and adds asynchronous runtime
 * ownership semantics: a client obtains a shared_ptr handle from the
 * engine, configures state functors using the same Dmn_State API, then
 * calls run() to schedule execution on the global runtime thread.
 *
 * Important behavior (summary):
 * - createState() returns std::shared_ptr<Dmn_Runtime_State> — the engine
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
 *   disallowed: implementations MUST assert/throw when detected.
 */
class Dmn_Runtime_State : public Dmn_State {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;
  using OnErrorFnc = Dmn_Runtime_Job::OnErrorFncType; // std::function<void(std::exception_ptr &)>

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
   * @param priority Job priority to use when enqueuing (maps to Dmn_Runtime_Job::Priority).
   * @param delay If non-zero, the job is scheduled via addTimedJob() after this delay.
   * @param onError Optional error callback forwarded to the runtime job. The
   *                type matches Dmn_Runtime_Job::OnErrorFncType.
   * @return true if the state was successfully queued; false if enqueue
   *         failed (already terminal, duplicate run, invalid configuration).
   *
   * Notes:
   * - run() is one-shot for a given handle: the first successful call enqueues
   *   the state; subsequent calls return false.
   * - Calling run() from inside the runtime async thread is disallowed and
   *   will assert/throw.
   */
  bool run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
           const std::chrono::steady_clock::duration &delay = std::chrono::steady_clock::duration::zero(),
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
   * Calling wait() from the runtime async thread is disallowed; implementations
   * should assert or throw if detected. See getFuture() for async waiting.
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
   * isRunning(): queued or actively running inside the engine.
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
 * @class Dmn_Runtime_State_Engine
 * @brief Singleton factory and manager for runtime-managed states.
 *
 * Responsibilities:
 * - provide createState() returning a shared_ptr handle to a Dmn_Runtime_State
 * - retain a shared_ptr to queued/running state objects to guarantee lifetime
 * - integrate with Dmn_Runtime_Manager by creating runtime jobs for state steps
 */
class Dmn_Runtime_State_Engine : public Dmn_Singleton<Dmn_Runtime_State_Engine> {
public:
  using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

  /**
   * @brief Obtain the singleton engine instance.
   */
  static auto createInstance() -> Dmn_Runtime_State_Engine &;

  /**
   * @brief Create a new runtime-managed state object and return a shared_ptr
   *        handle to the client. The engine will also hold a shared_ptr while
   *        the state is queued or running.
   */
  DmnRuntimeStatePtr createState(std::string_view name);

protected:
  Dmn_Runtime_State_Engine();
  virtual ~Dmn_Runtime_State_Engine() noexcept;
};

} // namespace dmn

#endif // DMN_RUNTIME_STATE_HPP_
