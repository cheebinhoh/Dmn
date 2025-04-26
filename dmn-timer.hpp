/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This module a watchdog or timer that executes a callback function
 * repeatedly after certain duration has elapsed. Note that it is
 * guaranteed that the callback function will not be executed before
 * the specific duration is elapsed, but it is not guaranteed that it
 * will be executed at the first moment that the duration is elapsed.
 */

#ifndef DMN_TIMER_HPP_

#define DMN_TIMER_HPP_

#include <chrono>
#include <functional>
#include <thread>

#include "dmn-proc.hpp"

namespace dmn {

template <typename T> class Timer : public Proc {
public:
  Timer(const T &reltime, std::function<void()> fn);
  virtual ~Timer() noexcept;

  Timer(const Timer &obj) = delete;
  const Timer &operator=(const Timer &obj) = delete;
  Timer(Timer &&obj) = delete;
  Timer &operator=(Timer &&obj) = delete;

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
}; // class Timer

template <typename T>
Timer<T>::Timer(const T &reltime, std::function<void()> fn)
    : Proc{"timer"}, m_fn{fn}, m_reltime{reltime} {
  this->start(this->m_reltime, this->m_fn);
}

template <typename T> Timer<T>::~Timer() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T>
void Timer<T>::start(const T &reltime, std::function<void()> fn) {
  m_reltime = reltime;

  if (fn) {
    m_fn = fn;
  }

  this->stop();

  this->exec([this]() {
    while (true) {
      std::this_thread::sleep_for(this->m_reltime);
      Proc::yield();

      this->m_fn();
    }
  });
}

template <typename T> void Timer<T>::stop() {
  try {
    this->stopExec();
  } catch (...) {
  }
}

} // namespace dmn

#endif // DMN_TIMER_HPP_
