/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-async.hpp
 * @brief Header for Dmn_Async: a small helper that serializes asynchronous
 *        execution of client-provided tasks and provides optional rendezvous
 *        points for callers that need to wait for completion.
 *
 *        Usage summary:
 *        - A client can inherit from Dmn_Async or hold an instance of it.
 *        - The client passes work as a std::function<void()> to Dmn_Async's
 *          write()/addExecTask* APIs. Dmn_Async will schedule the work to be
 *          executed asynchronously, serializing execution and avoiding the
 *          need for explicit mutexes in the client API.
 *        - For callers that need to block until a submitted task finishes,
 *          use addExecTaskWithWait()/addExecTaskAfterWithWait(), which return
 *          a Dmn_Async_Wait object whose wait() method will only return after
 *          the task has completed (and propagate exceptions thrown by the
 *          task).
 *
 *        This class is useful for offloading work from fast API paths while
 *        preserving ordering and providing optional synchronization points.
 */

#ifndef DMN_ASYNC_HPP_
#define DMN_ASYNC_HPP_

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "dmn-pipe.hpp"

// Helper macros to submit tasks with different capture styles. They call
// write() with a small lambda that captures as requested and forwards the
// captured code as the task body.
#define DMN_ASYNC_CALL_WITH_COPY_CAPTURE(block)                                \
  do {                                                                         \
    this->addExecTask([=]() mutable -> void { block; });                       \
  } while (false)

#define DMN_ASYNC_CALL_WITH_REF_CAPTURE(block)                                 \
  do {                                                                         \
    this->addExecTask([&]() mutable -> void { block; });                       \
  } while (false)

#define DMN_ASYNC_CALL_WITH_CAPTURE(block, ...)                                \
  do {                                                                         \
    this->addExecTask([__VA_ARGS__]() mutable -> void { block; });             \
  } while (false)

namespace dmn {

class Dmn_Async : public Dmn_Pipe<std::function<void()>> {
public:
  // A simple rendezvous object returned to callers that want to wait for a
  // previously submitted asynchronous task to finish. Calling wait() blocks
  // until the task has completed. If the task threw, the stored exception
  // will be rethrown to the waiter.
  class Dmn_Async_Wait {
    friend class Dmn_Async;

  public:
    void wait();

  private:
    std::mutex m_mutex{};
    std::condition_variable m_cond_var{};

    bool m_done{};

    // Stores an exception thrown by the task so wait() can rethrow it.
    std::exception_ptr m_thrownException{};
  };

  /**
   * @brief Construct a Dmn_Async helper.
   *
   * @param name Optional textual identifier for debugging/logging.
   */
  explicit Dmn_Async(std::string_view name = "");
  virtual ~Dmn_Async() noexcept;

  Dmn_Async(const Dmn_Async &obj) = delete;
  const Dmn_Async &operator=(const Dmn_Async &obj) = delete;
  Dmn_Async(Dmn_Async &&obj) = delete;
  Dmn_Async &operator=(Dmn_Async &&obj) = delete;

  /**
   * @brief Submit a task for asynchronous execution.
   *
   * @param fnc The task to execute asynchronously.
   */
  void addExecTask(std::function<void()> fnc);

  /**
   * @brief Submit a task for asynchronous execution and get a wait object.
   *
   * The returned shared_ptr points to a Dmn_Async_Wait; calling wait() on it
   * will block until the submitted task has finished. If the task throws an
   * exception, wait() will rethrow it.
   *
   * @param fnc The task to execute asynchronously.
   * @return shared_ptr<Dmn_Async_Wait> A rendezvous object to wait for task
   *         completion.
   */
  auto addExecTaskWithWait(std::function<void()> fnc)
      -> std::shared_ptr<Dmn_Async_Wait>;

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
   * @brief Same as addExecTaskAfter(), but returns a wait object so the
   * caller can block until the scheduled task finishes.
   *
   * @param duration Time to wait before executing the task.
   * @param fnc The task to execute.
   * @return shared_ptr<Dmn_Async_Wait> Rendezvous object for task completion.
   */
  template <class Rep, class Period>
  auto
  addExecTaskAfterWithWait(const std::chrono::duration<Rep, Period> &duration,
                           std::function<void()> fnc)
      -> std::shared_ptr<Dmn_Async_Wait>;

private:
  using Dmn_Pipe::read;
  using Dmn_Pipe::readAndProcess;
  using Dmn_Pipe::write;

  /**
   * @brief Internal helper that schedules a task to run at a specific time in
   * the future (expressed as nanoseconds since epoch).
   *
   * @param time_in_future Target time (nanoseconds since epoch) when the
   *                       task should be eligible to run.
   * @param fnc The task to execute.
   */
  void addExecTaskAfterInternal(long long time_in_future,
                                std::function<void()> fnc);

  // If a submitted task throws, the exception can be captured here. This
  // member is distinct from per-wait exceptions stored on Dmn_Async_Wait.
  std::exception_ptr m_thrownException{};
}; // class Dmn_Async

template <class Rep, class Period>
void Dmn_Async::addExecTaskAfter(
    const std::chrono::duration<Rep, Period> &duration,
    std::function<void()> fnc) {
  long long time_in_future =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  this->addExecTaskAfterInternal(time_in_future, fnc);
}

template <class Rep, class Period>
auto Dmn_Async::addExecTaskAfterWithWait(
    const std::chrono::duration<Rep, Period> &duration,
    std::function<void()> fnc) -> std::shared_ptr<Dmn_Async::Dmn_Async_Wait> {
  auto wait_shared_ptr = std::make_shared<Dmn_Async::Dmn_Async_Wait>();

  this->addExecTaskAfter(duration, [wait_shared_ptr, fnc]() {
    try {
      fnc();
    } catch (...) {
      wait_shared_ptr->m_thrownException = std::current_exception();
    }

    std::unique_lock<std::mutex> lock(wait_shared_ptr->m_mutex);
    wait_shared_ptr->m_done = true;
    wait_shared_ptr->m_cond_var.notify_all();
  });

  return wait_shared_ptr;
}

} // namespace dmn

#endif // DMN_ASYNC_HPP_
