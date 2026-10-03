/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-proc.hpp
 * @brief Lightweight RAII wrapper around native pthread functionality.
 *
 * Overview
 * --------
 * This header declares Dmn_Proc, a small object-oriented wrapper that
 * encapsulates a pthread and executes a user-provided callable
 * (std::function<void()>) in a separate thread. Instead of varying behaviour
 * through inheritance, Dmn_Proc accepts a task (functor/closure) that the
 * thread runs — this encourages composition over inheritance and reduces the
 * proliferation of subclasses.
 *
 * Key characteristics and expectations
 * ------------------------------------
 * - RAII: Dmn_Proc attempts to cancel and join its thread in its destructor to
 *   free resources. Users should therefore ensure their task cooperates with
 *   pthread cancellation (either by reaching cancellation points or by calling
 *   Dmn_Proc::yield() periodically inside long-running loops).
 * - Cancellation: Thread cancellation via stopExec() is synchronous: if a task
 *   blocks indefinitely without reaching a cancellation point, the thread will
 *   not terminate. Place voluntary cancellation points (e.g. calls to
 *   Dmn_Proc::yield()) in long-running loops if you expect prompt cancellation.
 *
 * Design pattern
 * --------------
 * Command - Implements a variant of the Command design pattern, allowing
 *           clients to submit parameterized requests encapsulated as
 *           std::function<void()> tasks executed by the thread.
 * Bridge - Abstracts the underlying threading implementation from the client.
 * Decorator - Provides an alternative to subclassing for adding additional
 *             responsibilities to the thread object or object that inherits
 *             the Dmn_Proc, a degenerated decorator.
 * Strategy - The provided callback functor serves as a mechanism for strategy
 *            design pattern to varying the functionalities for the thread.
 *
 * Note on mutex cleanup macros
 * The macros below wrap pthread_cleanup_push/pop for the common pattern of
 * unlocking a mutex in cleanup handlers. They are convenience macros and rely
 * on the presence of dmn::cleanupFuncToUnlockPthreadMutex.
 */

#ifndef DMN_PROC_HPP_
#define DMN_PROC_HPP_

#include <atomic>
#include <exception>
#include <functional>
#include <pthread.h>
#include <string>
#include <string_view>

/**
 * @brief Macro wrapper around @c pthread_cleanup_push.
 *
 * Registers a cleanup handler to be called when the current thread is
 * cancelled or when @c DMN_PROC_CLEANUP_POP is executed.  Arguments are
 * forwarded verbatim to @c pthread_cleanup_push.
 */
#define DMN_PROC_CLEANUP_PUSH(...) pthread_cleanup_push(__VA_ARGS__)

/**
 * @brief Macro wrapper around @c pthread_cleanup_pop.
 *
 * Pops the most recently pushed cleanup handler.  If the argument is
 * non-zero, the handler is also executed.  Arguments are forwarded
 * verbatim to @c pthread_cleanup_pop.
 */
#define DMN_PROC_CLEANUP_POP(...) pthread_cleanup_pop(__VA_ARGS__)

namespace dmn {

/**
 * @brief Cleanup function used with pthread_cleanup_push/pop.
 * Expects a pointer to a pthread_mutex_t and unlocks it.
 *
 * This function is declared here so it can be used with the macros above
 * and with pthread_cleanup_push/pop in implementation files.
 *
 * @param arg Pointer to a pthread_mutex_t to be unlocked (void* per pthread
 * API)
 */
void cleanupFuncToUnlockPthreadMutex(void *arg);

/**
 * @brief A small RAII-style wrapper around pthreads that runs a user-provided
 * task (@c std::function<void()>) in a new thread.
 *
 * Behaviour details:
 * - Construct with an optional name and/or task. The name is stored for
 *   diagnostic purposes (no threading name APIs are invoked here).
 * - exec(): start the thread and run the given task (or the previously-set
 *   task from the constructor). Returns true on successful start.
 * - wait(): join the thread, blocking until it completes. Returns true if
 *   the join succeeded; throws if no thread is running or joining fails.
 *   By default, an uncaught task/setup exception terminates the process.
 *   The optional capture policy stores it and rethrows it from wait().
 * - stopExec(): attempt to cancel the running thread and join it. The
 *   destructor uses this to clean up a running thread, which requires the
 *   task to be responsive to pthread cancellation.
 * - ExceptionPolicy defaults to kTerminate to retain legacy behavior. With
 *   kCaptureAndRethrowFromWait, task/setup exceptions are delivered to the
 *   caller of wait() after the thread has been joined.
 *
 * Cancellation warning:
 * - pthread cancellation is cooperative. If the task never reaches a
 *   cancellation point (or explicitly enables deferred cancellation without
 *   checking), cancellation will be delayed indefinitely. For loops that may
 *   run for a long time, call Dmn_Proc::yield() periodically to create
 *   cancellation points (or otherwise ensure the task calls functions that
 *   are cancellation points).
 *
 * Thread-safety:
 * - State, task, and thread-handle access is not internally synchronized.
 *   Callers must ensure lifecycle operations do not overlap and that each
 *   operation happens-before the next one. This may be done by handing control
 *   between caller threads with external synchronization; a single OS thread is
 *   not required. Do not replace the task until the worker has been joined or
 *   destroy the object concurrently with another operation. Destruction stops
 *   and joins a running worker. Derived classes must stop the worker before
 *   destroying members accessed by its task. State accessed by both the task
 *   and callers needs its own synchronization.
 */
class Dmn_Proc {
  /**
   * @brief Lifecycle state of a @c Dmn_Proc instance.
   */
  enum class State {
    kUnknown, ///< Invalid / post-destruction state.
    kNew,     ///< Constructed but no task assigned yet.
    kReady,   ///< Task assigned; ready to be started via exec().
    kRunning, ///< Thread is running; task is executing.
  };

public:
  using Task = std::function<void()>;

  /**
   * @brief Policy for reporting exceptions thrown by the worker task.
   */
  enum class ExceptionPolicy {
    kTerminate, ///< Preserve legacy behavior and terminate the process.
    kCaptureAndRethrowFromWait ///< Store the exception and rethrow from wait().
  };

  /**
   * @brief Construct a Dmn_Proc.
   *
   * @param name Human-readable name for diagnostics/logging.
   * @param fnc Optional task to run when exec() is called. If not provided,
   * a task must be provided to exec().
   * @param exceptionPolicy How exceptions from the worker task are handled.
   * Defaults to @c kTerminate to preserve legacy behavior.
   */
  explicit Dmn_Proc(
      std::string_view name, const Dmn_Proc::Task &fnc = {},
      ExceptionPolicy exceptionPolicy = ExceptionPolicy::kTerminate);
  virtual ~Dmn_Proc() noexcept;

  Dmn_Proc(const Dmn_Proc &obj) = delete;
  Dmn_Proc &operator=(const Dmn_Proc &obj) = delete;
  Dmn_Proc(Dmn_Proc &&obj) = delete;
  Dmn_Proc &operator=(Dmn_Proc &&obj) = delete;

  /**
   * @brief Execute the provided task in a new asynchronous thread.
   *
   * If fnc is empty, the previously-set task (from constructor or setTask)
   * will be used. Returns true on successful thread creation.
   *
   * @param fnc Optional task to run in the new thread.
   *
   * @return true if the thread was started successfully.
   */
  auto exec(const Dmn_Proc::Task &fnc = {}) -> bool;

  /**
   * @brief Wait (join) for the asynchronous thread to finish.
   *
   * If configured with @c ExceptionPolicy::kCaptureAndRethrowFromWait, an
   * exception captured from thread setup or the task is rethrown after the
   * thread is joined and the process returns to the ready state.
   *
   * @return True if the thread was joined successfully.
   * @throws The captured task/setup exception when using the capture policy.
   */
  auto wait() -> bool;

  /**
   * @brief Voluntarily yield execution to allow other threads to run and to
   * create a cooperative cancellation point. Call this inside long-running
   * loops if you expect the thread to be cancellable in a timely manner.
   */
  static void yield();

  /**
   * @brief Voluntarily test if the current thread has a pending cancellation
   * request.
   *
   * This is a deferred cancellation point: if a cancellation request is
   * pending, the thread is terminated at this call site rather than
   * asynchronously.
   */
  static void testcancel();

protected:
  /**
   * @brief Return the current lifecycle state of this Dmn_Proc.
   *
   * @return The current @c State enum value.
   */
  auto getState() const -> Dmn_Proc::State;

  /**
   * @brief Set a lifecycle state and return the previous state.
   *
   * This operation is not atomic; callers must serialize access to the process
   * state, as described by the class thread-safety contract.
   *
   * @param state The new state to set.
   * @return The previous @c State value before the transition.
   */
  auto setState(Dmn_Proc::State state) -> Dmn_Proc::State;

  /**
   * @brief Assign the task that will be executed when exec() is called.
   *
   * The process must be in @c kNew or @c kReady state. After a successful
   * assignment the state transitions to @c kReady.
   *
   * @param fnc The task function to assign. Must be a valid (non-empty)
   * callable.
   */
  void setTask(Dmn_Proc::Task fnc);

  /**
   * @brief Start the underlying pthread and transition to @c kRunning state.
   *
   * @return @c true if the thread was created successfully, @c false otherwise.
   * @throws std::runtime_error if the process is not in @c kReady state.
   */
  auto runExec() -> bool;

  /**
   * @brief Cancel the running thread and wait for it to terminate.
   *
   * If the thread is not currently running, this is a no-op and returns
   * @c true immediately.
   *
   * @return @c true if the thread was stopped (or was not running).
   * @throws std::runtime_error if cancellation or joining fails.
   * @throws The captured task/setup exception when using
   * @c ExceptionPolicy::kCaptureAndRethrowFromWait.
   */
  auto stopExec() -> bool;

  /**
   * @brief Static thread-entry trampoline passed to pthread_create.
   *
   * Sets up deferred cancellation for the new thread and then invokes the
   * stored task.
   *
   * @param context Pointer to the owning @c Dmn_Proc instance.
   * @return Always @c nullptr.
   */
  static auto runFnInThreadHelper(void *context) -> void *;

  const std::string m_name{}; ///< Human-readable name for diagnostics/logging.
  Dmn_Proc::Task m_fnc{};     ///< Task to execute in the thread.
  Dmn_Proc::State m_state{};  ///< Current lifecycle state of this object.
  pthread_t m_th{};           ///< Native pthread handle.
  Dmn_Proc::ExceptionPolicy
      m_exception_policy{};       ///< Worker exception behavior.
  std::exception_ptr m_failure{}; ///< Worker failure handed to the joiner.
  std::atomic_bool m_cancel_requested{}; ///< Marks cancellation initiated
                                         ///< through stopExec().
}; // class Dmn_Proc

} // namespace dmn

#endif // DMN_PROC_HPP_
