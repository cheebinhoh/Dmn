/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-proc.hpp
 * @brief Lightweight RAII wrapper around native pthread functionality.
 *
 * This header declares Dmn_Proc, a small object-oriented wrapper that
 * encapsulates a pthread and executes a user-provided callable
 * (std::function<void()>) in a separate thread. Instead of varying behaviour
 * through inheritance, Dmn_Proc accepts a task (functor/closure) that the
 * thread runs — this encourages composition over inheritance and reduces the
 * proliferation of subclasses.
 *
 * Key characteristics and expectations:
 * - RAII: Dmn_Proc attempts to cancel and join its thread in its destructor to
 *   free resources. Users should therefore ensure their task cooperates with
 *   pthread cancellation (either by reaching cancellation points or by calling
 *   Dmn_Proc::yield() periodically inside long-running loops).
 * - Cancellation: Thread cancellation via stopExec() is synchronous: if a task
 *   blocks indefinitely without reaching a cancellation point, the thread will
 *   not terminate. Place voluntary cancellation points (e.g. calls to
 *   Dmn_Proc::yield()) in long-running loops if you expect prompt cancellation.
 *
 * Note on mutex cleanup macros:
 * The macros below wrap pthread_cleanup_push/pop for the common pattern of
 * unlocking a mutex in cleanup handlers. They are convenience macros and rely
 * on the presence of dmn::cleanupFuncToUnlockPthreadMutex.
 */

#ifndef DMN_PROC_HPP_
#define DMN_PROC_HPP_

#include <pthread.h>

#include <functional>
#include <string>
#include <string_view>

/**
 * Macro to push a pthread cleanup handler that will unlock the given mutex
 * when the thread exits or is cancelled inside the protected region.
 *
 * Usage:
 *   DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&mutex);
 *   pthread_mutex_lock(&mutex);
 *   // ... protected code ...
 *   DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();
 *
 * The pair expands to pthread_cleanup_push/ pthread_cleanup_pop and calls
 * dmn::cleanupFuncToUnlockPthreadMutex when executed by pthread cleanup.
 */
#define DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(mutex)                            \
  pthread_cleanup_push(&dmn::cleanupFuncToUnlockPthreadMutex, (mutex))

/**
 * Macro to pop the pthread cleanup handler pushed by
 * DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP. The argument list is left variadic
 * to match typical usage patterns; we intentionally do not execute the cleanup
 * handler here (pop with 0).
 */
#define DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP(...) pthread_cleanup_pop(0)

namespace dmn {

/**
 * Cleanup function used with pthread_cleanup_push/pop.
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
 * Dmn_Proc
 *
 * A small RAII-style wrapper around pthreads that runs a user-provided task
 * (std::function<void()>) in a new thread.
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
  using Task = std::function<void()>;

  enum class State { kInvalid, kNew, kReady, kRunning };

public:
  /**
   * Construct a Dmn_Proc.
   *
   * @param name Human-readable name for diagnostics/logging.
   * @param fnc Optional task to run when exec() is called. If not provided,
   *            a task must be provided to exec().
   */
  explicit Dmn_Proc(std::string_view name, const Dmn_Proc::Task &fnc = {});
  virtual ~Dmn_Proc() noexcept;

  Dmn_Proc(const Dmn_Proc &obj) = delete;
  const Dmn_Proc &operator=(const Dmn_Proc &obj) = delete;
  Dmn_Proc(Dmn_Proc &&obj) = delete;
  Dmn_Proc &operator=(Dmn_Proc &&obj) = delete;

  /**
   * Execute the provided task in a new asynchronous thread.
   *
   * If fnc is empty, the previously-set task (from constructor or setTask)
   * will be used. Returns true on successful thread creation.
   *
   * @param fnc Optional task to run in the new thread.
   * @return true if the thread was started successfully.
   */
  auto exec(const Dmn_Proc::Task &fnc = {}) -> bool;

  /**
   * Wait (join) for the asynchronous thread to finish.
   *
   * @return True if the thread was joined successfully.
   */
  auto wait() -> bool;

  /**
   * Voluntarily yield execution to allow other threads to run and to create a
   * cooperative cancellation point. Call this inside long-running loops if you
   * expect the thread to be cancellable in a timely manner.
   */
  static void yield();

protected:
  auto getState() const -> Dmn_Proc::State;
  auto setState(Dmn_Proc::State state) -> Dmn_Proc::State;
  void setTask(Dmn_Proc::Task fnc);

  /**
   * Internal helpers used by exec/stopExec and the thread entry routine.
   * runExec/stopExec manage starting and stopping the underlying pthread.
   */
  auto runExec() -> bool;
  auto stopExec() -> bool;

private:
  static auto runFnInThreadHelper(void *context) -> void *;

  /**
   * Data members:
   * - m_name: diagnostic name for the thread/object.
   * - m_fnc: current task to execute in the thread.
   * - m_state: internal state machine for lifecycle management.
   * - m_th: native pthread handle.
   */
  const std::string m_name{};

  Dmn_Proc::Task m_fnc{};
  Dmn_Proc::State m_state{};
  pthread_t m_th{};
}; // class Dmn_Proc

} // namespace dmn

#endif // DMN_PROC_HPP_
