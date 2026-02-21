/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-async.cpp
 * @brief Implementation of Dmn_Async: a small helper that serializes
 *        asynchronous execution of client-provided tasks and provides
 *        optional rendezvous points for callers that need to wait for
 *        completion.
 *
 * Dmn_Async_Wait::wait() blocks the calling thread until the
 * associated task finishes (or propagates its exception).
 *
 * Dmn_Async::addExecTask() wraps the user function in a try/catch so
 * that exceptions thrown by tasks do not terminate the background
 * thread.
 *
 * addExecTaskWithWait() additionally captures any thrown exception in
 * the Dmn_Async_Wait object so the waiter can observe it via wait().
 *
 * addExecTaskAfterInternal() re-enqueues itself until the target wall-
 * clock time has been reached, then executes the user function.
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

void Dmn_Async::Dmn_Async_Wait::wait() { fut.get(); }

Dmn_Async::Dmn_Async(std::string_view name)
    : Dmn_Pipe{name, [](std::function<void()> &&task) -> void {
                 std::move(task)();
                 Dmn_Proc::yield();
               }} {}

Dmn_Async::~Dmn_Async() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_Async::addExecTask(std::function<void()> fnc) {
  this->write([fnc = std::move(fnc)]() -> void {
    if (fnc) {
      fnc();
    }
  });
}

auto Dmn_Async::addExecTaskWithWait(std::function<void()> fnc)
    -> std::shared_ptr<Dmn_Async::Dmn_Async_Wait> {

  return addExecTaskAfterWithWait(std::chrono::seconds(0), fnc);
}

void Dmn_Async::addExecTaskAfterInternal(long long time_in_future,
                                         std::function<void()> fnc) {
  this->write([this, time_in_future, fnc = std::move(fnc)]() -> void {
    const long long now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    if (now >= time_in_future) {
      if (fnc) {
        fnc();
      }
    } else {
      this->addExecTaskAfterInternal(time_in_future, fnc);
    }
  });
}

} // namespace dmn
