/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.cpp
 * @brief Implementation of Dmn_Runtime_Manager — centralized signal
 *        handling and asynchronous job scheduling.
 *
 * Key implementation areas:
 * - Constructor: registers default SIGTERM/SIGINT handlers that call
 *   exitMainLoop().
 * - addJob() / addHighJob() / addMediumJob() / addLowJob(): enqueue
 *   Dmn_Runtime_Job objects into the appropriate priority buffer.
 *   Each method spins on its own atomic flag to prevent jobs from
 *   being submitted before enterMainLoop() enables the queue.
 * - execRuntimeJobInternal(): dequeues and executes one job per
 *   priority level in order (high → medium → low), then schedules
 *   itself again after a 1 µs delay to create a yield point.
 * - execRuntimeJobInContext(): executes a single coroutine-based job,
 *   resuming it until it either completes or suspends, interleaving
 *   lower-priority jobs between suspension points.
 * - enterMainLoop(): enables all priority queues, installs a SIGALRM
 *   handler and a POSIX/ITIMER timer to fire every 50 µs for timed
 *   job dispatch, starts the signal-wait thread which calls sigwait()
 *   and dispatches to registered handlers via async context, then
 *   blocks until exitMainLoop() is called.
 * - runPriorToCreateInstance(): masks SIGALRM, SIGINT, SIGTERM,
 *   SIGQUIT and SIGHUP before any threads are created so that all
 *   descendant threads inherit the same signal mask and signals are
 *   delivered only to the dedicated signal-wait thread.
 */

#include "dmn-runtime.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/time.h>
#include <system_error>

#include "dmn-async.hpp"
#include "dmn-blockingqueue.hpp"
#include "dmn-proc.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

sigset_t Dmn_Runtime_Manager::s_mask{};

// Platform specific implementation
struct Dmn_Runtime_Manager_Impl {
#ifdef _POSIX_TIMERS
  timer_t m_timerid{};
  bool m_timer_created{};
#else
  bool m_timer_created{};
#endif
};

Dmn_Runtime_Manager::Dmn_Runtime_Manager()
    : Dmn_Singleton{}, Dmn_Async{"Dmn_Runtime_Manager"},
      m_mask{Dmn_Runtime_Manager::s_mask} {
  // default and to be overridden if needed, these signal handler hooks will
  // be executed when main thread calls enterMainLoop()
  m_signal_handlers[SIGTERM] = [this]([[maybe_unused]] int signo) -> void {
    this->exitMainLoop();
  };

  m_signal_handlers[SIGINT] = [this]([[maybe_unused]] int signo) -> void {
    this->exitMainLoop();
  };

  m_signal_handlers[SIGALRM] = [this]([[maybe_unused]] int signo) -> void {
    if (m_timedQueue.empty()) {
      return;
    }

    auto job = m_timedQueue.top();

    struct timespec timesp{};
    if (clock_gettime(CLOCK_MONOTONIC, &timesp) == 0) {
      const long long microseconds =
          (static_cast<long long>(timesp.tv_sec) * 1000000LL) +
          (timesp.tv_nsec / 1000);

      if (microseconds >= job.m_runtimeSinceEpoch) {
        m_timedQueue.pop();

        this->addJob(job.m_job, job.m_priority);
      }
    }
  };
}

Dmn_Runtime_Manager::~Dmn_Runtime_Manager() noexcept try {
  exitMainLoop();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

/**
 * @brief The method will add a priority asynchronous job to be run in runtime
 *        context.
 *
 * @param job The asynchronous job
 * @param priority The priority of the asynchronous job
 */
void Dmn_Runtime_Manager::addJob(Dmn_Runtime_Job::FncType job,
                                 Dmn_Runtime_Job::Priority priority) {
  switch (priority) {
  case Dmn_Runtime_Job::kHigh:
    this->addHighJob(std::move(job));
    break;

  case Dmn_Runtime_Job::kMedium:
    this->addMediumJob(std::move(job));
    break;

  case Dmn_Runtime_Job::kLow:
    this->addLowJob(std::move(job));
    break;

  default:
    assert("Unsupported priority type" == nullptr);
    break;
  }
}

/**
 * @brief The method will add high priority asynchronous job.
 *
 * @param job The high priority asynchronous job
 */
void Dmn_Runtime_Manager::addHighJob(Dmn_Runtime_Job::FncType &&job) {
  while (!m_enter_high_atomic_flag.test()) { // NOLINT(altera-unroll-loops)
    m_enter_high_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{.m_priority = Dmn_Runtime_Job::kHigh,
                       .m_job = std::move(job)};
  m_highQueue.push(std::move(rjob));
}

/**
 * @brief The method will add low priority asynchronous job.
 *
 * @param job The low priority asynchronous job
 */
void Dmn_Runtime_Manager::addLowJob(Dmn_Runtime_Job::FncType &&job) {
  while (!m_enter_low_atomic_flag.test()) {
    m_enter_low_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{.m_priority = Dmn_Runtime_Job::kLow,
                       .m_job = std::move(job)};
  m_lowQueue.push(std::move(rjob));
}

/**
 * @brief The method will add medium priority asynchronous job.
 *
 * @param job The medium priority asynchronous job
 */
void Dmn_Runtime_Manager::addMediumJob(Dmn_Runtime_Job::FncType &&job) {
  while (!m_enter_medium_atomic_flag.test(std::memory_order_relaxed)) {
    m_enter_medium_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{.m_priority = Dmn_Runtime_Job::kMedium,
                       .m_job = std::move(job)};
  m_mediumQueue.push(std::move(rjob));
}

/**
 * @brief The method will add an asynchronous task to run the job.
 *
 * @param duration The duration after that to run execRuntimeJobInternal in
 *                 interval
 */
template <class Rep, class Period>
void Dmn_Runtime_Manager::execRuntimeJobForInterval(
    const std::chrono::duration<Rep, Period> &duration) {
  if (!m_main_exit_atomic_flag.test(std::memory_order_relaxed)) {
    m_async_job_wait = this->addExecTaskAfterWithWait(
        duration, [this]() -> void { this->execRuntimeJobInternal(); });
  }
}

/**
 * @brief The method will execute the job in runtime' co-routine context.
 *
 * @param job The job to be run in the runtime context
 */
void Dmn_Runtime_Manager::execRuntimeJobInContext(Dmn_Runtime_Job &&job) {
  auto priority = job.m_priority;

  this->m_sched_stack.push(std::move(job));

  const Dmn_Runtime_Job &runningJob = this->m_sched_stack.top();
  auto task = runningJob.m_job(runningJob);

  do {
    task.handle.resume();

    if (task.handle.done()) {
      break;
    } else {
      assert(!this->m_sched_stack.empty());

      switch (priority) {
      case Dmn_Runtime_Job::kHigh:
        // skip!
        break;

      case Dmn_Runtime_Job::kMedium:
      case Dmn_Runtime_Job::kLow:
      default:
        this->execRuntimeJobInternal();
        break;
      }
    }
  } while (true);

  this->m_sched_stack.pop();
}

/**
 * @brief The method will execute the job continously.
 */
void Dmn_Runtime_Manager::execRuntimeJobInternal() {
  // This place allows us to implement stagnant avoidance logic,
  // one potential example is that:
  // - if there is a high priority and medium job, we execute
  //   the high priority job
  // - but we then alevate the medium job to a buffer between hgh and medium
  // - if in next iteration, we have no high priority job, we execute the
  //   elevate medium job before medium or low priority jobs.
  // - if there is still high priority job, we add the elevated medium job into
  //   end of high priority queue.

  Dmn_Runtime_Job::Priority state{}; // not high, not medium, not low

  if (!this->m_sched_stack.empty()) {
    state = this->m_sched_stack.top().m_priority;
  }

  if (state != Dmn_Runtime_Job::kHigh) {
    auto item = m_highQueue.popNoWait();
    if (item) {
      this->execRuntimeJobInContext(std::move(*item));
    } else if (state != Dmn_Runtime_Job::kMedium) {
      item = m_mediumQueue.popNoWait();

      if (item) {
        this->execRuntimeJobInContext(std::move(*item));
      } else if (state != Dmn_Runtime_Job::kLow) {

        item = m_lowQueue.popNoWait();
        if (item) {
          this->execRuntimeJobInContext(std::move(*item));
        }
      }
    }
  }

  if (this->m_sched_stack.empty()) {
    this->execRuntimeJobForInterval(std::chrono::microseconds(1));
  }
}

/**
 * @brief The method will exit the Dmn_Runtime_Manager mainloop, returns control
 *        (usually the mainthread) to the main() function to be continued.
 */
void Dmn_Runtime_Manager::exitMainLoop() {
  if (!m_main_exit_atomic_flag.test_and_set(std::memory_order_release)) {
    m_main_enter_atomic_flag.clear(std::memory_order_relaxed);
    m_main_exit_atomic_flag.notify_all();

    if (m_signalWaitProc) {
      m_signalWaitProc->wait();
      m_signalWaitProc = {};
    }
  }

  if (m_pimpl) {
#ifdef _POSIX_TIMERS
    struct itimerspec stop_its{};

    // Apply the change to your specific timerid
    timer_settime(m_pimpl->m_timerid, 0, &stop_its, nullptr);

    timer_delete(m_pimpl->m_timerid);
#else /* _POSIX_TIMERS */
    struct itimerval timer{};

    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;

    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;

    setitimer(ITIMER_REAL, &timer, nullptr);
#endif

    m_pimpl->m_timer_created = false;

    m_pimpl = {};
  }
}

/**
 * @brief The method will enter the Dmn_Runtime_Manager mainloop, and wait
 *        for runtime loop to be exited. this is usually called by the main()
 *        method.
 */
void Dmn_Runtime_Manager::enterMainLoop() {
  if (m_main_enter_atomic_flag.test_and_set(std::memory_order_acquire)) {
    assert("Error: enter main loop twice without exit" == nullptr);

    throw std::runtime_error("Error: enter main loop twice without exit");
  }

  m_main_exit_atomic_flag.clear(std::memory_order_relaxed);

  // up to this point, all async jobs are paused.
  this->execRuntimeJobForInterval(std::chrono::microseconds(1));

  m_enter_high_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_enter_high_atomic_flag.notify_all();
  Dmn_Proc::yield();

  m_enter_medium_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_enter_medium_atomic_flag.notify_all();
  Dmn_Proc::yield();

  m_enter_low_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_enter_low_atomic_flag.notify_all();
  Dmn_Proc::yield();

  m_pimpl = std::make_unique<Dmn_Runtime_Manager_Impl>();

#ifdef _POSIX_TIMERS
  struct sigevent sev{};
  struct itimerspec its{};

  // 1. Setup signal delivery
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGALRM;
  int err = timer_create(CLOCK_MONOTONIC, &sev, &(m_pimpl->m_timerid));
  if (-1 == err) {
    throw std::runtime_error("Error in timer_create: " +
                             std::system_category().message(err));
  }

  // 2. Set for 500 microseconds (0.5ms)
  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = 50000;    // 500,00 ns = 50 us
  its.it_interval.tv_sec = 0;      // Periodic repeat
  its.it_interval.tv_nsec = 50000; // Periodic repeat

  err = timer_settime(m_pimpl->m_timerid, 0, &its, nullptr);
  if (-1 == err) {
    throw std::runtime_error("Error in timer_settime: " +
                             std::system_category().message(err));
  }
#else /* _POSIX_TIMERS */
  struct itimerval timer{};

  // Set initial delay to 50 microseconds
  timer.it_value.tv_sec = 0;
  timer.it_value.tv_usec = 50;

  // Set repeat interval to 50 microseconds
  timer.it_interval.tv_sec = 0;
  timer.it_interval.tv_usec = 50;

  int err = setitimer(ITIMER_REAL, &timer, nullptr);
  if (-1 == err) {
    throw std::runtime_error("Error in setitimer: " +
                             std::system_category().message(err));
  }
#endif

  m_pimpl->m_timer_created = true;

  m_signalWaitProc = std::make_unique<Dmn_Proc>(
      "DmnRuntimeManager_SignalWait", [this]() -> void {
        while (true) {
          int signo{};
          int err{};

          err = sigwait(&m_mask, &signo);
          if (err) {
            throw std::runtime_error("Error in sigwait: " +
                                     std::system_category().message(err));
          }

          if (m_main_exit_atomic_flag.test(std::memory_order_relaxed)) {
            break;
          } else {
            DMN_ASYNC_CALL_WITH_CAPTURE(
                { this->execSignalHandlerInternal(signo); }, this, signo);
          }
        }
      });

  if (!m_signalWaitProc->exec()) {
    throw std::runtime_error(
        "Failed to start DmnRuntimeManager_SignalWait task");
  }

  while (!m_main_exit_atomic_flag.test(std::memory_order_relaxed)) {
    m_main_exit_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  if (m_async_job_wait) {
    m_async_job_wait->wait();
    m_async_job_wait = nullptr;
  }
}

/**
 * @brief The method executes the signal handlers in asynchronous thread context
 *
 * @param signo signal number that is raised
 */
void Dmn_Runtime_Manager::execSignalHandlerInternal(int signo) {
  auto extHandlers = m_ext_signal_handlers.find(signo);
  if (m_ext_signal_handlers.end() != extHandlers) {
    for (auto &handler : extHandlers->second) {
      handler(signo);
    }
  }

  auto handler = m_signal_handlers.find(signo);
  if (m_signal_handlers.end() != handler) {
    handler->second(signo);
  }
}

/**
 * @brief The method registers signal handler for the signal number. Note that
 *        SIGKILL and SIGSTOP can NOT be handled.
 *
 * @param signo   The POSIX signal number
 * @param handler The signal handler to be called when the signal is raised.
 */
void Dmn_Runtime_Manager::registerSignalHandler(int signo,
                                                const SignalHandler &handler) {
  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->registerSignalHandlerInternal(signo, handler); }, this, signo,
      handler);
}

/**
 * @brief The method registers external signal handler for the signal number.
 *        The external signal handlers are executed before default handler from
 *        Dmn_Runtime_Manager. Note that SIGKILL and SIGSTOP can NOT be handled.
 *
 *        This is private method to be called in the Dmn_Runtime_Manager
 *        instance asynchronous thread context.
 *
 * @param signo   The POSIX signal number
 * @param handler The signal handler to be called when the signal is raised.
 */
void Dmn_Runtime_Manager::registerSignalHandlerInternal(
    int signo, const SignalHandler &handler) {
  auto &extHandlers = m_ext_signal_handlers[signo];
  extHandlers.push_back(handler);
}

void Dmn_Runtime_Manager::runPriorToCreateInstance() {
  // Mask signals before creating any threads so they are blocked
  // in all subsequently created threads (avoids races).
  sigset_t old_mask{};
  int err{};

  sigemptyset(&Dmn_Runtime_Manager::s_mask);
  sigaddset(&Dmn_Runtime_Manager::s_mask, SIGALRM);
  sigaddset(&Dmn_Runtime_Manager::s_mask, SIGINT);
  sigaddset(&Dmn_Runtime_Manager::s_mask, SIGTERM);
  sigaddset(&Dmn_Runtime_Manager::s_mask, SIGQUIT);
  sigaddset(&Dmn_Runtime_Manager::s_mask, SIGHUP);

  err = pthread_sigmask(SIG_BLOCK, &Dmn_Runtime_Manager::s_mask, &old_mask);
  if (0 != err) {
    throw std::runtime_error("Error in pthread_sigmask: " +
                             std::string{strerror(err)});
  }
}

} // namespace dmn
