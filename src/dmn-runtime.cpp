/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.cpp
 * @brief Dmn_Runtime_Manager_Impl of Dmn_Runtime_Manager.
 */

#include "dmn-runtime.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/time.h>
#include <system_error>

namespace dmn {

namespace detail {

// Platform specific implementation
struct Dmn_Runtime_Manager_Impl {
#ifdef _POSIX_TIMERS
  timer_t m_timerid{};
  bool m_timer_created{};
#else
  bool m_timer_created{};
#endif
};

/**
 * @brief Create the Dmn_Runtime_Manager_Impl object and its timer details
 *        depending on the compile feature set available, and initialize the
 *        timer to be 0 (not runnable). Ownership of the returned object is
 *        transferred to the caller, who is responsible for calling
 *        Dmn_Runtime_Manager_Impl_destroy to free the object.
 *
 * @return The newly created and 0 initialized timer.
 */
auto Dmn_Runtime_Manager_Impl_create() -> Dmn_Runtime_Manager_Impl * {
  auto *impl = new Dmn_Runtime_Manager_Impl;

#ifdef _POSIX_TIMERS
  struct sigevent sev{};

  // 1. Setup signal delivery
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGALRM;
  int err = timer_create(CLOCK_MONOTONIC, &sev, &(impl->m_timerid));
  if (-1 == err) {
    delete impl;

    throw std::runtime_error("Error in timer_create: " +
                             std::system_category().message(errno));
  }

  impl->m_timer_created = true;
#else /* _POSIX_TIMERS */
  impl->m_timer_created = true;
#endif

  try {
    Dmn_Runtime_Manager_Impl_setNextTimer(impl, 0, 0);
  } catch (...) {
    delete impl;
    impl = nullptr;

    throw;
  }

  return impl;
}

/**
 * @brief Destroy the implementation's timer details and freeing the object.
 *
 * @param impl_ptr The pointer to the implementation detail object.
 */
void Dmn_Runtime_Manager_Impl_destroy(
    Dmn_Runtime_Manager_Impl **impl_ptr) noexcept {
  assert(impl_ptr);
  assert(*impl_ptr);

  try {
    Dmn_Runtime_Manager_Impl_setNextTimer(*impl_ptr, 0, 0);
  } catch (...) {
    // catch and ignore the exception as runtime is shutting down.
    // catch, so we still proceed to free the object
  }

#ifdef _POSIX_TIMERS
  if ((*impl_ptr)->m_timer_created) {
    // Ignore errors in destructor; cannot throw from noexcept destructor.
    (void)timer_delete((*impl_ptr)->m_timerid);
  }
#endif

  (*impl_ptr)->m_timer_created = false;

  delete *impl_ptr;
  (*impl_ptr) = nullptr;
}

/**
 * @brief Set the next timer to be fired in future time point.
 *
 * @param impl The implementation detail object that contains timer.
 * @param tp The future timepoint that timer is triggered.
 */
void Dmn_Runtime_Manager_Impl_setNextTimerAt(Dmn_Runtime_Manager_Impl *impl,
                                             TimePoint tp) {
  if (!impl) {
    return;
  }

  TimePoint tpNow = std::chrono::steady_clock::now();

  // 1. Subtract to get the total duration
  auto diff = tp - tpNow;

  if (diff <= std::chrono::nanoseconds::zero()) {
    Dmn_Runtime_Manager_Impl_setNextTimer(
        impl, 0, 10000); // run it in next 10 microseconds
  } else {
    // 2. Extract the whole seconds
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(diff);

    // 3. Extract the remaining nanoseconds (the "remainder")
    auto nsecs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(diff - secs);

    Dmn_Runtime_Manager_Impl_setNextTimer(impl, secs.count(), nsecs.count());
  }
}

/**
 * @brief Set the next timer to be fired after next seconds and nanoseconds.
 *
 * @param impl The implementation detail object that contains timer.
 * @param sec Seconds after that timer is triggered.
 * @param nsec Nanoseconds after that timer is triggered.
 */
void Dmn_Runtime_Manager_Impl_setNextTimer(Dmn_Runtime_Manager_Impl *impl,
                                           SecInt sec, NSecInt nsec) {
  assert(impl);
  assert(impl->m_timer_created);

#ifdef _POSIX_TIMERS
  struct itimerspec its{};

  its.it_value.tv_sec = sec;
  its.it_value.tv_nsec = nsec;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;

  int err = timer_settime(impl->m_timerid, 0, &its, nullptr);
  if (-1 == err) {
    throw std::runtime_error("Error in timer_settime: " +
                             std::system_category().message(errno));
  }
#else /* _POSIX_TIMERS */
  struct itimerval timer{};

  timer.it_value.tv_sec = sec;
  timer.it_value.tv_usec = nsec / 1000;

  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 0;

  int err = setitimer(ITIMER_REAL, &timer, nullptr);
  if (-1 == err) {
    throw std::runtime_error("Error in setitimer: " +
                             std::system_category().message(errno));
  }
#endif
}

} // namespace detail

} // namespace dmn
