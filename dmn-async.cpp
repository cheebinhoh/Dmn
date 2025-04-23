/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This class implements base class for other class to adapt and implementing
 * asynchronous execution API.
 *
 * A client class can inherit from Dmn_Async or composes an Dmn_Async object,
 * and implement the client class API to pass a functor to the Dmn_Async object
 * for execution on behalf of the client API call' execution. This will help
 * serialize multiple the API call executions, avoid any explicit mutex lock on
 * client API calls, and more important is that it can shorten the latency of
 * calling the client API and returns from the API call for functionalities that
 * does not need to be synchronized between caller and callee (see
 * dmn-pub-sub.hpp for an example usage of this class).
 */

#include "dmn-async.hpp"

#include <chrono>
#include <functional>
#include <string_view>

#include "dmn-pipe.hpp"

namespace Dmn {

Dmn_Async::Dmn_Async(std::string_view name)
    : Dmn_Pipe{name, [](std::function<void()> &&task) {
                 task();
                 Dmn_Proc::yield();
               }} {}

Dmn_Async::~Dmn_Async() noexcept try { this->waitForEmpty(); } catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_Async::execAfterInternal(long long timeInFuture,
                                  std::function<void()> fn) {
  this->write([this, timeInFuture, fn]() {
    long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    if (now >= timeInFuture) {
      if (fn) {
        fn();
      }
    } else {
      this->execAfterInternal(timeInFuture, fn);
    }
  });
}

} // namespace Dmn
