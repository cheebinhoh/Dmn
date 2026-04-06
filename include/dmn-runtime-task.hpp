/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-task.hpp
 * @brief Coroutine support types for Dmn_Runtime_Manager runtime
 * integration, used by the public runtime API.
 */

#ifndef DMN_RUNTIME_TASK_HPP_
#define DMN_RUNTIME_TASK_HPP_

#include <coroutine>
#include <exception>
#include <utility>

namespace dmn {

/**
 * @brief A small RAII wrapper around coroutine frame for Dmn_Runtime_Job
 */
struct Dmn_Runtime_Task {
  /**
   * @brief Promise type required by the C++ coroutine machinery.
   *
   * Controls suspension points, return value handling, and exception
   * propagation for @c Dmn_Runtime_Task coroutines.
   */
  struct promise_type {
    /** @brief Return the @c Dmn_Runtime_Task handle that wraps this promise. */
    Dmn_Runtime_Task get_return_object() {
      return Dmn_Runtime_Task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /** @brief Suspend the coroutine immediately on entry so the scheduler
     *         controls when it first runs. */
    std::suspend_always initial_suspend() { return {}; }

    /**
     * @brief Custom final awaiter that resumes the registered continuation
     *        (if any) when this coroutine finishes.
     */
    struct FinalAwaiter {
      /** @brief Never ready — always suspend to allow continuation transfer. */
      bool await_ready() const noexcept { return false; }

      /** @brief Resume the stored continuation (if any) via symmetric transfer. */
      void await_suspend(std::coroutine_handle<promise_type> h) const noexcept {
        if (h && h.promise().m_continuation) {
          h.promise().m_continuation.resume();
        }
      }

      /** @brief No-op: the coroutine frame is already finished at this point. */
      void await_resume() const noexcept {}
    };

    /** @brief Return the custom final awaiter that chains the continuation. */
    FinalAwaiter final_suspend() noexcept { return {}; }

    /** @brief Capture any exception thrown by the coroutine body for later
     *         rethrowing via @c Awaiter::await_resume(). */
    void unhandled_exception() { m_except = std::current_exception(); }

    /** @brief Coroutines that return @c void reach this on co_return. */
    void return_void() {}

    std::coroutine_handle<>
        m_continuation; ///< Handle of the awaiting coroutine (continuation).
    std::exception_ptr
        m_except{}; ///< Stores any exception thrown by the coroutine body.
  };

  /**
   * @brief Minimal awaitable interface for @c Dmn_Runtime_Task.
   *
   * @c Dmn_Runtime_Task is intentionally not a general-purpose awaitable type.
   * Its lifetime and execution are driven by @c Dmn_Runtime_Manager's
   * scheduler.  Library consumers should normally interact with higher-level
   * runtime abstractions instead of awaiting this type directly.
   *
   * However, to preserve backward compatibility for existing code that
   * previously did @c co_await @c Dmn_Runtime_Task, a minimal awaitable
   * interface is provided via @c operator @c co_await.  This simply wires the
   * awaiting coroutine into the task's continuation and propagates any stored
   * exception.
   */
  struct Awaiter {
    std::coroutine_handle<promise_type>
        m_handle; ///< Handle to the task coroutine being awaited.

    /** @brief Return @c true when the task has already completed (or the
     *         handle is null), so no suspension is needed. */
    bool await_ready() const noexcept { return !m_handle || m_handle.done(); }

    std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> awaiting) const noexcept {
      // Record the awaiting coroutine as the continuation and use symmetric
      // transfer to start the task without re-entrant resume.
      if (m_handle) {
        m_handle.promise().m_continuation = awaiting;
        return m_handle;
      }
      // Fallback: nothing to resume (should not be hit when await_ready() is
      // false), so transfer to a no-op coroutine.
      return std::noop_coroutine();
    }

    /** @brief Resume after task completion; rethrow any exception stored in
     *         the promise. */
    void await_resume() {
      if (m_handle) {
        auto &promise = m_handle.promise();
        if (promise.m_except) {
          std::rethrow_exception(promise.m_except);
        }
      }
    }
  };

  /** @brief Return an @c Awaiter that suspends the caller until this task
   *         finishes (non-const overload). */
  [[nodiscard]] Awaiter operator co_await() noexcept {
    return Awaiter{m_handle};
  }

  /** @brief Return an @c Awaiter that suspends the caller until this task
   *         finishes (const overload). */
  [[nodiscard]] Awaiter operator co_await() const noexcept {
    return Awaiter{m_handle};
  }

  /** @brief Destroy the coroutine frame if this task still owns a valid
   *         handle. */
  ~Dmn_Runtime_Task() noexcept {
    if (m_handle) {
      m_handle.destroy();
    }
  }

  /**
   * @brief Construct a task that takes ownership of the given coroutine handle.
   *
   * @param h Coroutine handle to take ownership of.
   */
  explicit Dmn_Runtime_Task(std::coroutine_handle<promise_type> h) noexcept
      : m_handle(h) {}

  Dmn_Runtime_Task(const Dmn_Runtime_Task &) = delete;
  Dmn_Runtime_Task &operator=(const Dmn_Runtime_Task &) = delete;

  /**
   * @brief Move constructor — transfers ownership of the coroutine handle.
   *
   * @param other The source task; its handle is set to @c nullptr after the
   *              move.
   */
  Dmn_Runtime_Task(Dmn_Runtime_Task &&other) noexcept
      : m_handle(std::exchange(other.m_handle, nullptr)) {}

  /**
   * @brief Move assignment — transfers ownership of the coroutine handle.
   *
   * @param other The source task; its handle is set to @c nullptr after the
   *              move.
   * @return Reference to @c *this.
   */
  Dmn_Runtime_Task &operator=(Dmn_Runtime_Task &&other) noexcept {
    if (this != &other) {
      if (m_handle) {
        m_handle.destroy();
      }

      m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
  }

  /**
   * @brief Return @c true if this task holds a valid (non-null) coroutine
   *        frame.
   *
   * @return @c true when the internal coroutine handle is non-null.
   */
  [[nodiscard]] bool isValid() const { return m_handle ? true : false; }

  std::coroutine_handle<promise_type> m_handle;
};

} // namespace dmn

#endif // DMN_RUNTIME_TASK_HPP_
