/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
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
 *   write()/addExecTask* APIs. Dmn_Async will schedule the work to be executed
 *   asynchronously in the order of the submitted tasks and avoiding the need
 * for explicit mutexes in the client API.
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
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string_view>

/// @name Convenience macros for submitting asynchronous tasks
/// @{

/**
 * @def DMN_ASYNC_CALL_WITH_COPY_CAPTURE
 * @brief Submit a task with copy-capture semantics (captures everything by
 *        value) to the calling @c Dmn_Async instance.
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
 *        by reference) to the calling @c Dmn_Async instance.
 *
 * Expands to a call to @c this->addExecTask() with a lambda that captures
 * all referenced variables by reference.
 *
 * @warning The caller must ensure that all captured variables remain valid
 *          until the task executes.
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
 *        instance.
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
public:
  // A simple rendezvous object returned to callers that want to wait for a
  // previously submitted asynchronous task to finish. Calling wait() blocks
  // until the task has completed. If the task threw, the stored exception
  // will be rethrown to the waiter.
  class Dmn_Async_Handle {
    template <template <class> class> friend class Dmn_Async;

  public:
    /**
     * @brief Construct a handle for the given task, optionally scheduled at a
     *        future time.
     *
     * @param fnc           The task callable to wrap.
     * @param due_in_future Absolute nanosecond timestamp (from
     *                      @c std::chrono::steady_clock) after which the task
     *                      may execute.  Pass 0 (the default) for immediate
     *                      execution.
     */
    Dmn_Async_Handle(std::function<void()> fnc, long long due_in_future = 0)
        : m_fnc{fnc}, m_due_in_future{due_in_future} {
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
    long long m_due_in_future{};   ///< Earliest execution time as a nanosecond
                                   ///< timestamp; 0 = immediate.

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
   * @brief Submit a task for asynchronous execution.
   *
   * @param fnc The task to execute asynchronously.
   */
  void addExecTask(std::function<void()> fnc);

  /**
   * @brief Submit a task for asynchronous execution and get a task wait object.
   *
   * The returned shared_ptr points to a Dmn_Async_Handle; calling wait() on it
   * will block until the submitted task has finished. If the task throws an
   * exception, wait() will rethrow it.
   *
   * @param fnc The task to execute asynchronously.
   *
   * @return shared_ptr<Dmn_Async_Handle> Rendezvous object for task completion.
   */
  auto addExecTaskWithWait(std::function<void()> fnc)
      -> std::shared_ptr<Dmn_Async_Handle>;

  /**
   * @brief Schedule a task to run after the given duration has elapsed.
   *
   * The task will not be executed before the duration has passed. It may not
   * execute precisely at the moment the duration elapses (scheduling is not
   * real-time), but execution will occur at or after the specified time.
   *
   * @param duration Time to wait before executing the task.
   * @param fnc The task to execute.
   */
  template <class Rep, class Period>
  void addExecTaskAfter(const std::chrono::duration<Rep, Period> &duration,
                        std::function<void()> fnc);

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
   * @param fnc The task to execute.
   *
   * @return shared_ptr<Dmn_Async_Handle> Rendezvous object for task completion.
   */
  template <class Rep, class Period>
  auto
  addExecTaskAfterWithWait(const std::chrono::duration<Rep, Period> &duration,
                           std::function<void()> fnc)
      -> std::shared_ptr<Dmn_Async_Handle>;

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
      [this](std::shared_ptr<Dmn_Async::Dmn_Async_Handle> task) -> void {
        try {
          if (task->m_due_in_future > 0) {
            const long long now =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();

            if (now >= task->m_due_in_future) {
              if (task->m_fnc) {
                task->m_fnc();
              }

              task->m_p.set_value();
            } else {
              this->m_pipe->write(task);
            }
          } else {
            if (task->m_fnc) {
              task->m_fnc();
            }

            task->m_p.set_value();
          }
        } catch (...) {
          task->m_p.set_exception(std::current_exception());
        }

        Dmn_Proc::yield();
      });
}

template <template <class> class QueueType>
Dmn_Async<QueueType>::~Dmn_Async() noexcept try {
  m_pipe.reset(); // this will initialize shutdown and destroy process
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <template <class> class QueueType>
template <class Rep, class Period>
void Dmn_Async<QueueType>::addExecTaskAfter(
    const std::chrono::duration<Rep, Period> &duration,
    std::function<void()> fnc) {
  this->addExecTaskAfterWithWait(duration, fnc);
}

template <template <class> class QueueType>
template <class Rep, class Period>
auto Dmn_Async<QueueType>::addExecTaskAfterWithWait(
    const std::chrono::duration<Rep, Period> &duration,
    std::function<void()> fnc) -> std::shared_ptr<Dmn_Async::Dmn_Async_Handle> {
  long long time_in_future =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count() +
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  auto task_shared_ptr =
      std::make_shared<Dmn_Async::Dmn_Async_Handle>(fnc, time_in_future);
  auto task_ret = task_shared_ptr;

  this->m_pipe->write(task_shared_ptr);

  return task_ret;
}

template <template <class> class QueueType>
void Dmn_Async<QueueType>::addExecTask(std::function<void()> fnc) {
  addExecTaskWithWait(fnc);
}

template <template <class> class QueueType>
auto Dmn_Async<QueueType>::addExecTaskWithWait(std::function<void()> fnc)
    -> std::shared_ptr<Dmn_Async::Dmn_Async_Handle> {
  auto task_shared_ptr = std::make_shared<Dmn_Async::Dmn_Async_Handle>(fnc);
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
