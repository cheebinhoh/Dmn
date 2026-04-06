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
 *        task (@c std::function<void()>) in a new thread.
 *
 * Behaviour details:
 * - Construct with an optional name and/or task. The name is stored for
 *   diagnostic purposes (no threading name APIs are invoked here).
 * - exec(): start the thread and run the given task (or the previously-set
 *   task from the constructor). Returns true on successful start.
 * - wait(): join the thread, blocking until it completes. Returns true if
 *   the join succeeded.
 * - stopExec(): attempt to cancel the running thread. The destructor calls
 *   stopExec() and then waits to join the thread — this makes the object safe
 *   to destroy without leaking the underlying pthread, but it also requires
 *   that the running task be responsive to pthread cancellation.
 *
 * Cancellation warning:
 * - pthread cancellation is cooperative. If the task never reaches a
 *   cancellation point (or explicitly enables deferred cancellation without
 *   checking), cancellation will be delayed indefinitely. For loops that may
 *   run for a long time, call Dmn_Proc::yield() periodically to create
 *   cancellation points (or otherwise ensure the task calls functions that
 *   are cancellation points).
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
   * @brief Construct a Dmn_Proc.
   *
   * @param name Human-readable name for diagnostics/logging.
   * @param fnc Optional task to run when exec() is called. If not provided,
   * a task must be provided to exec().
   */
  explicit Dmn_Proc(std::string_view name, const Dmn_Proc::Task &fnc = {});
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
   * @return True if the thread was joined successfully.
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
   *        request.
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
   * @brief Transition to a new lifecycle state and return the previous one.
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
   *            callable.
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
   * @throws std::runtime_error if pthread_cancel fails.
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
}; // class Dmn_Proc

} // namespace dmn

#endif // DMN_PROC_HPP_
