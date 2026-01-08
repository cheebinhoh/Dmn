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
 * the timer loop. Exceptions derived from std::exception thrown by the callback
 * are caught and reported via DMN_DEBUG_PRINT; other exception types are
 * swallowed to ensure the timer's execution thread continues running.
 *
 * Public API summary:
 *  - Dmn_Timer(const T &reltime, std::function<void()> fn):
 *      Constructs the timer and starts it immediately with the given interval
 *      and callback.
 *  - void start(const T &reltime, std::function<void()> fn = {}):
 *      Stops any existing timer and starts a new one with the provided interval
 *      and optional callback (if fn is empty, the previously set callback is
 *      retained).
 *  - void stop():
 *      Stops the running timer.
 *
 * Notes:
 *  - Copy and move constructors/operators are deleted.
 *  - The destructor is noexcept.
 *  - Template parameter T should be a std::chrono::duration-like type.
 */

#ifndef DMN_TIMER_HPP_

#define DMN_TIMER_HPP_

#include <chrono>
#include <functional>
#include <thread>

#include "dmn-debug.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T> class Dmn_Timer : public Dmn_Proc {
public:
  Dmn_Timer(const T &reltime, std::function<void()> fn);
  virtual ~Dmn_Timer() noexcept;

  Dmn_Timer(const Dmn_Timer &obj) = delete;
  const Dmn_Timer &operator=(const Dmn_Timer &obj) = delete;
  Dmn_Timer(Dmn_Timer &&obj) = delete;
  Dmn_Timer &operator=(Dmn_Timer &&obj) = delete;

  /**
   * @brief The method starts the timer that executes fn repeatedly after
   *        every reltime interval. Any existing timer is stopped before
   *        the new timer is started.
   *
   * @param reltime The std::chrono::duration timer
   * @param fn      The functor to be run by timer
   */
  void start(const T &reltime, std::function<void()> fn = {});

  /**
   * @brief The method stops the timer.
   */
  void stop();

private:
  /**
   * data members for constructor to instantiate the object.
   */
  std::function<void()> m_fn{};
  T m_reltime{};
}; // class Dmn_Timer

template <typename T>
Dmn_Timer<T>::Dmn_Timer(const T &reltime, std::function<void()> fn)
    : Dmn_Proc{"timer"}, m_fn{fn}, m_reltime{reltime} {
  this->start(this->m_reltime, this->m_fn);
}

template <typename T> Dmn_Timer<T>::~Dmn_Timer() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T>
void Dmn_Timer<T>::start(const T &reltime, std::function<void()> fn) {
  m_reltime = reltime;

  if (fn) {
    m_fn = fn;
  }

  this->stop();

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
