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
 *   Each method spins on its enter main loop atomic flag to prevent
 *   jobs from being submitted before enterMainLoop() enables the queue.
 * - runRuntimeJobExecutor(): dequeues and executes one job per
 *   priority level in order (high → medium → low), then schedules
 *   itself again if there is more job to be executed
 * - execRuntimeJobInContext(): executes a single coroutine-based job,
 *   resuming it until it either completes or suspends, interleaving
 *   lower-priority jobs between suspension points.
 * - enterMainLoop(): enables all priority queues, starts the signal-wait thread
 *   which calls sigwait() and dispatches to registered handlers via async
 *   context, then blocks until exitMainLoop() is called.
 * - runPriorToCreateInstance(): masks SIGALRM, SIGINT, SIGTERM,
 *   SIGQUIT and SIGHUP before any threads are created so that all
 *   descendant threads inherit the same signal mask and signals are
 *   delivered only to the dedicated signal-wait thread.
 * - SIGALRM timer is set to the next due TimedJob and rearm whenever change
 *   is needed.
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
#include <pthread.h>
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
  m_pimpl = std::make_unique<Dmn_Runtime_Manager_Impl>();

// set first timer (SIGALRM), so that all prior-queued timed jobs
// are queued to be processed without accessing m_timedQueue to avoid
// data race condition
#ifdef _POSIX_TIMERS
  struct sigevent sev{};

  // 1. Setup signal delivery
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo = SIGALRM;
  int err = timer_create(CLOCK_MONOTONIC, &sev, &(m_pimpl->m_timerid));
  if (-1 == err) {
    throw std::runtime_error("Error in timer_create: " +
                             std::system_category().message(errno));
  }

  m_pimpl->m_timer_created = true;
  this->setNextTimer(0, 10000);
#else /* _POSIX_TIMERS */
  m_pimpl->m_timer_created = true;

  this->setNextTimer(0, 10000);
#endif

  // default, these signal handler hooks will be executed in the singleton
  // asynchronous context right after externally registered signal handler hooks
  m_signal_handler_hooks[SIGTERM] = [this]([[maybe_unused]] int signo) -> void {
    this->exitMainLoop();
  };

  m_signal_handler_hooks[SIGINT] = [this]([[maybe_unused]] int signo) -> void {
    this->exitMainLoop();
  };

  m_signal_handler_hooks[SIGALRM] = [this]([[maybe_unused]] int signo) -> void {
    if (m_timedQueue.empty()) {
      return;
    }

    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      auto d = std::chrono::seconds{ts.tv_sec} +
               std::chrono::nanoseconds{ts.tv_nsec};

      TimePoint tp{std::chrono::duration_cast<Clock::duration>(d)};

      while (!m_timedQueue.empty()) {
        auto job = m_timedQueue.top();
        if (tp >= job.m_due) {
          m_timedQueue.pop();

          this->addJob(std::move(job.m_job), job.m_priority);
        } else {
          this->setNextTimerSinceEpoch(job.m_due);

          break;
        }
      }
    }
  };
}

Dmn_Runtime_Manager::~Dmn_Runtime_Manager() noexcept try {
  exitMainLoop();

  if (m_pimpl) {
#ifdef _POSIX_TIMERS
    timer_delete(m_pimpl->m_timerid);
#endif

    m_pimpl->m_timer_created = false;

    m_pimpl = {};
  }
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

  while (!m_main_enter_atomic_flag.test()) { // NOLINT(altera-unroll-loops)
    m_main_enter_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{.m_priority = priority, .m_job = std::move(job)};

  switch (priority) {
  case Dmn_Runtime_Job::Priority::kHigh:
    m_highQueue.push(std::move(rjob));
    break;

  case Dmn_Runtime_Job::Priority::kMedium:
    m_mediumQueue.push(std::move(rjob));
    break;

  case Dmn_Runtime_Job::Priority::kLow:
    m_lowQueue.push(std::move(rjob));
    break;

  default:
    throw std::runtime_error(
        "Error: invalid priority, only kHigh, kMedium and kLow is allowed");
  }

  if (m_jobs_count.fetch_add(1) == 0) {
    this->runRuntimeJobExecutor();
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

  if (task.isValid()) {
    // runtime jobs must complete before returning to scheduler; suspension is
    // only used for intra-job yielding and will be resumed immediately.
    do {
      task.handle.resume();

      if (task.handle.done()) {
        break;
      } else {
        assert(!this->m_sched_stack.empty());

        switch (priority) {
        case Dmn_Runtime_Job::Priority::kHigh:
          // skip!
          break;

        case Dmn_Runtime_Job::Priority::kMedium:
        case Dmn_Runtime_Job::Priority::kLow:
        default:
          this->execRuntimeJobInternal();
          break;
        }
      }
    } while (true);
  }

  this->m_sched_stack.pop();
}

/**
 * @brief The method will execute the job continously.
 */
void Dmn_Runtime_Manager::execRuntimeJobInternal() {
  bool jobIsRun{};
  Dmn_Runtime_Job::Priority state{}; // not high, not medium, not low

  if (!this->m_sched_stack.empty()) {
    state = this->m_sched_stack.top().m_priority;
  }

  if (state != Dmn_Runtime_Job::Priority::kHigh) {
    auto item = m_highQueue.popNoWait();
    if (item) {
      this->execRuntimeJobInContext(std::move(*item));
      jobIsRun = true;
    } else if (state != Dmn_Runtime_Job::Priority::kMedium) {
      item = m_mediumQueue.popNoWait();

      if (item) {
        this->execRuntimeJobInContext(std::move(*item));
        jobIsRun = true;
      } else if (state != Dmn_Runtime_Job::Priority::kLow) {

        item = m_lowQueue.popNoWait();
        if (item) {
          this->execRuntimeJobInContext(std::move(*item));
          jobIsRun = true;
        }
      }
    }
  }

  // this will make sure that:
  // - we decrement the m_jobs_count if a job is run and
  // - we call runRuntimeJobExecutor() if sched_stack is empty (it means that
  //   sched does not run any job now, and we should repost it again.
  if (jobIsRun && this->m_jobs_count.fetch_sub(1) > 1 &&
      this->m_sched_stack.empty()) {
    this->runRuntimeJobExecutor();
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

    // set the last timer, so that signalWaitProc gradefully exit.
    this->setNextTimer(0, 10000);

    // after set the main loop enter (false) and exit (true) flag, we just
    // have to wait for signalWaitProc to gradefully exit as it is triggered
    // at fixed interval before we clear the timer.
    if (m_signalWaitProc) {
      m_signalWaitProc->wait();
      m_signalWaitProc = {};
    }
  }

  if (m_pimpl) {
    this->setNextTimer(
        0, 0); // disable timer though m_signalWaitProc shall already done so
               // but if it is crashed, we still disable timer.
  }
}

/**
 * @brief The method will enter the Dmn_Runtime_Manager mainloop, and wait
 *        for runtime loop to be exited. this is usually called by the main()
 *        method.
 */
void Dmn_Runtime_Manager::enterMainLoop() {
  // If there are already pending jobs (e.g., from a prior run), ensure the
  // runtime job executor is started so the system does not remain idle.
  bool startJobExecutor = m_jobs_count.load(std::memory_order_acquire) > 0;

  if (m_main_enter_atomic_flag.test_and_set(std::memory_order_acquire)) {
    assert(false && "Error: enter main loop twice without exit");

    throw std::runtime_error("Error: enter main loop twice without exit");
  }

  m_main_exit_atomic_flag.clear(std::memory_order_relaxed);
  m_main_enter_atomic_flag.notify_all();
  Dmn_Proc::yield();

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
            setNextTimer(0, 0);

            break;
          } else {
            this->addExecTask([this, signo]() {
              this->execSignalHandlerHookInternal(signo);
            });
          }
        }
      });

  if (!m_signalWaitProc->exec()) {
    throw std::runtime_error(
        "Failed to start DmnRuntimeManager_SignalWait task");
  }

  if (startJobExecutor) {
    this->runRuntimeJobExecutor();
  }

  while (!m_main_exit_atomic_flag.test(std::memory_order_relaxed)) {
    m_main_exit_atomic_flag.wait(false, std::memory_order_relaxed);
  }
}

/**
 * @brief The method executes the signal handlers in the singleton
 *        asynchronous thread context.
 *
 * @param signo signal number that is raised
 */
void Dmn_Runtime_Manager::execSignalHandlerHookInternal(int signo) {
  auto ext_hooks = m_signal_handler_hooks_external.find(signo);
  if (m_signal_handler_hooks_external.end() != ext_hooks) {
    for (auto &fnc : ext_hooks->second) {
      fnc(signo);
    }
  }

  auto hook = m_signal_handler_hooks.find(signo);
  if (m_signal_handler_hooks.end() != hook) {
    hook->second(signo);
  }
}

void Dmn_Runtime_Manager::registerSignalHandlerHook(int signo,
                                                    SignalHandlerHook &&hook) {
  this->addExecTask([this, signo, hook = std::move(hook)]() mutable {
    this->registerSignalHandlerHookInternal(signo, std::move(hook));
  });
}

/**
 * @brief The method registers external signal handler hook function for the
 * signal number. The hook functions are executed before default internal
 * handler hook set up by the Dmn_Runtime_Manager. Note that SIGKILL and SIGSTOP
 * can NOT be handled.
 *
 *        This is a private method to be called in the Dmn_Runtime_Manager
 *        instance asynchronous thread context.
 *
 * @param signo The POSIX signal number
 * @param hook  The signal handler hook function to be called when the signal is
 *              raised.
 */
void Dmn_Runtime_Manager::registerSignalHandlerHookInternal(
    int signo, SignalHandlerHook &&hook) {
  auto &extHandlerHooks = m_signal_handler_hooks_external[signo];
  extHandlerHooks.push_back(std::move(hook));
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

void Dmn_Runtime_Manager::runRuntimeJobExecutor() {
  this->addExecTask([this]() -> void { this->execRuntimeJobInternal(); });
}

void Dmn_Runtime_Manager::setNextTimerSinceEpoch(TimePoint tp) {
  if (!m_pimpl) {
    return;
  }

  struct timespec ts{};

#ifdef _POSIX_TIMERS
  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
#else
  if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
#endif /* _POSIX_TIMERS */
    auto d =
        std::chrono::seconds{ts.tv_sec} + std::chrono::nanoseconds{ts.tv_nsec};

    TimePoint tpNow = TimePoint{std::chrono::duration_cast<Clock::duration>(d)};

    // 1. Subtract to get the total duration
    auto diff = tp - tpNow;

    if (diff < std::chrono::nanoseconds::zero() ||
        diff == std::chrono::nanoseconds::zero()) {

      this->setNextTimer(0, 10000); // run it in next 10 microseconds
    } else {
      // 2. Extract the whole seconds
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(diff);

      // 3. Extract the remaining nanoseconds (the "remainder")
      auto nsecs =
          std::chrono::duration_cast<std::chrono::nanoseconds>(diff - secs);

      this->setNextTimer(secs.count(), nsecs.count());
    }
  } else {
    this->setNextTimer(0, 10000); // run it in next 10 microseconds
  }
}

void Dmn_Runtime_Manager::setNextTimer(SecInt sec, NSecInt nsec) {
  assert(m_pimpl);
  assert(m_pimpl->m_timer_created);

#ifdef _POSIX_TIMERS
  struct itimerspec its{};

  its.it_value.tv_sec = sec;
  its.it_value.tv_nsec = nsec;
  its.it_interval.tv_sec = 0;  // sec;
  its.it_interval.tv_nsec = 0; // nsec;

  int err = timer_settime(m_pimpl->m_timerid, 0, &its, nullptr);
  if (-1 == err) {
    throw std::runtime_error("Error in timer_settime: " +
                             std::system_category().message(errno));
  }
#else /* _POSIX_TIMERS */
  struct itimerval timer{};

  timer.it_value.tv_sec = sec;
  timer.it_value.tv_usec = nsec / 1000;

  timer.it_interval.tv_sec = 0;  // sec;
  timer.it_interval.tv_usec = 0; // nsec / 1000;

  int err = setitimer(ITIMER_REAL, &timer, nullptr);
  if (-1 == err) {
    throw std::runtime_error("Error in setitimer: " +
                             std::system_category().message(errno));
  }
#endif
}

} // namespace dmn
