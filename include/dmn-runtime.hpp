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
 *   (high/medium/low) as well as delayed (timed) jobs.
 * - All jobs are scheduled (if via addTimedJob()), serialized and executed in
 *   a singleton asynchronous thread context.
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
 *   and then the attached signal handler hook function(s) are executed in the
 *   singleton asynchronous thread context.
 *
 * Usage Summary
 * -------------
 * - Create or obtain the singleton via Dmn_Singleton::createInstance().
 * - Register signal handler hook function using
 *   registerSignalHandlerHook(signo, hook).
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
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <csignal>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <stack>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dmn-async.hpp"
#include "dmn-blockingqueue.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

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
        if (h && h.promise().continuation) {
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
    if (handle) {
      handle.promise().continuation = caller_handle;
      handle.resume(); // Start the child coroutine
    }
  }

  void await_resume() {}

  ~Dmn_Runtime_Task() noexcept {
    if (handle) {
      handle.destroy();
    }
  }

  // Construct a task that takes ownership of the given coroutine handle.
  explicit Dmn_Runtime_Task(std::coroutine_handle<promise_type> h) noexcept
      : handle(h) {}

  Dmn_Runtime_Task(const Dmn_Runtime_Task &) = delete;
  Dmn_Runtime_Task &operator=(const Dmn_Runtime_Task &) = delete;

  // Move: transfer ownership
  Dmn_Runtime_Task(Dmn_Runtime_Task &&other) noexcept
      : handle(std::exchange(other.handle, nullptr)) {}

  Dmn_Runtime_Task &operator=(Dmn_Runtime_Task &&other) noexcept {
    if (this != &other) {
      if (handle) {
        handle.destroy();
      }
      handle = std::exchange(other.handle, nullptr);
    }
    return *this;
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
 *  - m_due: TimePoint representing the absolute time (since boot/monotonic
 *           start) when the job should be executed.
 */
struct Dmn_Runtime_Job {
  using FncType = std::function<Dmn_Runtime_Task(const Dmn_Runtime_Job &j)>;

  enum Priority : int { kSched = 0, kHigh = 1, kMedium, kLow };

  Priority m_priority{kMedium};
  FncType m_job{};
  TimePoint m_due{};
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

// Platform specific implementation
struct Dmn_Runtime_Manager_Impl;

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
 *  - registerSignalHandlerHook(signo, hook): register a handler to be invoked
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
  using SignalHandlerHook = std::function<void(int signo)>;

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
      Dmn_Runtime_Job::FncType job,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium);

  /**
   * Schedule a job to run after the specified duration.
   * If the kernel clock read fails, an exception is thrown.
   *
   * Template parameters:
   *  - Rep, Period: std::chrono::duration parameterization (e.g. milliseconds).
   *
   * Note: timed jobs are converted to a microseconds since boot/monotonic start
   * stored in a min-heap to guarantee earliest-first execution.
   */
  template <class Rep, class Period>
  void addTimedJob(
      Dmn_Runtime_Job::FncType job, std::chrono::duration<Rep, Period> duration,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium) {
    struct timespec ts{};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
      auto d = std::chrono::seconds{ts.tv_sec} +
               std::chrono::nanoseconds{ts.tv_nsec};

      TimePoint tp =
          TimePoint{std::chrono::duration_cast<Clock::duration>(d)} + duration;

      Dmn_Runtime_Job rjob{
          .m_priority = priority, .m_job = std::move(job), .m_due = tp};

      // add rjob to m_timedQueue via singleton main asynchronous thread
      this->addExecTask([this, rjob = std::move(rjob)]() {
        this->m_timedQueue.push(std::move(rjob));
      });
    } else {
      throw std::runtime_error("Error in clock_gettime: " +
                               std::system_category().message(errno));
    }
  }

  /**
   * @brief Start processing runtime events / jobs. Blocks until exitMainLoop()
   * is called or the runtime decides to stop (a signal handler hook calls
   *        exitMainLoop).
   */
  void enterMainLoop();

  /**
   * Request the runtime to stop processing and exit the main loop.
   */
  void exitMainLoop();

  /**
   * @brief Register a signal handler for a particular signal number. Handlers
   *        are invoked by the runtime in a safe context (not from the raw
   * signal handler) in a singleton asynchronous thread context.
   *
   * @param signo The POSIX signal number
   * @param hook  The signal handler hook function to be called when the
   *              signal is raised.
   */
  void registerSignalHandlerHook(int signo, SignalHandlerHook &&hook);

  static void runPriorToCreateInstance();

private:
  // Helpers for pushing jobs to the appropriate priority buffer.
  void addHighJob(Dmn_Runtime_Job::FncType &&job);
  void addLowJob(Dmn_Runtime_Job::FncType &&job);
  void addMediumJob(Dmn_Runtime_Job::FncType &&job);

  void execRuntimeJobInContext(Dmn_Runtime_Job &&job);
  void execRuntimeJobInternal();
  void execSignalHandlerHookInternal(int signo);

  void registerSignalHandlerHookInternal(int signo, SignalHandlerHook &&hook);

  void runRuntimeJobExecutor();

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
  Dmn_BlockingQueue<Dmn_Runtime_Job> m_highQueue{};
  Dmn_BlockingQueue<Dmn_Runtime_Job> m_lowQueue{};
  Dmn_BlockingQueue<Dmn_Runtime_Job> m_mediumQueue{};

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
  std::stack<Dmn_Runtime_Job> m_sched_stack{};

  // Wrap platform specific implementation behind this unique ptr object
  // So that specific part of it is within dmn-runtime.cpp
  std::unique_ptr<Dmn_Runtime_Manager_Impl> m_pimpl{};

  /**
   * Static members for singleton management
   * - s_mask: signal mask applied during singleton creation.
   */
  static sigset_t s_mask;
}; // class Dmn_Runtime_Manager

} // namespace dmn

#endif // DMN_RUNTIME_HPP_
