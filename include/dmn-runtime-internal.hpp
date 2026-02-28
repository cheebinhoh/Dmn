/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-internal.hpp
 * @brief Internal module and details for Dmn_Runtime_Manager coroutine
 * integration.
 *
 */

#ifndef DMN_RUNTIME_INTERNAL_HPP_
#define DMN_RUNTIME_INTERNAL_HPP_

#include <coroutine>

namespace dmn {

/**
 * @brief A small RAII wrapper around coroutine frame for Dmn_Runtime_Job
 */
struct Dmn_Runtime_Task {
  struct promise_type {
    Dmn_Runtime_Task get_return_object() {
      return Dmn_Runtime_Task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() { return {}; }

    // When the task finishes, resume the "waiter"
    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }

      void await_suspend(std::coroutine_handle<promise_type> h) const noexcept {
        if (h && h.promise().m_continuation) {
          h.promise().m_continuation.resume();
        }
      }

      void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() { m_except = std::current_exception(); }

    void return_void() {}

    std::coroutine_handle<> m_continuation; // The handle of the caller
    std::exception_ptr m_except{};
  };

  /**
   * Dmn_Runtime_Task is intentionally not a general-purpose awaitable type.
   *
   * Its lifetime and execution are driven by Dmn_Runtime_Manager’s scheduler.
   * Library consumers should normally interact with higher-level runtime
   * abstractions instead of awaiting this type directly.
   *
   * However, to preserve backward compatibility for existing code that
   * previously did `co_await Dmn_Runtime_Task`, we provide a minimal
   * awaitable interface via `operator co_await`. This simply wires the
   * awaiting coroutine into the task’s continuation and propagates any
   * stored exception.
   */

  struct Awaiter {
    std::coroutine_handle<promise_type> m_handle;

    bool await_ready() const noexcept { return !m_handle || m_handle.done(); }

    void await_suspend(std::coroutine_handle<> awaiting) const noexcept {
      // Record the awaiting coroutine as the continuation and start the task.
      if (m_handle) {
        m_handle.promise().m_continuation = awaiting;
        m_handle.resume();
      }
    }

    void await_resume() {
      if (m_handle) {
        auto &promise = m_handle.promise();
        if (promise.m_except) {
          std::rethrow_exception(promise.m_except);
        }
      }
    }
  };

  [[nodiscard]] Awaiter operator co_await() noexcept {
    return Awaiter{m_handle};
  }

  [[nodiscard]] Awaiter operator co_await() const noexcept {
    return Awaiter{m_handle};
  }

  ~Dmn_Runtime_Task() noexcept {
    if (m_handle) {
      m_handle.destroy();
    }
  }

  // Construct a task that takes ownership of the given coroutine handle.
  explicit Dmn_Runtime_Task(std::coroutine_handle<promise_type> h) noexcept
      : m_handle(h) {}

  Dmn_Runtime_Task(const Dmn_Runtime_Task &) = delete;
  Dmn_Runtime_Task &operator=(const Dmn_Runtime_Task &) = delete;

  // Move: transfer ownership
  Dmn_Runtime_Task(Dmn_Runtime_Task &&other) noexcept
      : m_handle(std::exchange(other.m_handle, nullptr)) {}

  Dmn_Runtime_Task &operator=(Dmn_Runtime_Task &&other) noexcept {
    if (this != &other) {
      if (m_handle) {
        m_handle.destroy();
      }

      m_handle = std::exchange(other.m_handle, nullptr);
    }
    return *this;
  }

  [[nodiscard]] bool isValid() const {
    // Returns true if the handle points to a coroutine frame
    return m_handle ? true : false;
  }

  std::coroutine_handle<promise_type> m_handle;
};

} // namespace dmn

#endif // DMN_RUNTIME_INTERNAL_HPP_
