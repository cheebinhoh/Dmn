/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.hpp
 * @brief Public API for runtime-managed Dmn_State execution.
 */

#ifndef DMN_RUNTIME_STATE_HPP_
#define DMN_RUNTIME_STATE_HPP_

#include "dmn-runtime.hpp"
#include "dmn-state.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace dmn {

class Dmn_Runtime_State_Engine;

/**
 * @brief A one-shot Dmn_State executed by Dmn_Runtime_Manager.
 *
 * Instances are created only by Dmn_Runtime_State_Engine. Configure all state
 * callbacks before calling run(). Each callback must call setNext() or
 * setEnd() exactly once before returning.
 */
class Dmn_Runtime_State
    : public Dmn_State,
      public std::enable_shared_from_this<Dmn_Runtime_State> {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;
  using OnErrorFnc = Dmn_Runtime_Job::OnErrorFncType;

  enum class Status {
    kIdle,
    kQueued,
    kRunning,
    kCompleted,
    kFailed,
    kCancelled,
  };

  virtual ~Dmn_Runtime_State() noexcept;

  Dmn_Runtime_State(const Dmn_Runtime_State &obj) = delete;
  Dmn_Runtime_State &operator=(const Dmn_Runtime_State &obj) = delete;
  Dmn_Runtime_State(Dmn_Runtime_State &&obj) = delete;
  Dmn_Runtime_State &operator=(Dmn_Runtime_State &&obj) = delete;

  /**
   * @brief Append or replace a callback while the state is idle.
   *
   * The callback is adapted to Dmn_State's base callback type. It must make
   * exactly one transition through setNext() or setEnd().
   */
  void setStateFnc(FncType fnc, int index = 0);

  /**
   * @brief Select the next state from a currently executing state callback.
   */
  void setNext(int index);

  /**
   * @brief Select the sequential next state from a running callback.
   */
  void setNext();

  /**
   * @brief Finalize after the currently executing state callback returns.
   */
  void setEnd();

  /**
   * @brief Submit this configured handle once to the runtime.
   *
   * A non-zero delay applies only to the initial step. Continuations are
   * submitted immediately at the same priority. Invalid configuration marks
   * the state failed and returns false without invoking @p onError.
   */
  bool
  run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
      const std::chrono::steady_clock::duration &delay =
          std::chrono::steady_clock::duration::zero(),
      OnErrorFnc onError = {});

  /**
   * @brief Request cancellation.
   *
   * Cancelling while idle terminally cancels the handle immediately. A queued
   * or running callback is not preempted; no additional user callback is run
   * after cancellation is observed.
   */
  void cancel();

  /**
   * @brief Wait for a terminal status.
   * @throws std::runtime_error if called from the runtime async thread.
   */
  void wait();

  /**
   * @brief Wait for a terminal status or timeout.
   * @throws std::runtime_error if called from the runtime async thread.
   */
  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout) {
    assertNotRuntimeThread();
    return getFuture().wait_for(timeout) == std::future_status::ready;
  }

  /**
   * @brief Return a multi-waiter completion future.
   *
   * It becomes ready with a value for completed/cancelled states and with the
   * captured exception for failed states. An idle state remains pending until
   * run() or cancel() makes it terminal.
   */
  [[nodiscard]] std::shared_future<void> getFuture() const;

  [[nodiscard]] bool isRunning() const;
  [[nodiscard]] bool isCompleted() const;
  [[nodiscard]] bool isFailed() const;
  [[nodiscard]] bool isCancelled() const;
  [[nodiscard]] Status status() const;

  /**
   * @brief Return the failure that will be rethrown by getFuture().get().
   */
  [[nodiscard]] std::exception_ptr failure() const;

private:
  friend class Dmn_Runtime_State_Engine;

  explicit Dmn_Runtime_State(std::string_view name);
  void assertNotRuntimeThread() const;

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

/**
 * @brief Singleton factory and lifetime manager for runtime state handles.
 */
class Dmn_Runtime_State_Engine
    : public Dmn_Singleton<Dmn_Runtime_State_Engine> {
  friend class Dmn_Singleton<Dmn_Runtime_State_Engine>;

public:
  using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

  enum class ShutdownMode { kGraceful, kImmediate };

  /// Inherited factory returning std::shared_ptr<Dmn_Runtime_State_Engine>.
  using Dmn_Singleton<Dmn_Runtime_State_Engine>::createInstance;

  /**
   * @brief Create an idle, client-configurable runtime state handle.
   */
  [[nodiscard]] DmnRuntimeStatePtr createState(std::string_view name);

  /**
   * @brief Stop accepting new runs permanently.
   *
   * Graceful shutdown lets accepted work finish. Immediate shutdown cancels
   * queued handles and requests cancellation of a running callback.
   */
  void shutdown(ShutdownMode mode);

  [[nodiscard]] bool isShutdown() const;

  virtual ~Dmn_Runtime_State_Engine() noexcept;

private:
  Dmn_Runtime_State_Engine();

  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_RUNTIME_STATE_HPP_
