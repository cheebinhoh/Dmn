/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-proc.cpp
 * @brief Lightweight RAII wrapper around native pthread functionality.
 *
 * This module provides a C++ wrapper around POSIX threads (pthread) with RAII
 * semantics. It manages thread lifecycle and ensures proper cleanup on
 * destruction.
 */

#include "dmn-proc.hpp"

#include <pthread.h>
#include <sched.h>

#include <cassert>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dmn {

/**
 * @brief Helper function to unlock a pthread mutex during thread cancellation.
 *
 * This is used as a cleanup handler registered with pthread_cleanup_push
 * to ensure the mutex is properly released when a thread is cancelled.
 *
 * @param arg Pointer to the pthread_mutex_t to unlock
 */
void cleanupFuncToUnlockPthreadMutex(void *arg) {
  auto *mutex = static_cast<pthread_mutex_t *>(arg);

  pthread_mutex_unlock(mutex);
}

/**
 * @brief Constructs a new Dmn_Proc instance with optional task assignment.
 *
 * Initializes the process in the "New" state. If a task function is provided,
 * it is set and the state transitions to "Ready".
 *
 * @param name Descriptive name for the process (for debugging/logging)
 * @param fnc Optional task function to be executed. Can be nullptr if task
 *            is to be set later via setTask() or exec()
 */
Dmn_Proc::Dmn_Proc(std::string_view name, const Dmn_Proc::Task &fnc)
    : m_name{name} {
  setState(State::kNew);

  if (fnc) {
    setTask(fnc);
  }
}

/**
 * @brief Destructor that ensures proper cleanup of the thread.
 *
 * If the thread is running, it will be stopped (cancelled and joined).
 * Sets state to Unknown to indicate invalid state after destruction.
 * No-throw guarantee - catches and suppresses all exceptions.
 */
Dmn_Proc::~Dmn_Proc() noexcept try {
  if (getState() == State::kRunning) {
    stopExec();
  }

  setState(State::kUnknown);
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

/**
 * @brief Executes the task with optional task replacement.
 *
 * If a new task is provided, it replaces the current task before execution.
 * Transitions the state from Ready to Running by calling runExec().
 *
 * @param fnc Optional new task function to execute. If provided, replaces
 *            the current task. If nullptr, executes the previously set task.
 * @return true if thread creation succeeded, false otherwise
 */
auto Dmn_Proc::exec(const Dmn_Proc::Task &fnc) -> bool {
  if (fnc) {
    setTask(fnc);
  }

  return runExec();
}

/**
 * @brief Retrieves the current state of the process.
 *
 * @return Current State enum value (kNew, kReady, kRunning, kUnknown)
 */
auto Dmn_Proc::getState() const -> Dmn_Proc::State { return m_state; }

/**
 * @brief Sets a new state and returns the previous state.
 *
 * This is an atomic state transition operation. Useful for comparing and
 * swapping states.
 *
 * @param state The new state to set
 * @return The previous state before this call
 */
auto Dmn_Proc::setState(State state) -> Dmn_Proc::State {
  const State old_state = this->m_state;

  this->m_state = state;

  return old_state;
}

/**
 * @brief Assigns a task function to be executed by the thread.
 *
 * The process must be in either New or Ready state. After assignment,
 * the state transitions to Ready, indicating the task is prepared for
 * execution.
 *
 * @param fnc The task function to assign. Must not be nullptr.
 * @throws std::runtime_error if process is not in New or Ready state
 */
void Dmn_Proc::setTask(Dmn_Proc::Task fnc) {
  assert(getState() == State::kNew || getState() == State::kReady);

  this->m_fnc = std::move(fnc);
  setState(State::kReady);
}

/**
 * @brief Waits for the running thread to complete execution.
 *
 * This method performs a pthread_join to synchronously wait for thread
 * termination. After successful join, the state transitions back to Ready.
 *
 * @return true if the join succeeded (err == 0)
 * @throws std::runtime_error if thread is not running or pthread_join fails
 */
auto Dmn_Proc::wait() -> bool {
  int err{};
  void *ret{};

  if (getState() != State::kRunning) {
    throw std::runtime_error("No task is exec");
  }

  err = pthread_join(m_th, &ret);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  setState(State::kReady);

  return 0 == err;
}

/**
 * @brief Tests if the current thread should be cancelled.
 *
 * This is a cancellation point. If a cancellation request is pending,
 * the thread will be terminated at this point.
 */
void Dmn_Proc::testcancel() { pthread_testcancel(); }

/**
 * @brief Yields the current thread and allows cancellation testing.
 *
 * This method combines a cancellation test with a scheduler yield,
 * allowing other threads to run and checking for pending cancellations.
 */
void Dmn_Proc::yield() {
  Dmn_Proc::testcancel();

  sched_yield();
}

/**
 * @brief Stops execution of the running thread.
 *
 * Sends a cancellation request to the thread and waits for it to terminate.
 * If the thread is not running, returns true (no-op on stopped process).
 *
 * @return true if the thread was successfully stopped or was not running
 * @throws std::runtime_error if pthread_cancel fails
 */
auto Dmn_Proc::stopExec() -> bool {
  int err{};

  if (getState() == State::kRunning) {

    err = pthread_cancel(m_th);
    if (0 != err) {
      throw std::runtime_error(std::system_category().message(err));
    }

    return wait();
  } else {
    return true;
  }
}

/**
 * @brief Initiates thread execution with the assigned task.
 *
 * Creates a new pthread and sets its state to Running. The thread is configured
 * to accept deferred cancellations. On failure, state is reverted to previous
 * value.
 *
 * @return true if pthread_create succeeded, false otherwise
 * @throws std::runtime_error if process is not in Ready state
 */
auto Dmn_Proc::runExec() -> bool {
  int err{};
  State old_state{};

  if (getState() != State::kReady) {
    throw std::runtime_error("No task is assigned to the Dmn_Proc (" + m_name +
                             ")");
  }

  old_state = setState(State::kRunning);
  err = pthread_create(&m_th, nullptr, &(Dmn_Proc::runFnInThreadHelper), this);
  if (0 != err) {
    setState(old_state);
    return false;
  }

  return true;
}

/**
 * @brief Helper function executed in the new thread context.
 *
 * Sets up thread-level cancellation policy (enabled with deferred type),
 * then executes the assigned task function. Acts as the entry point for
 * the newly created thread.
 *
 * @param context Pointer to the Dmn_Proc instance managing this thread
 * @return Always returns nullptr
 * @throws std::runtime_error if cancellation setup fails
 */
auto Dmn_Proc::runFnInThreadHelper(void *context) -> void * {
  int old_state{};
  int err{};

  // Enable thread cancellation with deferred cancellation type
  // (cancellation only occurs at cancellation points, not asynchronously)
  err = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  err = pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_state);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  auto *proc = static_cast<Dmn_Proc *>(context);
  proc->m_fnc();

  return nullptr;
}

} // namespace dmn
