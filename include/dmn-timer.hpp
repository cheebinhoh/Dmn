/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-timer.hpp
 * @brief A lightweight recurring timer (watchdog) class template.
 *
 * Dmn_Timer<T> runs a user-provided callback repeatedly at a fixed relative
 * interval specified by a duration type T (for example,
 * std::chrono::milliseconds or std::chrono::seconds). The timer guarantees the
 * callback will not be invoked before the specified interval has elapsed.
 * However, it does not guarantee immediate invocation at the exact moment the
 * interval expires: callback execution may be delayed due to scheduling, the
 * callback's own execution time, or other runtime factors.
 *
 * The implementation uses Dmn_Proc to run a background execution context for
 * the timer loop. Callback exceptions derived from std::exception are caught
 * and reported via DMN_DEBUG_PRINT. Other exception types escape the pthread
 * entry point and normally cause std::terminate.
 *
 * Public API summary:
 *  - Dmn_Timer(const T &reltime, std::function<void()> fn):
 *      Constructs the timer and starts it immediately with the given interval
 *      and callback.
 *  - void start(const T &reltime, std::function<void()> fn = {}):
 *      Stops and joins any existing timer before updating its interval and
 *      optional callback, then attempts to start a new thread. If fn is empty,
 *      the previously set callback is retained. Thread-creation failure is
 *      currently not reported.
 *  - void stop():
 *      Requests cancellation and joins the running timer. Exceptions from
 *      cancellation or joining are suppressed.
 *
 * Notes:
 *  - Copy and move constructors/operators are deleted.
 *  - The destructor is noexcept.
 *  - Template parameter T should be a std::chrono::duration-like type.
 */

#ifndef DMN_TIMER_HPP_

#define DMN_TIMER_HPP_

#include "dmn-debug.hpp"
#include "dmn-proc.hpp"

#include <chrono>
#include <functional>
#include <thread>
#include <utility>

namespace dmn {

template <typename T> class Dmn_Timer : public Dmn_Proc {
public:
  /**
   * @brief Construct and immediately start a recurring timer.
   *
   * @param reltime Interval between consecutive invocations of @p fn.
   * @param fn      Callback invoked each time the interval elapses.
   */
  Dmn_Timer(const T &reltime, std::function<void()> fn);

  /**
   * @brief Destroy the timer, stopping the background thread if it is running.
   */
  virtual ~Dmn_Timer() noexcept;

  Dmn_Timer(const Dmn_Timer &obj) = delete;
  Dmn_Timer &operator=(const Dmn_Timer &obj) = delete;
  Dmn_Timer(Dmn_Timer &&obj) = delete;
  Dmn_Timer &operator=(Dmn_Timer &&obj) = delete;

  /**
   * @brief Start (or restart) the timer with the given interval and callback.
   *
   * Any currently running timer is stopped and joined before the interval or
   * callback is updated and the new timer is started. If @p fn is empty the
   * previously set callback is retained. Callers must serialize calls to
   * start(), stop(), and destruction of this timer.
   *
   * @param reltime Interval between consecutive callback invocations.
   * @param fn      Optional new callback.  If empty, the existing callback
   * stored from construction or a previous call to start() is
   * reused.
   */
  void start(const T &reltime, std::function<void()> fn = {});

  /**
   * @brief Stop the running timer.
   *
   * Requests cancellation and joins the background thread. Exceptions from
   * cancellation or joining are suppressed. The timer may be restarted later
   * by calling start().
   */
  void stop();

private:
  std::function<void()> m_fn{}; ///< Callback invoked repeatedly by the timer.
  T m_reltime{}; ///< Interval between consecutive callback invocations.
}; // class Dmn_Timer

template <typename T>
Dmn_Timer<T>::Dmn_Timer(const T &reltime, std::function<void()> fn)
    : Dmn_Proc{"timer"}, m_fn{fn}, m_reltime{reltime} {
  this->start(this->m_reltime, this->m_fn);
}

template <typename T> Dmn_Timer<T>::~Dmn_Timer() noexcept try {
  this->stop();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T>
void Dmn_Timer<T>::start(const T &reltime, std::function<void()> fn) {
  this->stopExec();

  m_reltime = reltime;

  if (fn) {
    m_fn = std::move(fn);
  }

  this->exec([this]() {
    while (true) {
      std::this_thread::sleep_for(this->m_reltime);
      Dmn_Proc::yield();

      try {
        if (m_fn) {
          this->m_fn();
        }
      } catch (const std::exception &e) {
        DMN_DEBUG_PRINT(std::cerr << e.what() << "\n");
      }
    }
  });
}

template <typename T> void Dmn_Timer<T>::stop() {
  try {
    this->stopExec();
  } catch (...) {
  }
}

} // namespace dmn

#endif // DMN_TIMER_HPP_
