/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.hpp
 * @brief Runtime manager for signal handling and asynchronous jobs.
 *
 * Detailed description:
 * This header declares Dmn_Runtime_Manager, a singleton runtime that
 * centralizes POSIX signal handling and scheduling/execution of asynchronous
 * jobs within a single runtime context.
 *
 * Responsibilities:
 * - Provide a single, process-wide runtime instance (singleton) that masks and
 *   manages POSIX signals so that worker threads inherit a consistent signal
 *   mask. Signals of interest (SIGALRM, SIGINT, SIGTERM, SIGQUIT, SIGHUP)
 *   are blocked during singleton creation to prevent races between threads.
 * - Allow registration of signal handlers (both internal and external hooks),
 *   and invoke those handlers in a controlled context.
 * - Accept asynchronous jobs with priorities (High / Medium / Low) and
 *   schedule timed jobs to run after specified durations. Timed jobs are stored
 *   in a priority queue ordered by scheduled runtime.
 * - Integrate with the Dmn_Async base to execute jobs and wait efficiently for
 *   asynchronous work without busy-waiting.
 *
 * Thread-safety and signal considerations:
 * - Signals are masked (via pthread_sigmask) before the runtime and its worker
 *   threads are created so all descendant threads inherit the same signal mask.
 * - The singleton creation logic ensures the mask is applied exactly once using
 *   std::call_once.
 * - The runtime uses atomic flags and thread-safe buffers to coordinate job
 *   queues and execution between producers and the runtime thread(s).
 *
 * Usage sketch:
 *  - Acquire or create the singleton instance via the appropriate Dmn_Singleton
 *    factory mechanism (see Dmn_Singleton and createInstanceInternal).
 *  - Register signal handlers with registerSignalHandler(signo, handler).
 *  - Schedule jobs via addJob(...) or addTimedJob(...).
 *  - Start processing using enterMainLoop(); stop using exitMainLoop().
 *
 * Note:
 * - This header focuses on the public API and the in-header helper types.
 * - Implementation details (member function bodies) are located in the
 *   corresponding source files; only declarations and inline templates are
 *   present here.
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
 * Represents a unit of work that can be scheduled on the runtime.
 * - m_priority: job priority (kHigh, kMedium, kLow).
 * - m_job: callable to execute.
 * - m_runtimeSinceEpoch: 0 for immediate execution; >0 indicates an absolute
 *   microsecond timestamp since epoch when the job should run (used for timed
 *   jobs).
 */

struct Dmn_Runtime_Job {
  enum Priority : int { kHigh = 1, kMedium, kLow };

  Priority m_priority{kMedium};
  std::function<void(const Dmn_Runtime_Job &j)> m_job{};
  long long m_runtimeSinceEpoch{}; // 0 if immediate or > 0 if start later.
};

// Custom Comparator: We want the SMALLEST time to be at the top
// (priority_queue in C++ places the largest element at top by default).
struct TimedJobComparator {
  bool operator()(const Dmn_Runtime_Job &a, const Dmn_Runtime_Job &b) {
    return a.m_runtimeSinceEpoch > b.m_runtimeSinceEpoch;
  }
};

/**
 * Dmn_Runtime_Manager
 *
 * A singleton class that manages POSIX signal handling and asynchronous jobs.
 * It derives from Dmn_Singleton (to enforce a single global instance) and
 * privately inherits Dmn_Async to leverage an asynchronous execution thread.
 *
 * Public API highlights:
 * - addJob(job, priority): enqueue a job for immediate execution.
 * - addTimedJob(job, duration, priority): schedule job to run after duration.
 * - registerSignalHandler(signo, handler): attach a handler for signal signo.
 * - enterMainLoop() / exitMainLoop(): control runtime processing lifecycle.
 *
 * Important behavior:
 * - Signals used by the runtime are blocked during singleton initialization so
 *   that the runtime can manage them explicitly from a dedicated thread.
 * - Timed jobs are converted to an absolute microsecond epoch value and stored
 *   in a min-heap (priority_queue with custom comparator) so the earliest
 *   job is executed first.
 *
 * Lifetime and ownership:
 * - The singleton instance is stored in s_instance and created once using
 *   std::call_once and createInstanceInternal.
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

  void addJob(
      const RuntimeJobFncType &job,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium);

  /**
   * @brief The method will schedule and add the specified job after duration in
   * the specified priority.
   *
   * @param job The asynchronous job
   * @param duration The duration after that to schedule the job
   * @param priority The priority of the asynchronous job
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

  void enterMainLoop();
  void exitMainLoop();
  void registerSignalHandler(int signo, const SignalHandler &handler);

  void yield(const Dmn_Runtime_Job &j);

  friend class Dmn_Singleton;

private:
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
   * data members for internal logic.
   */
  std::unique_ptr<Dmn_Proc> m_signalWaitProc{};
  sigset_t m_mask{};
  std::unordered_map<int, SignalHandler> m_signal_handlers{};
  std::unordered_map<int, std::vector<SignalHandler>> m_ext_signal_handlers{};

  Dmn_Buffer<Dmn_Runtime_Job> m_highQueue{};
  Dmn_Buffer<Dmn_Runtime_Job> m_lowQueue{};
  Dmn_Buffer<Dmn_Runtime_Job> m_mediumQueue{};

  std::priority_queue<Dmn_Runtime_Job, std::vector<Dmn_Runtime_Job>,
                      TimedJobComparator>
      m_timedQueue{};

  std::atomic_flag m_enter_high_atomic_flag{};
  std::atomic_flag m_enter_low_atomic_flag{};
  std::atomic_flag m_enter_medium_atomic_flag{};
  std::atomic_flag m_exit_atomic_flag{};

  std::shared_ptr<Dmn_Async_Wait> m_async_job_wait{};

  std::stack<Dmn_Runtime_Job> m_sched_stack{};

  /**
   * static variables for the global singleton instance
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
          // We need to mask off signals before any thread is created, so that
          // all created threads will inherit the same signal mask, and block
          // the signals.
          //
          // We can NOT sigmask the signals in Dmn_Runtime_Manager constructor
          // as its parent class Dmn_Async thread has been created by the time
          // the Dmn_Runtime_Manager constructor is run.
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
