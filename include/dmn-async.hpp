/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-async.hpp
 * @brief Header for Dmn_Async: a small helper that serializes asynchronous
 * execution of client-provided tasks and provides optional rendezvous points
 * for callers that need to wait for completion of a task.
 *
 * Design pattern
 * --------------
 * Adaptor - it adapts the pipe to support asynchronous task and runtime.
 *
 * Usage summary
 * -------------
 * - A client can inherit from Dmn_Async or hold an instance of it.
 * - The client passes work as a std::function<void()> to Dmn_Async's
 *   addExecTask* APIs. Immediate tasks are serialized in queue order;
 *   delayed tasks wait for their steady-clock deadline and can be overtaken by
 *   immediate tasks. This serialization applies to submitted work; it does not
 *   synchronize unrelated access to client state.
 * - Delayed submissions reject negative durations and throw
 *   std::overflow_error when the duration or computed deadline cannot be
 *   represented by the steady clock. Zero duration is accepted.
 * - Delayed tasks use Dmn_Pipe's deadline-aware scheduled writes, so the worker
 *   blocks instead of polling while a task is not yet due. Destruction drains
 *   accepted tasks and can therefore wait until pending deadlines.
 * - For callers that need to block until a submitted task finishes, use
 *   addExecTaskWithWait()/addExecTaskAfterWithWait(), which return a
 *   Dmn_Async_Handle object whose wait() method will only return after the task
 *   has completed (and propagate exceptions thrown by the task).
 *
 * This class is useful for offloading work from fast API paths while
 * preserving ordering and providing optional synchronization points.
 */

#ifndef DMN_ASYNC_HPP_
#define DMN_ASYNC_HPP_

#include "dmn-blockingqueue-mt.hpp"
#include "dmn-pipe.hpp"

#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

/// @name Convenience macros for submitting asynchronous tasks
/// @{

/**
 * @def DMN_ASYNC_CALL_WITH_COPY_CAPTURE
 * @brief Submit a task with copy-capture semantics (captures everything by
 * value) to the calling @c Dmn_Async instance.
 *
 * Expands to a call to @c this->addExecTask() with a lambda that captures
 * all referenced variables by value.
 *
 * @param block A complete statement (the body of the lambda).
 */
#define DMN_ASYNC_CALL_WITH_COPY_CAPTURE(block)                                \
  do {                                                                         \
    this->addExecTask([=]() mutable -> void { block; });                       \
  } while (false)

/**
 * @def DMN_ASYNC_CALL_WITH_REF_CAPTURE
 * @brief Submit a task with reference-capture semantics (captures everything
 * by reference) to the calling @c Dmn_Async instance.
 *
 * Expands to a call to @c this->addExecTask() with a lambda that captures
 * all referenced variables by reference.
 *
 * @warning The caller must ensure that all captured variables remain valid
 * until the task executes.
 *
 * @param block A complete statement (the body of the lambda).
 */
#define DMN_ASYNC_CALL_WITH_REF_CAPTURE(block)                                 \
  do {                                                                         \
    this->addExecTask([&]() mutable -> void { block; });                       \
  } while (false)

/**
 * @def DMN_ASYNC_CALL_WITH_CAPTURE
 * @brief Submit a task with a custom capture list to the calling @c Dmn_Async
 * instance.
 *
 * Expands to a call to @c this->addExecTask() with a lambda whose capture
 * list is provided as a variadic argument.
 *
 * @param block       A complete statement (the body of the lambda).
 * @param ...         The capture list for the lambda (e.g., @c x, @c &y).
 */
#define DMN_ASYNC_CALL_WITH_CAPTURE(block, ...)                                \
  do {                                                                         \
    this->addExecTask([__VA_ARGS__]() mutable -> void { block; });             \
  } while (false)

/// @}

namespace dmn {

template <template <class> class QueueType = Dmn_BlockingQueue_Mt>
class Dmn_Async {
  using Clock = std::chrono::steady_clock;

  template <class Rep, class Period>
  static auto toClockDuration(std::chrono::duration<Rep, Period> duration)
      -> Clock::duration {
    using ClockRep = Clock::duration::rep;
    static_assert(
        std::is_integral_v<ClockRep> &&
            std::numeric_limits<ClockRep>::is_signed,
        "steady_clock duration representation must be signed integer");
    static_assert(std::numeric_limits<ClockRep>::radix == 2,
                  "steady_clock duration representation must be binary");

    const std::chrono::duration<long double, Clock::period> clock_ticks{
        duration};
    const long double rounded_ticks = std::ceil(clock_ticks.count());
    const auto max_exponent = std::numeric_limits<long double>::max_exponent;
    const long double exclusive_max =
        std::numeric_limits<ClockRep>::digits < max_exponent
            ? std::ldexp(1.0L, std::numeric_limits<ClockRep>::digits)
            : std::numeric_limits<long double>::infinity();
    if (!std::isfinite(rounded_ticks) || rounded_ticks < 0 ||
        rounded_ticks >= exclusive_max) {
      throw std::overflow_error("Dmn_Async delay exceeds steady_clock range");
    }

    return Clock::duration{static_cast<ClockRep>(rounded_ticks)};
  }

  static auto nextDeadline(Clock::duration delay) -> Clock::time_point {
    using Rep = Clock::duration::rep;
    const auto now = Clock::now();
    const auto now_ticks = now.time_since_epoch().count();
    const auto delay_ticks = delay.count();

    if (now_ticks > 0 &&
        delay_ticks > std::numeric_limits<Rep>::max() - now_ticks) {
      throw std::overflow_error(
          "Dmn_Async deadline exceeds steady_clock range");
    }

    return Clock::time_point{Clock::duration{now_ticks + delay_ticks}};
  }

public:
  // A simple rendezvous object returned to callers that want to wait for a
  // previously submitted asynchronous task to finish. Calling wait() blocks
  // until the task has completed. If the task threw, the stored exception
  // will be rethrown to the waiter.
  class Dmn_Async_Handle {
    template <template <class> class> friend class Dmn_Async;

  public:
    /**
     * @brief Construct a handle around the task to be executed.
     *
     * Scheduling is managed by the owning @c Dmn_Async pipe; the handle only
     * provides task execution and completion synchronization. The optional
     * timestamp is retained for source compatibility and is not used to
     * schedule the task; submit delayed work through @c Dmn_Async.
     *
     * @param fnc The task callable to wrap.
     * @param due_in_future Legacy timestamp argument; ignored.
     */
    explicit Dmn_Async_Handle(std::function<void()> fnc,
                              [[maybe_unused]] long long due_in_future = 0)
        : m_fnc{std::move(fnc)} {
      m_fut = m_p.get_future();
    }

    ~Dmn_Async_Handle() = default;

    Dmn_Async_Handle(const Dmn_Async_Handle &obj) = delete;
    Dmn_Async_Handle &operator=(const Dmn_Async_Handle &obj) = delete;
    Dmn_Async_Handle(Dmn_Async_Handle &&obj) = delete;
    Dmn_Async_Handle &operator=(Dmn_Async_Handle &&obj) = delete;

    /**
     * @brief Block until the associated task has finished.
     *
     * If the task threw an exception, it is rethrown here in the calling
     * thread.
     */
    void wait() { m_fut.get(); }

  private:
    std::function<void()> m_fnc{}; ///< The task callable.

    std::promise<void>
        m_p{}; ///< Promise fulfilled when the task completes (or throws).
    std::future<void>
        m_fut{}; ///< Future used by wait() to block until completion.
  }; // class Dmn_Async_Handle

  /**
   * @brief Construct a Dmn_Async helper.
   *
   * @param name Optional textual identifier for debugging/logging.
   */
  explicit Dmn_Async(std::string_view name = "");
  virtual ~Dmn_Async() noexcept;

  Dmn_Async(const Dmn_Async &obj) = delete;
  Dmn_Async &operator=(const Dmn_Async &obj) = delete;
  Dmn_Async(Dmn_Async &&obj) = delete;
  Dmn_Async &operator=(Dmn_Async &&obj) = delete;

  /**
   * @brief Submit a callable task for asynchronous execution.
   *
   * @param func The callable task to execute asynchronously.
   * @param args The arguments to the callable task.
   */
  template <typename Callable, typename... Args>
  void addExecTask(Callable &&func, Args &&...args);

  /**
   * @brief Submit a callable task for asynchronous execution and get a task
   * wait object.
   *
   * The returned shared_ptr points to a Dmn_Async_Handle; calling wait() on it
   * will block until the submitted task has finished. If the task throws an
   * exception, wait() will rethrow it.
   *
   * @param func The callable task to execute asynchronously.
   * @param args The arguments to the callable task.
   *
   * @return shared_ptr<Dmn_Async_Handle> Rendezvous object for task completion.
   */
  template <typename Callable, typename... Args>
  auto addExecTaskWithWait(Callable &&func,
                           Args &&...args) -> std::shared_ptr<Dmn_Async_Handle>;

  /**
   * @brief Schedule a callable task to run after the given duration has
   * elapsed.
   *
   * The task will not be executed before the duration has passed. It may not
   * execute precisely at the moment the duration elapses (scheduling is not
   * real-time), but execution will occur at or after the specified time.
   * Its handle's @c wait() rethrows exceptions from the task. Immediate tasks
   * are processed first while a delayed task waits for its deadline.
   * The worker waits for the deadline rather than repeatedly polling; queued
   * immediate tasks retain priority over tasks awaiting their deadlines.
   *
   * @param duration Time to wait before executing the task.
   * @param func The callable task to execute asynchronously.
   * @param args The arguments to the callable task.
   * @throws std::invalid_argument if @p duration is negative.
   * @throws std::overflow_error if @p duration or its deadline cannot be
   * represented by the steady clock.
   */
  template <class Rep, class Period, typename Callable, typename... Args>
  void addExecTaskAfter(const std::chrono::duration<Rep, Period> &duration,
                        Callable &&func, Args &&...args);

  /**
   * @brief Same as addExecTaskAfter(), but returns a task wait object so the
   * caller can block until the scheduled task finishes.
   *
   * The task will not be executed before the duration has passed. It may not
   * execute precisely at the moment the duration elapses (scheduling is not
   * real-time), but execution will occur at or after the specified time.
   *
   * The returned shared_ptr points to a Dmn_Async_Handle; calling wait() on it
   * will block until the submitted task has finished. If the task throws an
   * exception, wait() will rethrow it.
   *
   * @param duration Time to wait before executing the task.
   * @param func The callable task to execute asynchronously.
   * @param args The arguments to the callable task.
   *
   * @return shared_ptr<Dmn_Async_Handle> Rendezvous object for task completion.
   * @throws std::invalid_argument if @p duration is negative.
   * @throws std::overflow_error if @p duration or its deadline cannot be
   * represented by the steady clock.
   */
  template <class Rep, class Period, typename Callable, typename... Args>
  auto
  addExecTaskAfterWithWait(const std::chrono::duration<Rep, Period> &duration,
                           Callable &&func,
                           Args &&...args) -> std::shared_ptr<Dmn_Async_Handle>;

  /**
   * @brief Block until the async is empty and no task pending to be executed.
   */
  void waitForEmpty();

private:
  using BasePipe = Dmn_Pipe<std::shared_ptr<Dmn_Async_Handle>,
                            QueueType<std::shared_ptr<Dmn_Async_Handle>>>;

  std::string m_name{};
  std::unique_ptr<BasePipe> m_pipe{};
}; // class Dmn_Async

template <template <class> class QueueType>
Dmn_Async<QueueType>::Dmn_Async(std::string_view name) : m_name{name} {
  m_pipe = std::make_unique<BasePipe>(
      m_name,
      [](std::shared_ptr<Dmn_Async::Dmn_Async_Handle> task) -> void {
        try {
          if (task->m_fnc) {
            task->m_fnc();
          }

          task->m_p.set_value();
        } catch (...) {
          task->m_p.set_exception(std::current_exception());
        }

        Dmn_Proc::yield();
      },
      1, 0, true);
}

template <template <class> class QueueType>
Dmn_Async<QueueType>::~Dmn_Async() noexcept try {
  m_pipe.reset(); // this will initialize shutdown and destroy process
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <template <class> class QueueType>
template <class Rep, class Period, typename Callable, typename... Args>
void Dmn_Async<QueueType>::addExecTaskAfter(
    const std::chrono::duration<Rep, Period> &duration, Callable &&func,
    Args &&...args) {
  this->addExecTaskAfterWithWait(duration, std::forward<Callable>(func),
                                 std::forward<Args>(args)...);
}

template <template <class> class QueueType>
template <class Rep, class Period, typename Callable, typename... Args>
auto Dmn_Async<QueueType>::addExecTaskAfterWithWait(
    const std::chrono::duration<Rep, Period> &duration, Callable &&func,
    Args &&...args) -> std::shared_ptr<Dmn_Async_Handle> {
  if (duration < std::chrono::duration<Rep, Period>::zero()) {
    throw std::invalid_argument("Dmn_Async delay must not be negative");
  }

  const auto clock_duration = toClockDuration(duration);
  const auto deadline = nextDeadline(clock_duration);

  auto bound_task = [f = std::forward<Callable>(func),
                     ... captured_args = std::forward<Args>(args)]() mutable {
    std::invoke(std::move(f), std::move(captured_args)...);
  };

  auto task_shared_ptr =
      std::make_shared<Dmn_Async::Dmn_Async_Handle>(std::move(bound_task));
  auto task_ret = task_shared_ptr;

  this->m_pipe->writeAt(deadline, task_shared_ptr);

  return task_ret;
}

template <template <class> class QueueType>
template <typename Callable, typename... Args>
void Dmn_Async<QueueType>::addExecTask(Callable &&func, Args &&...args) {
  addExecTaskWithWait(std::forward<Callable>(func),
                      std::forward<Args>(args)...);
}

template <template <class> class QueueType>
template <typename Callable, typename... Args>
auto Dmn_Async<QueueType>::addExecTaskWithWait(Callable &&func, Args &&...args)
    -> std::shared_ptr<Dmn_Async::Dmn_Async_Handle> {
  auto bound_task = [f = std::forward<Callable>(func),
                     ... captured_args = std::forward<Args>(args)]() mutable {
    std::invoke(std::move(f), std::move(captured_args)...);
  };

  auto task_shared_ptr =
      std::make_shared<Dmn_Async::Dmn_Async_Handle>(std::move(bound_task));
  auto task_ret = task_shared_ptr;

  this->m_pipe->write(task_shared_ptr);

  return task_ret;
}

template <template <class> class QueueType>
void Dmn_Async<QueueType>::waitForEmpty() {
  m_pipe->waitForEmpty();
}

} // namespace dmn

#endif // DMN_ASYNC_HPP_
