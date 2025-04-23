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

#ifndef DMN_ASYNC_HPP_

#define DMN_ASYNC_HPP_

#include <chrono>
#include <functional>
#include <string_view>

#include "dmn-pipe.hpp"

#define DMN_ASYNC_CALL_WITH_COPY_CAPTURE(block)                                \
  do {                                                                         \
    this->write([=]() mutable { block; });                                     \
  } while (false)

#define DMN_ASYNC_CALL_WITH_REF_CAPTURE(block)                                 \
  do {                                                                         \
    this->write([&]() mutable { block; });                                     \
  } while (false)

#define DMN_ASYNC_CALL_WITH_CAPTURE(block, ...)                                \
  do {                                                                         \
    this->write([__VA_ARGS__]() mutable { block; });                           \
  } while (false)

namespace dmn {

class Dmn_Async : public Dmn_Pipe<std::function<void()>> {
public:
  Dmn_Async(std::string_view name = "");
  ~Dmn_Async() noexcept;

  Dmn_Async(const Dmn_Async &dmnAsync) = delete;
  const Dmn_Async &operator=(const Dmn_Async &dmnAsync) = delete;
  Dmn_Async(Dmn_Async &&dmnAsync) = delete;
  Dmn_Async &operator=(Dmn_Async &&dmnAsync) = delete;

  /**
   * @brief The method will execute the asynchronous task after duration
   *        is elapsed, the task will NOT be executed before duration is
   *        elapsed, but might not be guaranteed that it is executed in
   *        exact moment that the duration is elapsed.
   *
   * @param duration time in duraton that must be elapsed before task
   *                 is executed
   * @param fn       asynchronous task to be executed
   */
  template <class Rep, class Period>
  void execAfter(const std::chrono::duration<Rep, Period> &duration,
                 std::function<void()> fn);

private:
  /**
   * @brief The method will execute the asynchronous task after duration
   *        is elapsed, the task will NOT be executed before duration is
   *        elapsed, but might not be guaranteed that it is executed in
   *        exact moment that the duration is elapsed.
   *
   * @param timeInFuture nanoseconds that must be elapsed before the
   *                     task is executed
   * @param fn           asynchronous task to be executed
   */
  void execAfterInternal(long long timeInFuture, std::function<void()> fn);
}; // class Dmn_Async

template <class Rep, class Period>
void Dmn_Async::execAfter(const std::chrono::duration<Rep, Period> &duration,
                          std::function<void()> fn) {
  long long timeInFuture =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

  this->execAfterInternal(timeInFuture, fn);
}

} // namespace dmn

#endif // DMN_ASYNC_HPP_
