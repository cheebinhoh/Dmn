/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.hpp
 * @brief Runtime manager for centralized signal handling and asynchronous jobs.
 *
 * Overview
 * --------
 * Dmn_Runtime_Manager provides a single, process‑wide runtime responsible for:
 * - Managing POSIX signal masking and dispatching so all worker threads inherit
 *   a consistent signal configuration.
 * - Scheduling and executing asynchronous jobs with priority levels
 *   (high/medium/low) as well as delayed (timed) jobs.
 * - Running an internal runtime thread (via Dmn_Async) that processes job
 *   queues and invokes registered signal handlers in a safe, non–signal‑handler
 *   context.
 * - Executing jobs scheduled through addJob() and addTimedJob() sequentially
 *   according to priority within a C++ coroutine execution model.
 *
 * Key Responsibilities
 * --------------------
 * - **Singleton lifecycle**: The runtime is created once using std::call_once.
 *   Signal masking is applied before any threads are spawned so all descendant
 *   threads inherit the same mask.
 * - **Signal handling**: The runtime blocks SIGALRM, SIGINT, SIGTERM, SIGQUIT,
 *   and SIGHUP during initialization. Internal and external handlers can be
 *   registered and are invoked from a controlled context rather than directly
 *   inside an async‑signal handler.
 * - **Job scheduling**:
 *   - Immediate jobs are placed into priority queues.
 *   - Timed jobs are stored in a min‑heap ordered by absolute microsecond
 *     timestamps to guarantee earliest‑first execution.
 * - **Efficient waiting**: Integrates with Dmn_Async and Dmn_Async_Wait to
 *   avoid busy‑waiting when waiting for new jobs or signals.
 *
 * Thread‑Safety & Signal‑Safety Notes
 * -----------------------------------
 * - Signals are masked in runPriorToCreateInstance() before the runtime thread
 * is created. Masking inside the constructor would be too late because the
 *   parent Dmn_Async thread may already exist.
 * - Singleton initialization is protected by std::call_once and a static
 *   std::once_flag to prevent race conditions.
 * - Atomic flags and thread‑safe buffers coordinate producers and the runtime
 *   thread. Signal handlers avoid performing non‑async‑signal‑safe operations
 *   inside the raw signal handler.
 *
 * Usage Summary
 * -------------
 * - Create or obtain the singleton via Dmn_Singleton::createInstance().
 * - Register signal handlers using registerSignalHandler(signo, handler).
 * - Enqueue immediate work with addJob() or schedule delayed work with
 *   addTimedJob().
 * - Start processing with enterMainLoop() and stop with exitMainLoop().
 *
 * Implementation Notes
 * --------------------
 * - This header defines the public API, supporting types, and inline templates.
 *   The implementation of member functions is provided in the corresponding
 *   source file(s).
 */

#ifndef DMN_RUNTIME_HPP_
#define DMN_RUNTIME_HPP_

#include <atomic>
#include <coroutine>
#include <csignal>
#include <functional>
#include <mutex>
#include <queue>
#include <stack>
#include <unordered_map>
#include <vector>

#include "dmn-async.hpp"
#include "dmn-blockingqueue.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

struct Dmn_Runtime_Task {
  struct promise_type {
    Dmn_Runtime_Task get_return_object() {
      return Dmn_Runtime_Task{
          std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() { return {}; }

    // When the task finishes, resume the "waiter"
    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }

      void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        if (h.promise().continuation) {
          h.promise().continuation.resume();
        }
      }

      void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() { std::terminate(); }

    void return_void() {}

    std::coroutine_handle<> continuation; // The handle of the caller
  };

  std::coroutine_handle<promise_type> handle;

  // This makes the Dmn_Runtime_Task "Awaitable"
  bool await_ready() { return false; }

  void await_suspend(std::coroutine_handle<> caller_handle) {
    handle.promise().continuation = caller_handle;
    handle.resume(); // Start the child coroutine
  }

  void await_resume() {}

  ~Dmn_Runtime_Task() noexcept {
    if (handle)
      handle.destroy();
  }

  bool isValid() const {
    // Returns true if the handle points to a coroutine frame
    return handle ? true : false;
  }
};

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
  using FncType = std::function<Dmn_Runtime_Task(const Dmn_Runtime_Job &j)>;

  enum Priority : int { kSched = 0, kHigh = 1, kMedium, kLow };

  Priority m_priority{kMedium};
  FncType m_job{};
  long long
      m_runtimeSinceEpoch{}; // 0: immediate; >0: absolute microsecond epoch
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
 *  - enterMainLoop() / exitMainLoop(): control the runtime processing
 * lifecycle.
 *
 * Important behaviour:
 *  - Signals used by the runtime are blocked prior to creating the singleton
 *    so that all threads inherit the same mask. This avoids race conditions
 *    where signals could be delivered to a thread that has not yet registered
 *    or initialized handler state.
 *  - Timed jobs use absolute microsecond timestamps to avoid cumulative drift
 *    from repeated short-duration sleeps.
 */
class Dmn_Runtime_Manager : public Dmn_Singleton<Dmn_Runtime_Manager>,
                            private Dmn_Async {
public:
  using SignalHandler = std::function<void(int signo)>;

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
      const Dmn_Runtime_Job::FncType &job,
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
      const Dmn_Runtime_Job::FncType &job,
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

  static void runPriorToCreateInstance();

private:
  // Helpers for pushing jobs to the appropriate priority buffer.
  void addHighJob(const Dmn_Runtime_Job::FncType &job);
  void addLowJob(const Dmn_Runtime_Job::FncType &job);
  void addMediumJob(const Dmn_Runtime_Job::FncType &job);

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
  std::unordered_map<int, SignalHandler>
      m_signal_handlers{}; // internal handlers
  std::unordered_map<int, std::vector<SignalHandler>>
      m_ext_signal_handlers{}; // external hooks

  // Per-priority immediate job queues
  Dmn_BlockingQueue<Dmn_Runtime_Job> m_highQueue{};
  Dmn_BlockingQueue<Dmn_Runtime_Job> m_lowQueue{};
  Dmn_BlockingQueue<Dmn_Runtime_Job> m_mediumQueue{};

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
  std::shared_ptr<Dmn_Async_Handle> m_async_job_wait{};

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

} // namespace dmn

#endif // DMN_RUNTIME_HPP_
