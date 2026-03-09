/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.hpp
 * @brief Runtime manager for centralized signal handling and asynchronous jobs.
 *
 * Overview
 * --------
 * Dmn_Runtime_Manager provides a single, process‑wide runtime responsible for:
 * - Managing POSIX signal masking, all worker threads inherit a consistent
 *   signal configuration and provide interface to add signal handler hook
 *   function(s) that are executed safely in a singleton asynchronous thread
 *   context.
 * - Scheduling and executing asynchronous jobs with priority levels
 *   (high/medium/low) as well as delayed (timed) jobs with priority level.
 * - All jobs are scheduled, serialized and executed in a singleton asynchronous
 *   thread context by high, medium and then low priority ordering.
 * - Executing jobs scheduled through addJob() and addTimedJob() sequentially
 *   according to priority within a C++ coroutine execution model.
 *
 * Key Responsibilities
 * --------------------
 * - **Singleton lifecycle**: The runtime inherits from Dmn_Singleton, and
 *   Signal masking is applied before any threads are spawned so all descendant
 *   threads inherit the same mask.
 * - **Signal handling**: The runtime blocks SIGALRM, SIGINT, SIGTERM, SIGQUIT,
 *   and SIGHUP during initialization. Internal and external handler hook
 *   function(s) can be registered and are invoked from a singleton asynchronous
 *   thread context rather than directly inside an async‑signal handler.
 * - **Job scheduling**:
 *   - Immediate jobs are placed into priority queues.
 *   - Timed jobs are stored in a min‑heap ordered by absolute microsecond
 *     timestamps to guarantee earliest‑first execution.
 * - **Efficient waiting**: Integrates with Dmn_Async and Dmn_Async_Wait to
 *   avoid busy‑waiting when waiting for new jobs or signals.
 *
 * Thread‑Safety & Signal‑Safety Notes
 * -----------------------------------
 * - Signals are masked in runPriorToCreateInstance() before the runtime
 *   singleton asynchronous thread context is created. Masking inside the
 *   constructor would be too late because the parent class Dmn_Async singleton
 *   asynchronous thread context may already exist without any signal mask.
 * - Singleton initialization is protected by std::call_once and a static
 *   std::once_flag to prevent race conditions in Dmn_Singleton.
 * - Signal handlers avoid performing non‑async‑signal‑safe operations inside
 *   the raw signal handler but are handled through a dedicated Dmn_Proc thread
 *   as the singleton asynchronous thread context and then the attached signal
 *   handler hook function(s) are executed in the thread context.
 *
 * Usage Summary
 * -------------
 * - Create or obtain the singleton via Dmn_Singleton::createInstance().
 * - Register signal handler hook function using
 *   registerSignalHandlerHook(signo, hook).
 * - Enqueue immediate work with addJob() or schedule delayed work with
 *   addTimedJob().
 * - Start processing job and signal handler hooks with enterMainLoop()
 *   and stop with exitMainLoop().
 * - job, timedJob and signal handler hooks are all processed in the singleton
 *   asynchronous thread context.
 *
 * Implementation Notes
 * --------------------
 * - This header defines the public API, supporting types, and inline templates.
 *   The implementation of Dmn_Runtime_Manager_Impl is provided in the
 *   corresponding source file(s).
 * - Constructor: registers default SIGTERM/SIGINT handlers that call
 *   exitMainLoop().
 * - addJob(): enqueue Dmn_Runtime_Job objects into the appropriate priority
 *   buffer, the enterMainLoop() method will start the job executor to process
 *   enqueued jobs.
 * - runRuntimeJobExecutor(): dequeues and executes one job per
 *   priority level in order (high → medium → low), then schedules
 *   itself again if there are more jobs to be executed.
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

#ifndef DMN_RUNTIME_HPP_
#define DMN_RUNTIME_HPP_

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <queue>
#include <stack>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dmn-async.hpp"
#include "dmn-blockingqueue-lf.hpp"
#include "dmn-blockingqueue.hpp"
#include "dmn-proc.hpp"
#include "dmn-singleton.hpp"

#include "dmn-runtime-task.hpp"

namespace dmn {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using SecInt = int64_t;
using NSecInt = std::chrono::nanoseconds::rep;

/**
 * Dmn_Runtime_Job
 *
 * Represents a unit of work that can be scheduled and executed by the runtime.
 *
 * Members:
 *  - m_priority: Priority level for scheduling (kSched, kHigh, kMedium, kLow).
 *  - m_fnc: Callable that performs the work. It is invoked with the job
 *           metadata so the callable can inspect runtime fields if needed.
 *  - m_due: TimePoint representing the absolute time (since boot/monotonic
 *           start) when the job should be executed.
 *  - m_onErrorFnc: Callable for exception thrown inside m_fnc Callable.
 */
struct Dmn_Runtime_Job {
  using FncType = std::function<void(const Dmn_Runtime_Job &j)>;
  using TaskFncType = std::function<Dmn_Runtime_Task(const Dmn_Runtime_Job &j)>;
  using OnErrorFncType = std::function<void(std::exception_ptr &)>;

  enum class Priority : int { kSched = 0, kHigh = 1, kMedium, kLow };

  Priority m_priority{Priority::kMedium};
  TaskFncType m_fnc{};
  TimePoint m_due{};

  OnErrorFncType m_onErrorFnc{};
};

// TimedJobComparator
// ------------------
// The runtime keeps timed jobs in a priority_queue. The comparator places the
// job with the smallest (earliest) m_due at the top of the container so it
// can be popped and executed first.
struct TimedJobComparator {
  bool operator()(const Dmn_Runtime_Job &a, const Dmn_Runtime_Job &b) const {
    return a.m_due > b.m_due;
  }
};

template <typename F, typename... Args>
concept IsStrictJobFnc = requires(F &&f, Args &&...args) {
  {
    std::invoke(std::forward<F>(f), std::forward<Args>(args)...)
  } -> std::same_as<void>;
} || requires(F &&f, Args &&...args) {
  {
    std::invoke(std::forward<F>(f), std::forward<Args>(args)...)
  } -> std::same_as<Dmn_Runtime_Task>;
};

// Simplified alias for your specific Job signature
template <typename F>
concept IsValidJobFnc = IsStrictJobFnc<F, const Dmn_Runtime_Job &>;

namespace detail {
// Platform specific implementation
struct Dmn_Runtime_Manager_Impl;

auto Dmn_Runtime_Manager_Impl_create() -> Dmn_Runtime_Manager_Impl *;

void Dmn_Runtime_Manager_Impl_destroy(Dmn_Runtime_Manager_Impl **) noexcept;

void Dmn_Runtime_Manager_Impl_setNextTimerAt(Dmn_Runtime_Manager_Impl *,
                                             TimePoint tp);

void Dmn_Runtime_Manager_Impl_setNextTimer(Dmn_Runtime_Manager_Impl *,
                                           SecInt sec, NSecInt nsec);
} // namespace detail

/**
 * Dmn_Runtime_Manager
 *
 * A singleton runtime manager that centralizes POSIX signal handling and
 * asynchronous job scheduling/execution. It inherits Dmn_Singleton for
 * singleton lifecycle and privately from Dmn_Async to run an internal
 * runtime asynchronous thread used for processing job.
 *
 * Public API highlights:
 *  - addJob(fnc, priority, onErrorFnc): enqueue a job for immediate execution
 *    in the singleton asynchronous thread context.
 *  - addTimedJob(fnc, duration, priority, onErrorFnc): schedule job to run
 *    after duration in the singleton asychronous thread context.
 *  - registerSignalHandlerHook(signo, hook): register a handler hook to be
 *    invoked when the given signal is delivered in the singleton asychronous
 *    thread context.
 *  - clearSignalHandlerHook(signo): clear all register handler hooks for
 *    the particular signo.
 *  - enterMainLoop() / exitMainLoop(): control the runtime starts processing
 *    jobs and signal handler hooks in the singleton asynchronous thread
 *    context.
 *
 * Important behaviour:
 *  - Signals used by the runtime are blocked prior to creating the singleton
 *    so that all threads inherit the same mask. This avoids race conditions
 *    where signals could be delivered to a thread that has not yet registered
 *    or initialized handler state.
 *  - Timed jobs use absolute microsecond timestamps to avoid cumulative drift
 *    from repeated short-duration sleeps.
 */
template <template <class> class QueueType = Dmn_BlockingQueue>
class Dmn_Runtime_Manager
    : public Dmn_Singleton<Dmn_Runtime_Manager<QueueType>>,
      private Dmn_Async<QueueType> {
public:
  using SignalHandlerHook = std::function<void(int signo)>;

  Dmn_Runtime_Manager();
  virtual ~Dmn_Runtime_Manager() noexcept;

  Dmn_Runtime_Manager(const Dmn_Runtime_Manager &obj) = delete;
  Dmn_Runtime_Manager &operator=(const Dmn_Runtime_Manager &obj) = delete;
  Dmn_Runtime_Manager(Dmn_Runtime_Manager &&obj) = delete;
  Dmn_Runtime_Manager &operator=(Dmn_Runtime_Manager &&obj) = delete;

  /**
   * @brief Enqueue a job for immediate execution with the given priority.
   *        The runtime will schedule the job onto the appropriate internal
   *        priority queue.
   *
   * @param fnc The callable to be executed, either Dmn_Runtime_Job::FncType or
   *            Dmn_Runtime_Job::TaskFncType.
   * @param priority The job priority.
   * @param onErrorFnc The callable for exception thrown inside fnc.
   */
  template <typename F>
    requires IsValidJobFnc<F>
  void addJob(
      F &&fnc,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
      Dmn_Runtime_Job::OnErrorFncType onErrorFnc = {});

  /**
   * @brief Enqueue a job into appropriate queue to be run after the specified
   * duration.
   *
   *        Template parameters:
   *        - Rep, Period: std::chrono::duration parameterization.
   *
   *        Note: timed jobs are converted to a microseconds since
   *        boot/monotonic start stored in a min-heap to guarantee
   *        earliest-first execution.
   *
   * @param fnc The callable to be executed, either Dmn_Runtime_Job::FncType or
   *            Dmn_Runtime_Job::TaskFncType.
   * @param duration The duration after that the fnc is posted for execution.
   * @param priority The job priority.
   * @param onErrorFnc The callable for exception thrown inside fnc.
   */
  template <typename F, class Rep, class Period>
    requires IsValidJobFnc<F>
  void addTimedJob(
      F &&fnc, std::chrono::duration<Rep, Period> duration,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
      Dmn_Runtime_Job::OnErrorFncType onErrorFnc = {});

  /**
   * @brief Clear all registered handler hooks for the signal number. Note that
   *        only client registered signal handler hooks are clear.
   *
   * @param signo The POSIX signal number
   */
  void clearSignalHandlerHook(int signo);

  /**
   * @brief Start processing runtime events / jobs. Blocks until exitMainLoop()
   *        is called or the runtime decides to stop (a signal handler hook
   *        calls exitMainLoop).
   */
  void enterMainLoop();

  /**
   * Request the runtime to stop processing and exit the main loop.
   */
  void exitMainLoop();

  /**
   * @brief Register a signal handler hook for a particular signal number.
   *        Handlers are invoked by the runtime in a safe context (not from
   *        the raw signal handler) in a singleton asynchronous thread context.
   *
   * @param signo The POSIX signal number
   * @param hook  The signal handler hook function to be called when the
   *              signal is raised.
   */
  void registerSignalHandlerHook(int signo, SignalHandlerHook &&hook);

  static void runPriorToCreateInstance();

private:
  void addRuntimeJobToCoroutineSchedulerContext(Dmn_Runtime_Job &&job);

  template <typename F>
    requires IsValidJobFnc<F>
  auto createJobTaskFnc(F &&fnc) -> Dmn_Runtime_Job::TaskFncType;

  void clearSignalHandlerHookInternal(int signo);

  void execRuntimeJobInternal();
  void execSignalHandlerHookInternal(int signo);

  auto isRunInAsyncThread() -> bool;

  void registerSignalHandlerHookInternal(int signo, SignalHandlerHook &&hook);

  auto runRuntimeCoroutineScheduler() -> bool;
  void runRuntimeJobExecutor();

  void setNextTimer(SecInt sec, NSecInt nsec);
  void setNextTimerAt(TimePoint tp);

  /**
   * Internal state
   */
  std::unique_ptr<Dmn_Proc> m_signalWaitProc{};
  sigset_t m_mask{};
  std::unordered_map<int, SignalHandlerHook>
      m_signal_handler_hooks{}; // internal handler hooks
  std::unordered_map<int, std::vector<SignalHandlerHook>>
      m_signal_handler_hooks_external{}; // external handler hooks

  // Per-priority immediate job queues
  QueueType<Dmn_Runtime_Job> m_highQueue{};
  QueueType<Dmn_Runtime_Job> m_lowQueue{};
  QueueType<Dmn_Runtime_Job> m_mediumQueue{};

  // Min-heap of timed jobs (earliest timestamp at top)
  std::priority_queue<Dmn_Runtime_Job, std::vector<Dmn_Runtime_Job>,
                      TimedJobComparator>
      m_timedQueue{};

  // Atomic flags used for coordination (lightweight spin semantics)
  std::atomic_flag m_main_enter_atomic_flag{};
  std::atomic_flag m_main_exit_atomic_flag{};

  // Number of high, medium and low priority jobs scheduled and add to
  // pending queue waiting for scheduler.
  std::atomic<std::size_t> m_jobs_count{};

  // Small LIFO stack used by the scheduler to reorder or delay execution
  std::stack<Dmn_Runtime_Job> m_sched_job{};
  std::stack<Dmn_Runtime_Task> m_sched_task{};

  // Pointer to the platform-specific implementation (PIMPL).
  // Owned and managed by Dmn_Runtime_Manager; created and destroyed
  // in the corresponding implementation file (dmn-runtime.cpp).
  detail::Dmn_Runtime_Manager_Impl *m_pimpl{};

  // the id of the singleton asynchronous thread context.
  std::thread::id m_asyncThreadId{};

  /**
   * Static members for singleton management
   * - s_mask: signal mask applied during singleton creation.
   */
  inline static sigset_t s_mask{};
}; // class Dmn_Runtime_Manager

template <template <class> class QueueType>
Dmn_Runtime_Manager<QueueType>::Dmn_Runtime_Manager()
    : Dmn_Singleton<Dmn_Runtime_Manager<QueueType>>{},
      Dmn_Async<QueueType>{"Dmn_Runtime_Manager"},
      m_mask{Dmn_Runtime_Manager::s_mask} {
  this->addExecTask([this]() { m_asyncThreadId = std::this_thread::get_id(); });

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
        this->setNextTimerAt(job.m_due);

        break;
      }
    }
  };

  // create m_pimpl as last step
  m_pimpl = detail::Dmn_Runtime_Manager_Impl_create();
}

template <template <class> class QueueType>
Dmn_Runtime_Manager<QueueType>::~Dmn_Runtime_Manager() noexcept try {
  exitMainLoop();

  assert(m_main_exit_atomic_flag.test(std::memory_order_acquire));
  assert(!m_main_enter_atomic_flag.test(std::memory_order_acquire));

  if (m_pimpl) {
    detail::Dmn_Runtime_Manager_Impl_destroy(&m_pimpl);
  }

  // it is important that we wait for all Dmn_Runtime_Job and its companion
  // Dmn_Runtime_Task to be destroyed and unwound from the stack, as the
  // coroutine frame needs to be deleted prior to the destruction of
  // Dmn_Runtime_Manager.

  this->waitForEmpty();

  assert(this->m_sched_job.empty());
  assert(this->m_sched_task.empty());
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <template <class> class QueueType>
template <typename F>
  requires IsValidJobFnc<F>
void Dmn_Runtime_Manager<QueueType>::addJob(
    F &&fnc, Dmn_Runtime_Job::Priority priority,
    Dmn_Runtime_Job::OnErrorFncType onErrorFnc) {
  Dmn_Runtime_Job rjob{.m_priority = priority,
                       .m_fnc =
                           std::move(createJobTaskFnc(std::forward<F>(fnc))),
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
      m_main_enter_atomic_flag.test(std::memory_order_acquire)) {
    this->runRuntimeJobExecutor();
  }
}

template <template <class> class QueueType>
template <typename F, class Rep, class Period>
  requires IsValidJobFnc<F>
void Dmn_Runtime_Manager<QueueType>::addTimedJob(
    F &&fnc, std::chrono::duration<Rep, Period> duration,
    Dmn_Runtime_Job::Priority priority,
    Dmn_Runtime_Job::OnErrorFncType onErrorFnc) {
  Dmn_Runtime_Job rjob{.m_priority = priority,
                       .m_fnc =
                           std::move(createJobTaskFnc(std::forward<F>(fnc))),
                       .m_due = std::chrono::steady_clock::now() + duration,
                       .m_onErrorFnc = std::move(onErrorFnc)};

  // add rjob to m_timedQueue via singleton main asynchronous thread
  this->addExecTask([this, rjob = std::move(rjob)]() {
    assert(isRunInAsyncThread());

    this->m_timedQueue.push(std::move(rjob));
    this->setNextTimerAt(this->m_timedQueue.top().m_due);
  });
}

/**
 * @brief The method will execute the job in runtime coroutine context.
 *
 * @param job The job to be run in the runtime context
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::addRuntimeJobToCoroutineSchedulerContext(
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

    throw;
  }

  assert(this->m_sched_job.size() == this->m_sched_task.size());
}

template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::clearSignalHandlerHook(int signo) {
  this->addExecTask(
      [this, signo]() mutable { this->clearSignalHandlerHookInternal(signo); });
}

template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::clearSignalHandlerHookInternal(int signo) {
  assert(isRunInAsyncThread());

  auto it = m_signal_handler_hooks_external.find(signo);
  if (m_signal_handler_hooks_external.end() != it) {
    it->second.clear();
  }
}

/**
 * @brief The method creates a TaskFncType object based on if the input
 *        fnc type uses coroutine or not.
 *
 * @param fnc is either TaskFncType or FncType.
 *
 * @return The TaskFncType for callable coroutine task function.
 */
template <template <class> class QueueType>
template <typename F>
  requires IsValidJobFnc<F>
auto Dmn_Runtime_Manager<QueueType>::createJobTaskFnc(F &&fnc)
    -> Dmn_Runtime_Job::TaskFncType {
  Dmn_Runtime_Job::TaskFncType taskFnc{};

  if constexpr (std::is_invocable_r_v<Dmn_Runtime_Task, F,
                                      const Dmn_Runtime_Job &>) {
    // User gave us a coroutine: use it directly
    taskFnc = std::forward<F>(fnc);
  } else {
    // User gave us a void function: wrap it
    taskFnc = [f = std::forward<F>(fnc)](
                  const Dmn_Runtime_Job &j) mutable -> Dmn_Runtime_Task {
      f(j);
      co_return;
    };
  }

  return taskFnc;
}

/**
 * @brief The method will schedule job and dispatch it as a coroutine task.
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::execRuntimeJobInternal() {
  Dmn_Runtime_Job::Priority state{}; // not high, not medium, not low

  assert(isRunInAsyncThread());
  assert(this->m_sched_job.size() == this->m_sched_task.size());

  if (!this->m_sched_job.empty()) {
    state = this->m_sched_job.top().m_priority;
  }

  try {
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

    auto jobsCountInScheduler = this->m_sched_job.size();

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
  } catch (...) {
    if (this->m_jobs_count.fetch_sub(1) > 1) {
      this->runRuntimeJobExecutor();
    }
  }
}

/**
 * @brief The method will exit the Dmn_Runtime_Manager mainloop, returns control
 *        (usually the mainthread) to the main() function to be continued.
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::exitMainLoop() {
  if (!m_main_exit_atomic_flag.test_and_set(std::memory_order_release)) {
    m_main_enter_atomic_flag.clear(std::memory_order_release);
    m_main_exit_atomic_flag.notify_all();

    // set the last timer, so that signalWaitProc gracefully exit.
    this->setNextTimer(0, 10000);

    // after setting the main loop enter (false) and exit (true) flags, we just
    // have to wait for signalWaitProc to gracefully exit as it is triggered
    // at fixed intervals before we clear the timer.
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
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::enterMainLoop() {
  // If there are already pending jobs (e.g., from a prior run), ensure the
  // runtime job executor is started so the system does not remain idle.
  bool startJobExecutor = m_jobs_count.load(std::memory_order_acquire) > 0;

  if (m_main_enter_atomic_flag.test_and_set(std::memory_order_acquire)) {
    assert(false && "Error: enter main loop twice without exit");

    throw std::runtime_error("Error: enter main loop twice without exit");
  }

  m_main_exit_atomic_flag.clear(std::memory_order_release);
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

          if (m_main_exit_atomic_flag.test(std::memory_order_acquire)) {
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

  while (!m_main_exit_atomic_flag.test(std::memory_order_acquire)) {
    m_main_exit_atomic_flag.wait(false, std::memory_order_acquire);
  }
}

/**
 * @brief The method executes the signal handlers in the singleton
 *        asynchronous thread context.
 *
 * @param signo signal number that is raised
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::execSignalHandlerHookInternal(int signo) {
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
 *        singleton asynchronous thread context.
 *
 * @return True or false
 */
template <template <class> class QueueType>
auto Dmn_Runtime_Manager<QueueType>::isRunInAsyncThread() -> bool {
  return std::this_thread::get_id() == m_asyncThreadId;
}

template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::registerSignalHandlerHook(
    int signo, SignalHandlerHook &&hook) {
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
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::registerSignalHandlerHookInternal(
    int signo, SignalHandlerHook &&hook) {
  assert(isRunInAsyncThread());

  auto &extHandlerHooks = m_signal_handler_hooks_external[signo];
  extHandlerHooks.push_back(std::move(hook));
}

template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::runPriorToCreateInstance() {
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
template <template <class> class QueueType>
auto Dmn_Runtime_Manager<QueueType>::runRuntimeCoroutineScheduler() -> bool {
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
          auto &ep = task.m_handle.promise().m_except;
          if (ep && runningJob.m_onErrorFnc) {
            std::rethrow_exception(ep);
          }

          break;
        } else {
          assert(!this->m_sched_job.empty());
          assert(!this->m_sched_task.empty());

          complete = false;
          break;
        }
      } while (true);
    }
  } catch (...) {
    try {
      if (runningJob.m_onErrorFnc) {
        std::exception_ptr ep = std::current_exception();

        runningJob.m_onErrorFnc(ep);
      }
    } catch (...) {
      // ignore it.
    }
  }

  if (complete) {
    this->m_sched_task.pop();
    this->m_sched_job.pop();
  }

  return complete;
}

/**
 * @brief The method runs the job executor in the singleton asynchronous thread
 *        context.
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::runRuntimeJobExecutor() {
  this->addExecTask([this]() -> void { this->execRuntimeJobInternal(); });
}

/**
 * @brief The method sets the next scheduled timer (SIGALRM) according to the
 *        timepoint. If the timepoint is in now or the past, SIGALRM is
 *        scheduled after 10us.

 * @param tp The timepoint after that the timer (SIGALRM) will be triggered.
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::setNextTimerAt(TimePoint tp) {
  detail::Dmn_Runtime_Manager_Impl_setNextTimerAt(this->m_pimpl, tp);
}

/**
 * @brief The method sets the next scheduled timer (SIGALRM) after seconds +
 *        nanoseconds.
 *
 * @param sec The seconds after that to run the timer (SIGALRM)
 * @param nsec The nanoseconds after that to run the timer (SIGALRM)
 */
template <template <class> class QueueType>
void Dmn_Runtime_Manager<QueueType>::setNextTimer(SecInt sec, NSecInt nsec) {
  detail::Dmn_Runtime_Manager_Impl_setNextTimer(m_pimpl, sec, nsec);
}

} // namespace dmn

#endif // DMN_RUNTIME_HPP_
