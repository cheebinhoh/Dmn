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
 * - addRuntimeJobToCoroutineSchedulerContext(): adds runtime job and its
 *   coroutine task to the scheduler context, so that it will be picked up and
 *   executed by the coroutine scheduler.
 * - runRuntimeCoroutineScheduler(): retrieves the scheduled job (and
 *   corresponding coroutine task) and executes it in the singleton asynchronous
 *   thread context.
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
#include <thread>

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
  this->addExecTask([this]() { m_asyncThreadId = std::this_thread::get_id(); });

  m_pimpl = std::make_unique<Dmn_Runtime_Manager_Impl>();

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
  this->setNextTimer(0, 0);
#else /* _POSIX_TIMERS */
  m_pimpl->m_timer_created = true;

  this->setNextTimer(0, 0);
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
    assert(isRunInAsyncThread());

    if (m_timedQueue.empty()) {
      return;
    }

    const TimePoint &tp = std::chrono::steady_clock::now();
    while (!m_timedQueue.empty()) {
      const auto &job = m_timedQueue.top();
      if (tp >= job.m_due) {
        this->addJob(job.m_fnc, job.m_priority, job.m_onErrorFnc);

        m_timedQueue.pop();
      } else {
        this->setNextTimerSinceEpoch(job.m_due);

        break;
      }
    }
  };
}

Dmn_Runtime_Manager::~Dmn_Runtime_Manager() noexcept try {
  exitMainLoop();

  if (m_pimpl) {
#ifdef _POSIX_TIMERS
    if (m_pimpl->m_timer_created) {
      // Ignore errors in destructor; cannot throw from noexcept destructor.
      (void)timer_delete(m_pimpl->m_timerid);
    }
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
 * @param fnc The asynchronous job function to be executed
 * @param priority The priority of the asynchronous job
 * @param onErrorFnc The callback to be invoked if executing the job results
 *        in an error
 */
void Dmn_Runtime_Manager::addJob(Dmn_Runtime_Job::FncType fnc,
                                 Dmn_Runtime_Job::Priority priority,
                                 Dmn_Runtime_Job::OnErrorFncType onErrorFnc) {
  Dmn_Runtime_Job rjob{.m_priority = priority,
                       .m_fnc = std::move(fnc),
                       .m_onErrorFnc = std::move(onErrorFnc)};

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

  if (m_jobs_count.fetch_add(1) == 0 &&
      m_main_enter_atomic_flag.test(std::memory_order_relaxed)) {
    this->runRuntimeJobExecutor();
  }
}

void Dmn_Runtime_Manager::clearSignalHandlerHook(int signo) {
  this->addExecTask(
      [this, signo]() mutable { this->clearSignalHandlerHookInternal(signo); });
}

void Dmn_Runtime_Manager::clearSignalHandlerHookInternal(int signo) {
  assert(isRunInAsyncThread());

  auto &extHandlerHooks = m_signal_handler_hooks_external[signo];
  extHandlerHooks.clear();
}

/**
 * @brief The method will execute the job in runtime coroutine context.
 *
 * @param job The job to be run in the runtime context
 */
void Dmn_Runtime_Manager::addRuntimeJobToCoroutineSchedulerContext(
    Dmn_Runtime_Job &&job) {
  assert(isRunInAsyncThread());
  assert(this->m_sched_job.size() == this->m_sched_task.size());

  try {
    auto task = job.m_fnc(job);
    this->m_sched_task.push(std::move(task));
    this->m_sched_job.push(std::move(job));
  } catch (...) {
    if (job.m_onErrorFnc) {
      std::exception_ptr ep = std::current_exception();
      job.m_onErrorFnc(ep);
    }
  }

  assert(this->m_sched_job.size() == this->m_sched_task.size());
}

/**
 * @brief The method will schedule job and dispatch it as a coroutine task.
 */
void Dmn_Runtime_Manager::execRuntimeJobInternal() {
  Dmn_Runtime_Job::Priority state{}; // not high, not medium, not low

  assert(isRunInAsyncThread());

  if (!this->m_sched_job.empty()) {
    state = this->m_sched_job.top().m_priority;
  }

  if (state != Dmn_Runtime_Job::Priority::kHigh) {
    auto item = m_highQueue.popNoWait();
    if (item) {
      this->addRuntimeJobToCoroutineSchedulerContext(std::move(*item));
    } else if (state != Dmn_Runtime_Job::Priority::kMedium) {
      item = m_mediumQueue.popNoWait();

      if (item) {
        this->addRuntimeJobToCoroutineSchedulerContext(std::move(*item));
      } else if (state != Dmn_Runtime_Job::Priority::kLow) {

        item = m_lowQueue.popNoWait();
        if (item) {
          this->addRuntimeJobToCoroutineSchedulerContext(std::move(*item));
        }
      }
    }
  }

  assert(this->m_sched_job.size() == this->m_sched_task.size());

  size_t jobsCountInScheduler = this->m_sched_job.size();

  if (jobsCountInScheduler > 0) {
    bool done = this->runRuntimeCoroutineScheduler();

    if (!done) {
      assert(!this->m_sched_job.empty());
      assert(!this->m_sched_task.empty());
      assert(this->m_sched_job.size() == jobsCountInScheduler);

      this->runRuntimeJobExecutor();
    } else if (this->m_sched_job.size() < jobsCountInScheduler &&
               this->m_jobs_count.fetch_sub(1) > 1) {
      this->runRuntimeJobExecutor();
    } else {
      assert(this->m_sched_job.empty());
      assert(this->m_sched_task.empty());
      assert(0 == m_jobs_count.load(std::memory_order_acquire));
    }
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
  assert(isRunInAsyncThread());

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

/**
 * @brief The method returns true or false if it is called within the runtime
 * singleton asychronous thread context.
 *
 * @return True or false
 */
auto Dmn_Runtime_Manager::isRunInAsyncThread() -> bool {
  return std::this_thread::get_id() == m_asyncThreadId;
}

void Dmn_Runtime_Manager::registerSignalHandlerHook(int signo,
                                                    SignalHandlerHook &&hook) {
  this->addExecTask([this, signo, hook = std::move(hook)]() mutable {
    this->registerSignalHandlerHookInternal(signo, std::move(hook));
  });
}

/**
 * @brief The method registers external signal handler hook function for the
 *        signal number. The hook functions are executed before default internal
 *        handler hook set up by the Dmn_Runtime_Manager. Note that SIGKILL and
 *        SIGSTOP can NOT be handled.
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
  assert(isRunInAsyncThread());

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

/**
 * @brief Coroutine task scheduler entry point.
 *
 *        Resumes the top scheduled task until it either completes or reaches
 *        a suspension point. Returns true when the task has finished
 *        execution and has been removed from the scheduler and false when
 *        the task has been suspended and remains scheduled to be resumed later.
 *
 * @return true if the top task has completed and been removed from the
 *         scheduler; false if the task has suspended and remains in the
 *         scheduler to be resumed later.
 */
auto Dmn_Runtime_Manager::runRuntimeCoroutineScheduler() -> bool {
  bool complete{true};

  assert(isRunInAsyncThread());
  assert(this->m_sched_task.size() > 0);
  assert(this->m_sched_job.size() > 0);
  assert(this->m_sched_task.size() == this->m_sched_job.size());

  const Dmn_Runtime_Job &runningJob = this->m_sched_job.top();

  auto &task = this->m_sched_task.top();

  try {
    if (task.isValid()) {
      // runtime jobs must complete before returning to scheduler; suspension is
      // only used for intra-job yielding and will be resumed immediately.
      do {
        task.m_handle.resume();

        if (task.m_handle.done()) {
          task.await_resume(); // rethrow exception captured by
                               // promise_type::unhandled_exception()

          this->m_sched_task.pop();
          this->m_sched_job.pop();

          break;
        } else {
          assert(!this->m_sched_job.empty());
          assert(!this->m_sched_task.empty());

          complete = false;
          break;
        }
      } while (true);
    } else {
      this->m_sched_task.pop();
      this->m_sched_job.pop();
    }
  } catch (...) {
    if (runningJob.m_onErrorFnc) {
      std::exception_ptr ep = std::current_exception();
      runningJob.m_onErrorFnc(ep);
    }

    this->m_sched_task.pop();
    this->m_sched_job.pop();
  }

  return complete;
}

/**
 * @brief The method runs the job executor in the singleton asynchronous thread
 *        context.
 */
void Dmn_Runtime_Manager::runRuntimeJobExecutor() {
  this->addExecTask([this]() -> void { this->execRuntimeJobInternal(); });
}

/**
 * @brief The method sets the next scheduled timer (SIGALRM) according to the
 *        timepoint. if the timepoint is in now or the past, SIGALRM is
 * scheduled after 10us.
 *
 * @param tp The timepoint after that the timer (SIGALRM) will be triggred.
 */
void Dmn_Runtime_Manager::setNextTimerSinceEpoch(TimePoint tp) {
  if (!m_pimpl) {
    return;
  }

  TimePoint tpNow = std::chrono::steady_clock::now();

  // 1. Subtract to get the total duration
  auto diff = tp - tpNow;

  if (diff <= std::chrono::nanoseconds::zero()) {
    this->setNextTimer(0, 10000); // run it in next 10 microseconds
  } else {
    // 2. Extract the whole seconds
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(diff);

    // 3. Extract the remaining nanoseconds (the "remainder")
    auto nsecs =
        std::chrono::duration_cast<std::chrono::nanoseconds>(diff - secs);

    this->setNextTimer(secs.count(), nsecs.count());
  }
}

/**
 * @brief The method sets the next scheduled timer (SIGALRM) after seconds +
 *        nanoseconds.
 *
 * @param sec The seconds after that to run the timer (SIGALRM)
 * @param nsec The nanoseconds after that to run the timer (SIGALRM)
 */
void Dmn_Runtime_Manager::setNextTimer(SecInt sec, NSecInt nsec) {
  assert(m_pimpl);
  assert(m_pimpl->m_timer_created);

#ifdef _POSIX_TIMERS
  struct itimerspec its{};

  its.it_value.tv_sec = sec;
  its.it_value.tv_nsec = nsec;
  its.it_interval.tv_sec = 0;
  its.it_interval.tv_nsec = 0;

  int err = timer_settime(m_pimpl->m_timerid, 0, &its, nullptr);
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

} // namespace dmn
