/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-timer.hpp
 * @brief A recurring timer driven by Dmn_Pipe's scheduled-write worker.
 *
 * Dmn_Timer<T> invokes a client callback after each strictly positive
 * steady-clock interval. The next tick is scheduled relative to completion of
 * the previous callback. stop() pauses without joining; resume() schedules a
 * fresh tick. Generation tokens make ticks queued before a pause or restart
 * stale, even if they wake after the timer has resumed. Standard callback
 * exceptions are logged and recurrence continues; non-standard callback
 * exceptions and asynchronous rescheduling failures pause the timer and can be
 * observed through rethrowFailure(). Timer state is mutex-protected, but
 * destruction must not race public operations or run from the timer callback.
 */

#ifndef DMN_TIMER_HPP_

#define DMN_TIMER_HPP_

#include "dmn-debug.hpp"
#include "dmn-fault-injection.hpp"
#include "dmn-pipe.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace dmn {

template <typename T> class Dmn_Timer {
  using Clock = std::chrono::steady_clock;

  struct Generation {};

  struct Tick {
    std::shared_ptr<const Generation> generation;
  };

  using Pipe = Dmn_Pipe<Tick>;

public:
  /**
   * @brief Construct and immediately start a recurring timer.
   *
   * @param reltime Strictly positive interval between callback invocations.
   * @param fn Callback invoked after each interval.
   * @throws std::invalid_argument if @p reltime is not strictly positive.
   * @throws std::overflow_error if @p reltime or its first deadline cannot be
   * represented by the steady clock.
   * @throws std::runtime_error if the scheduled pipe worker cannot be started.
   */
  Dmn_Timer(const T &reltime, std::function<void()> fn)
      : m_fn{std::move(fn)}, m_interval{toClockDuration(reltime)} {
    m_pipe = std::make_unique<Pipe>(
        "timer", [this](Tick &&tick) { processTick(std::move(tick)); }, 1, 0,
        true);

    resume();
  }

  /**
   * @brief Destroy the timer and join its scheduled-pipe worker.
   *
   * Pending stale ticks remain in the pipe until their deadlines, so
   * destruction can wait until the latest queued deadline. Destruction must
   * not race public operations or be called from this timer's callback.
   */
  ~Dmn_Timer() noexcept {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_active = false;
    }

    m_pipe.reset();
  }

  Dmn_Timer(const Dmn_Timer &obj) = delete;
  Dmn_Timer &operator=(const Dmn_Timer &obj) = delete;
  Dmn_Timer(Dmn_Timer &&obj) = delete;
  Dmn_Timer &operator=(Dmn_Timer &&obj) = delete;

  /**
   * @brief Start or restart the timer with a new interval and optional
   * callback.
   *
   * The interval is validated before any current timer state is changed. A
   * successful call invalidates old ticks and schedules the first tick of a
   * new generation. If @p fn is empty, the existing callback is retained.
   *
   * @param reltime Strictly positive interval between callback invocations.
   * @param fn Optional replacement callback.
   * @throws std::invalid_argument if @p reltime is not strictly positive.
   * @throws std::overflow_error if @p reltime or its next deadline cannot be
   * represented by the steady clock.
   * @throws std::runtime_error if the timer pipe has been shut down.
   */
  void start(const T &reltime, std::function<void()> fn = {}) {
    const auto interval = toClockDuration(reltime);
    auto generation = std::make_shared<Generation>();

    std::lock_guard<std::mutex> lock(m_mutex);
    const auto deadline = nextDeadline(interval);

    m_pipe->writeAt(deadline, Tick{generation});

    if (fn) {
      m_fn = std::move(fn);
    }

    m_generation = std::move(generation);
    m_failure = {};
    m_active = true;
    m_interval = interval;
  }

  /**
   * @brief Pause the timer without waiting for a pending tick or callback.
   *
   * A callback already admitted by the wrapper can finish after this call
   * returns. Pending ticks are invalidated and will be discarded when they
   * become due. Calling stop() on an already-paused timer is a no-op.
   */
  void stop() {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_active = false;
  }

  /**
   * @brief Resume a paused timer, scheduling its first tick after one interval.
   *
   * Calling resume() while active is a no-op. If the previous worker-side
   * scheduling or callback operation failed, that exception is rethrown and
   * the timer remains paused; call start() to replace/restart the timer.
   *
   * @throws std::exception if the timer has a stored asynchronous failure.
   * @throws std::overflow_error if the next deadline cannot be represented.
   */
  void resume() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active) {
      return;
    }

    rethrowFailureLocked();

    auto generation = std::make_shared<Generation>();
    const auto deadline = nextDeadline(m_interval);
    m_pipe->writeAt(deadline, Tick{generation});

    m_generation = std::move(generation);
    m_active = true;
  }

  /**
   * @brief Rethrow an asynchronous callback or tick-rescheduling failure.
   *
   * Standard client callback exceptions are logged and the timer continues.
   * Non-standard callback exceptions and failures while scheduling a later
   * tick pause the timer and are stored for this method to rethrow.
   *
   * @throws The exception captured from a callback or asynchronous tick
   * scheduling.
   */
  void rethrowFailure() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    rethrowFailureLocked();
  }

private:
  static auto toClockDuration(const T &reltime) -> Clock::duration {
    if (!(reltime > T::zero())) {
      throw std::invalid_argument("Dmn_Timer interval must be positive");
    }

    using Rep = Clock::duration::rep;
    static_assert(std::is_integral_v<Rep>,
                  "steady_clock duration representation must be integral");

    const std::chrono::duration<long double, Clock::period> clock_ticks{
        reltime};
    const long double rounded_ticks = std::ceil(clock_ticks.count());
    static_assert(std::numeric_limits<Rep>::radix == 2,
                  "steady_clock duration representation must be binary");
    const auto max_exponent = std::numeric_limits<long double>::max_exponent;
    const long double exclusive_max =
        std::numeric_limits<Rep>::digits < max_exponent
            ? std::ldexp(1.0L, std::numeric_limits<Rep>::digits)
            : std::numeric_limits<long double>::infinity();

    if (!std::isfinite(rounded_ticks) || rounded_ticks >= exclusive_max) {
      throw std::overflow_error(
          "Dmn_Timer interval exceeds steady_clock range");
    }

    return Clock::duration{static_cast<Rep>(rounded_ticks)};
  }

  static auto nextDeadline(Clock::duration interval) -> Clock::time_point {
    using Rep = Clock::duration::rep;
    const auto now = Clock::now();
    const auto now_ticks = now.time_since_epoch().count();
    const auto interval_ticks = interval.count();

    if (now_ticks > 0 &&
        interval_ticks > std::numeric_limits<Rep>::max() - now_ticks) {
      throw std::overflow_error(
          "Dmn_Timer deadline exceeds steady_clock range");
    }

    return Clock::time_point{Clock::duration{now_ticks + interval_ticks}};
  }

  void processTick(Tick tick) noexcept {
    std::function<void()> callback{};

    try {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_active || tick.generation != m_generation) {
          return;
        }

        callback = m_fn;
      }

      try {
        if (callback) {
          callback();
        }
      } catch (const std::exception &error) {
        DMN_DEBUG_PRINT(std::cerr << error.what() << "\n");
      } catch (...) {
        storeFailure(tick.generation, std::current_exception());

        return;
      }

      std::lock_guard<std::mutex> lock(m_mutex);
      if (!m_active || tick.generation != m_generation) {
        return;
      }

      const auto deadline = nextDeadline(m_interval);
      try {
        // Keep this injection point on recurring writes, not initial
        // scheduling.
        if (DMN_FI_TIMER_RESCHEDULE_WRITE_AT()) {
          throw std::runtime_error("injected timer tick rescheduling failure");
        }

        m_pipe->writeAt(deadline, Tick{tick.generation});
      } catch (...) {
        storeFailureLocked(tick.generation, std::current_exception());
      }
    } catch (...) {
      storeFailure(tick.generation, std::current_exception());
    }
  }

  void storeFailure(const std::shared_ptr<const Generation> &generation,
                    std::exception_ptr failure) noexcept {
    try {
      std::lock_guard<std::mutex> lock(m_mutex);

      storeFailureLocked(generation, std::move(failure));
    } catch (...) {
    }
  }

  /**
   * @brief Store a failure while the timer state mutex is already held.
   *
   * Stale generations are ignored so failures from invalidated ticks cannot
   * affect a newer timer run.
   */
  void storeFailureLocked(const std::shared_ptr<const Generation> &generation,
                          std::exception_ptr failure) noexcept {
    if (m_active && generation == m_generation) {
      m_active = false;
      m_failure = std::move(failure);
    }
  }

  void rethrowFailureLocked() const {
    if (m_failure) {
      std::rethrow_exception(m_failure);
    }
  }

  mutable std::mutex m_mutex{};
  std::function<void()> m_fn{};
  Clock::duration m_interval{};
  std::shared_ptr<const Generation> m_generation{};
  bool m_active{};
  std::exception_ptr m_failure{};
  std::unique_ptr<Pipe> m_pipe{};
}; // class Dmn_Timer

} // namespace dmn

#endif // DMN_TIMER_HPP_
