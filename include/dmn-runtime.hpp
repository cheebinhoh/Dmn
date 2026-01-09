/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.hpp
 * @brief Runtime manager for centralized signal handling and asynchronous jobs.
 *
 * Overview
 * --------
 * Dmn_Runtime_Manager provides a single, process-wide runtime that:
 *  - Centralizes POSIX signal management so worker threads inherit a
 *    consistent signal mask.
 *  - Schedules and executes asynchronous jobs with priority levels and
 *    supports timed (delayed) execution.
 *  - Runs an internal runtime thread (via Dmn_Async) that processes job
 *    queues and dispatches registered signal handlers in a safe context.
 *
 * Key responsibilities
 * --------------------
 *  - Singleton semantics: a single instance is created and initialized once
 *    (std::call_once). Signal masking is applied before any threads are
 *    created so descendant threads inherit the same mask.
 *  - Signal handling: the runtime blocks signals of interest (SIGALRM,
 *    SIGINT, SIGTERM, SIGQUIT, SIGHUP) during initialization and provides
 *    facilities to register internal and external handlers. Signals are
 *    dispatched in a controlled context (not from an async-signal handler).
 *  - Job scheduling: provides immediate (high/medium/low) and timed jobs.
 *    Timed jobs are stored in a min-heap ordered by absolute microsecond
 *    epoch timestamp so the earliest job executes first.
 *  - Efficient waiting: integrates with Dmn_Async/Dmn_Async_Wait to avoid
 *    busy-waiting when awaiting job execution or signals.
 *
 * Thread-safety and signal-safety notes
 * -------------------------------------
 *  - Signals are masked by createInstanceInternal() before the runtime and
 *    its worker thread(s) are created. Masking in the constructor would be
 *    too late because the parent Dmn_Async thread may already exist.
 *  - The singleton initialization is protected with std::call_once and a
 *    static std::once_flag to avoid race conditions.
 *  - Atomic flags and thread-safe queues/buffers coordinate producers and the
 *    runtime thread. Signal handler registration and invocation avoid
 *    performing non-async-signal-safe work inside the signal handler itself.
 *
 * Usage sketch
 * ------------
 *  - Obtain/create the singleton via the Dmn_Singleton factory (createInstanceInternal).
 *  - Register signal handlers with registerSignalHandler(signo, handler).
 *  - Enqueue work with addJob(...) or schedule with addTimedJob(...).
 *  - Start processing with enterMainLoop(); stop with exitMainLoop().
 *
 * Implementation
 * --------------
 *  - This header contains the public API, supporting types, and inline
 *    templates. Implementation details for member functions are provided
 *    in the corresponding source file(s).
 */

#ifndef DMN_RUNTIME_HPP_
#define DMN_RUNTIME_HPP_

#include <atomic>
#include <csignal>
#include <functional>
#include <mutex>
#include <queue>
#include <stack>
#include <unordered_map>
#include <vector>

#include "dmn-async.hpp"
#include "dmn-buffer.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

/**
 * Dmn_Runtime_Job
 *
 * Represents a unit of work that can be scheduled and executed by the runtime.
 *
 * Members:
 *  - m_priority: Priority level for scheduling (kHigh, kMedium, kLow).
 *  - m_job: Callable that performs the work. It is invoked with the job
 *           metadata so the callable can inspect runtime fields if needed.
 *  - m_runtimeSinceEpoch:
 *      * 0 => immediate execution (push into appropriate immediate queue).
 *      * >0 => absolute timestamp in microseconds since the epoch when the
 *               job should be executed (used by the timed job queue).
 */
struct Dmn_Runtime_Job {
  enum Priority : int { kHigh = 1, kMedium, kLow };

  Priority m_priority{kMedium};
  std::function<void(const Dmn_Runtime_Job &j)> m_job{};
  long long m_runtimeSinceEpoch{}; // 0: immediate; >0: absolute microsecond epoch
};

// TimedJobComparator
// ------------------
// The runtime keeps timed jobs in a priority_queue. The comparator places the
// job with the smallest (earliest) m_runtimeSinceEpoch at the top of the
// container so it can be popped and executed first.
struct TimedJobComparator {
  bool operator()(const Dmn_Runtime_Job &a, const Dmn_Runtime_Job &b) {
    return a.m_runtimeSinceEpoch > b.m_runtimeSinceEpoch;
  }
};

/**
 * Dmn_Runtime_Manager
 *
 * A singleton runtime manager that centralizes POSIX signal handling and
 * asynchronous job scheduling/execution. It inherits Dmn_Singleton for
 * singleton lifecycle and privately from Dmn_Async to run an internal
 * runtime thread used for processing.
 *
 * Public API highlights:
 *  - addJob(job, priority): enqueue a job for (near-)immediate execution.
 *  - addTimedJob(job, duration, priority): schedule job to run after duration.
 *  - registerSignalHandler(signo, handler): register a handler to be invoked
 *    when the given signal is delivered.
 *  - enterMainLoop() / exitMainLoop(): control the runtime processing lifecycle.
 *
 * Important behaviour:
 *  - Signals used by the runtime are blocked prior to creating the singleton
 *    so that all threads inherit the same mask. This avoids race conditions
 *    where signals could be delivered to a thread that has not yet registered
 *    or initialized handler state.
 *  - Timed jobs use absolute microsecond timestamps to avoid cumulative drift
 *    from repeated short-duration sleeps.
 */
class Dmn_Runtime_Manager : public Dmn_Singleton, private Dmn_Async {
public:
  using SignalHandler = std::function<void(int signo)>;
  using RuntimeJobFncType = std::function<void(const Dmn_Runtime_Job &j)>;

  Dmn_Runtime_Manager();
  virtual ~Dmn_Runtime_Manager() noexcept;

  Dmn_Runtime_Manager(const Dmn_Runtime_Manager &obj) = delete;
  const Dmn_Runtime_Manager &operator=(const Dmn_Runtime_Manager &obj) = delete;
  Dmn_Runtime_Manager(Dmn_Runtime_Manager &&obj) = delete;
  Dmn_Runtime_Manager &operator=(Dmn_Runtime_Manager &&obj) = delete;

  /**
   * Enqueue a job for immediate execution with the given priority.
   * The runtime will schedule the job onto the appropriate internal buffer.
   */
  void addJob(
      const RuntimeJobFncType &job,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium);

  /**
   * Schedule a job to run after the specified duration.
   * If the kernel clock read fails, the job falls back to being enqueued
   * for immediate execution.
   *
   * Template parameters:
   *  - Rep, Period: std::chrono::duration parameterization (e.g. milliseconds).
   *
   * Note: timed jobs are converted to an absolute microsecond epoch value and
   * stored in a min-heap to guarantee earliest-first execution.
   */
  template <class Rep, class Period>
  void addTimedJob(
      const RuntimeJobFncType &job,
      const std::chrono::duration<Rep, Period> &duration,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium) {
    struct timespec ts{};

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
      auto usec =
          std::chrono::duration_cast<std::chrono::microseconds>(duration);

      long long microseconds =
          ((long long)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000)) +
          usec.count();

      Dmn_Runtime_Job rjob{.m_priority = Dmn_Runtime_Job::kHigh,
                           .m_job = job,
                           .m_runtimeSinceEpoch = microseconds};
      m_timedQueue.push(rjob);
    } else {
      addJob(job, priority);
    }
  }

  /**
   * Start processing runtime events / jobs. Blocks until exitMainLoop() is
   * called or the runtime decides to stop.
   */
  void enterMainLoop();

  /**
   * Request the runtime to stop processing and exit the main loop.
   */
  void exitMainLoop();

  /**
   * Register a signal handler for a particular signal number. Handlers are
   * invoked by the runtime in a safe context (not from the raw signal handler).
   */
  void registerSignalHandler(int signo, const SignalHandler &handler);

  /**
   * Yield control for the current job: scheduling callback invoked by job
   * implementations to re-enqueue or otherwise cooperate with the runtime.
   */
  void yield(const Dmn_Runtime_Job &j);

  friend class Dmn_Singleton;

private:
  // Helpers for pushing jobs to the appropriate priority buffer.
  void addHighJob(const RuntimeJobFncType &job);
  void addLowJob(const RuntimeJobFncType &job);
  void addMediumJob(const RuntimeJobFncType &job);

  template <class... U>
  static std::shared_ptr<Dmn_Runtime_Manager> createInstanceInternal(U &&...u);

  template <class Rep, class Period>
  void
  execRuntimeJobForInterval(const std::chrono::duration<Rep, Period> &duration);
  void execRuntimeJobInContext(Dmn_Runtime_Job &&job);
  void execRuntimeJobInternal();
  void execSignalHandlerInternal(int signo);

  void exitMainLoopInternal();

  void registerSignalHandlerInternal(int signo, const SignalHandler &handler);

  /**
   * Internal state
   */
  std::unique_ptr<Dmn_Proc> m_signalWaitProc{};
  sigset_t m_mask{};
  std::unordered_map<int, SignalHandler> m_signal_handlers{};        // internal handlers
  std::unordered_map<int, std::vector<SignalHandler>> m_ext_signal_handlers{}; // external hooks

  // Per-priority immediate job queues
  Dmn_Buffer<Dmn_Runtime_Job> m_highQueue{};
  Dmn_Buffer<Dmn_Runtime_Job> m_lowQueue{};
  Dmn_Buffer<Dmn_Runtime_Job> m_mediumQueue{};

  // Min-heap of timed jobs (earliest timestamp at top)
  std::priority_queue<Dmn_Runtime_Job, std::vector<Dmn_Runtime_Job>,
                      TimedJobComparator>
      m_timedQueue{};

  // Atomic flags used for coordination (lightweight spin semantics)
  std::atomic_flag m_enter_high_atomic_flag{};
  std::atomic_flag m_enter_low_atomic_flag{};
  std::atomic_flag m_enter_medium_atomic_flag{};
  std::atomic_flag m_exit_atomic_flag{};

  // Wait object used to efficiently block until there is work
  std::shared_ptr<Dmn_Async_Wait> m_async_job_wait{};

  // Small LIFO stack used by the scheduler to reorder or delay execution
  std::stack<Dmn_Runtime_Job> m_sched_stack{};

  /**
   * Static members for singleton management
   * - s_init_once: ensures the singleton is initialized once.
   * - s_instance: shared_ptr holding the singleton instance.
   * - s_mask: signal mask applied during singleton creation.
   */
  static std::once_flag s_init_once;
  static std::shared_ptr<Dmn_Runtime_Manager> s_instance;
  static sigset_t s_mask;
}; // class Dmn_Runtime_Manager

template <class... U>
std::shared_ptr<Dmn_Runtime_Manager>
Dmn_Runtime_Manager::createInstanceInternal(U &&...u) {
  if (!Dmn_Runtime_Manager::s_instance) {
    std::call_once(
        s_init_once,
        [](U &&...arg) {
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

          err = pthread_sigmask(SIG_BLOCK, &Dmn_Runtime_Manager::s_mask,
                                &old_mask);
          if (0 != err) {
            throw std::runtime_error("Error in pthread_sigmask: " +
                                     std::string(strerror(errno)));
          }

          Dmn_Runtime_Manager::s_instance =
              std::make_shared<Dmn_Runtime_Manager>(std::forward<U>(arg)...);
        },
        std::forward<U>(u)...);
  }

  return Dmn_Runtime_Manager::s_instance;
} // static method createInstanceInternal()

} // namespace dmn

#endif // DMN_RUNTIME_HPP_
