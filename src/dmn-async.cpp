/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-async.cpp
 * @brief Implementation of Dmn_Async: a small helper that serializes
 *        asynchronous execution of client-provided tasks and provides
 *        optional rendezvous points for callers that need to wait for
 *        completion.
 *
 * Dmn_Async_Task::wait() blocks the calling thread until the
 * associated task finishes (or propagates its exception).
 *
 * Dmn_Async::addExecTask() wraps the user function in a try/catch so
 * that exceptions thrown by tasks do not terminate the background
 * thread.
 *
 * addExecTaskWithWait() additionally captures any thrown exception in
 * the Dmn_Async_Wait object so the task waiter can observe it via wait().
 */

#include "dmn-async.hpp"

#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"

namespace dmn {

void Dmn_Async_Task::wait() { m_fut.get(); }

Dmn_Async::Dmn_Async(std::string_view name)
    : Dmn_Pipe{
          name, [this](std::shared_ptr<Dmn_Async_Task> task) -> void {
            try {
              if (task->m_future > 0) {
                const long long now =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

                if (now >= task->m_future) {
                  task->m_fnc();
                  task->m_p.set_value();
                } else {
                  this->write(task);
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
          }} {}

Dmn_Async::~Dmn_Async() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_Async::addExecTask(std::function<void()> fnc) {
  addExecTaskWithWait(fnc);
}

auto Dmn_Async::addExecTaskWithWait(std::function<void()> fnc)
    -> std::shared_ptr<Dmn_Async_Task> {
  auto task_shared_ptr = std::make_shared<Dmn_Async_Task>(fnc);
  auto task_ret = task_shared_ptr;

  this->write(task_shared_ptr);

  return task_ret;
}

} // namespace dmn
