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

namespace dmn {

/**
 * @brief A small RAII wrapper around coroutine frame for Dmn_Runtime_Job.
 *
 * Dmn_Runtime_Task is the coroutine return type used by
 * Dmn_Runtime_Manager's job scheduling infrastructure.  It owns the
 * coroutine frame (std::coroutine_handle) and destroys it on
 * destruction.
 *
 * Lifecycle:
 *  - The coroutine is suspended at its first suspension point
 *    (initial_suspend returns std::suspend_always).
 *  - Dmn_Runtime_Manager resumes and schedules the handle explicitly.
 *  - When the coroutine body returns, FinalAwaiter::await_suspend
 *    resumes any registered continuation (the awaiting coroutine).
 *
 * Move-only: copy is disabled to ensure unique ownership of the frame.
 */
struct Dmn_Runtime_Task {
  /**
   * @brief Coroutine promise type required by the C++20 coroutine machinery.
   *
   * Satisfies the requirements of a coroutine promise object:
   *  - get_return_object() wraps the frame handle in a Dmn_Runtime_Task.
   *  - initial_suspend() suspends immediately so the scheduler controls
   *    first execution.
   *  - final_suspend() uses a custom FinalAwaiter to resume a registered
   *    continuation (if any) when the coroutine body finishes.
   *  - unhandled_exception() stores the exception for later re-throw by
   *    Awaiter::await_resume().
   */
  struct promise_type {
    /// @brief Create the Dmn_Runtime_Task wrapper that represents this frame.
    Dmn_Runtime_Task get_return_object() {
      return Dmn_Runtime_Task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    /// @brief Suspend the coroutine immediately on creation.
    std::suspend_always initial_suspend() { return {}; }

    /**
     * @brief Custom final awaiter that resumes the registered continuation.
     *
     * When the coroutine body finishes (or returns), final_suspend() returns
     * this awaiter.  await_suspend() uses symmetric transfer to resume the
     * coroutine that is awaiting this task (stored in m_continuation), which
     * avoids unbounded stack growth from recursive resumes.
     */
    struct FinalAwaiter {
      /// @brief Never ready so await_suspend is always called.
      bool await_ready() const noexcept { return false; }

      /**
       * @brief Resume the continuation (awaiting coroutine) if one is set.
       *
       * Implements symmetric transfer by returning the continuation handle
       * to the coroutine runtime instead of calling resume() directly.
       *
       * @param h Handle to the finishing coroutine.
       * @return Handle of the continuation to resume, or an empty handle if
       *         there is no continuation.
       */
      std::coroutine_handle<>
      await_suspend(std::coroutine_handle<promise_type> h) const noexcept {
        if (h) {
          return h.promise().m_continuation;
        }
        return std::coroutine_handle<>{};
      }

      /// @brief No value to retrieve after final suspension.
      void await_resume() const noexcept {}
    };

    /// @brief Suspend at the end and resume any waiting continuation.
    FinalAwaiter final_suspend() noexcept { return {}; }

    /// @brief Capture any exception thrown inside the coroutine body.
    void unhandled_exception() { m_except = std::current_exception(); }

    /// @brief Called when the coroutine body executes a plain @c co_return.
    void return_void() {}

    std::coroutine_handle<>
        m_continuation; ///< Coroutine to resume on completion.
    std::exception_ptr
        m_except{}; ///< Exception thrown by the coroutine body (if any).
  };

  /**
   * Dmn_Runtime_Task is intentionally not a general-purpose awaitable type.
   *
   * Its lifetime and execution are driven by Dmn_Runtime_Manager's scheduler.
   * Library consumers should normally interact with higher-level runtime
   * abstractions instead of awaiting this type directly.
   *
   * However, to preserve backward compatibility for existing code that
   * previously did `co_await Dmn_Runtime_Task`, we provide a minimal
   * awaitable interface via `operator co_await`. This simply wires the
   * awaiting coroutine into the task's continuation and propagates any
   * stored exception.
   */

  /**
   * @brief Awaiter returned by @c operator co_await to allow one coroutine to
   *        wait for this task to complete.
   *
   * When @c co_await task is used, @c await_suspend stores the calling
   * coroutine as the continuation and uses symmetric transfer to start the
   * task.  When the task finishes, FinalAwaiter resumes the caller and
   * @c await_resume re-throws any stored exception.
   */
  struct Awaiter {
    std::coroutine_handle<promise_type>
        m_handle; ///< Handle to the awaited task.

    /// @brief True if the task has already finished (no suspension needed).
    bool await_ready() const noexcept { return !m_handle || m_handle.done(); }

    /**
     * @brief Register @p awaiting as the continuation and start the task.
     *
     * Uses symmetric transfer (returning the task's handle) to resume it
     * without growing the call stack.
     *
     * @param awaiting Handle of the coroutine that is @c co_await-ing this
     * task.
     * @return The task's handle to resume via symmetric transfer, or
     *         std::noop_coroutine() if the handle is null.
     */
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

    /**
     * @brief Re-throw any exception captured by the task, or return normally.
     *
     * @throws Any exception thrown inside the coroutine body.
     */
    void await_resume() {
      if (m_handle) {
        auto &promise = m_handle.promise();
        if (promise.m_except) {
          std::rethrow_exception(promise.m_except);
        }
      }
    }
  };

  /**
   * @brief Return an Awaiter so this task can be @c co_await-ed.
   * @return Awaiter wrapping this task's handle.
   */
  [[nodiscard]] Awaiter operator co_await() noexcept {
    return Awaiter{m_handle};
  }

  /// @copydoc operator co_await()
  [[nodiscard]] Awaiter operator co_await() const noexcept {
    return Awaiter{m_handle};
  }

  /// @brief Destroy the coroutine frame on destruction.
  ~Dmn_Runtime_Task() noexcept {
    if (m_handle) {
      m_handle.destroy();
    }
  }

  /**
   * @brief Construct a task that takes ownership of the given coroutine handle.
   * @param h The coroutine handle to own (must not be null for a valid task).
   */
  explicit Dmn_Runtime_Task(std::coroutine_handle<promise_type> h) noexcept
      : m_handle(h) {}

  Dmn_Runtime_Task(const Dmn_Runtime_Task &) = delete;
  Dmn_Runtime_Task &operator=(const Dmn_Runtime_Task &) = delete;

  /**
   * @brief Move constructor: transfer ownership of the coroutine handle.
   * @param other The task to move from (left in a valid but empty state).
   */
  Dmn_Runtime_Task(Dmn_Runtime_Task &&other) noexcept
      : m_handle(std::exchange(other.m_handle, nullptr)) {}

  /**
   * @brief Move assignment: destroy any current frame, then take ownership.
   * @param other The task to move from.
   * @return *this
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
   * @brief Return whether this task holds a valid (non-null) coroutine handle.
   * @return @c true if the handle is non-null, @c false otherwise.
   */
  [[nodiscard]] bool isValid() const {
    // Returns true if the handle points to a coroutine frame
    return m_handle ? true : false;
  }

  std::coroutine_handle<promise_type>
      m_handle; ///< Owned coroutine frame handle.
};

} // namespace dmn

#endif // DMN_RUNTIME_TASK_HPP_
