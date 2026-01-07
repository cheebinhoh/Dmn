/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.hpp
 * @brief The header file for dmn-runtime which implements singleton instance to
 *        manage POSIX signal and asynchronous tasks in a single runtime
 * context.
 */

#ifndef DMN_RUNTIME_HPP_

#define DMN_RUNTIME_HPP_

#include <atomic>
#include <csignal>
#include <functional>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

#include "dmn-async.hpp"
#include "dmn-buffer.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

struct Dmn_Runtime_Job {
  enum Priority { kHigh, kMedium, kLow };

  Priority m_priority{kMedium};
  std::function<void()> m_job{};
  long long m_runtimeSinceEpoch{}; // 0 if immediate or > 0 if start later.
};

// Custom Comparator: We want the SMALLEST time to be at the top
struct JobComparator {
  bool operator()(const Dmn_Runtime_Job &a, const Dmn_Runtime_Job &b) {
    return a.m_runtimeSinceEpoch > b.m_runtimeSinceEpoch;
  }
};

class Dmn_Runtime_Manager : public Dmn_Singleton, private Dmn_Async {
  using SignalHandler = std::function<void(int signo)>;

public:
  Dmn_Runtime_Manager();
  virtual ~Dmn_Runtime_Manager() noexcept;

  Dmn_Runtime_Manager(const Dmn_Runtime_Manager &obj) = delete;
  const Dmn_Runtime_Manager &operator=(const Dmn_Runtime_Manager &obj) = delete;
  Dmn_Runtime_Manager(Dmn_Runtime_Manager &&obj) = delete;
  Dmn_Runtime_Manager &operator=(Dmn_Runtime_Manager &&obj) = delete;

  void addJob(
      const std::function<void()> &job,
      Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium);

  /**
   * @brief The method will schedule and add the specified job after duration in
   * the specified priority.
   *
   * @param job The asychronous job
   * @param duration The duration after that to schedule the job
   * @param priority The priority of the asychronous job
   */
  template <class Rep, class Period>
  void addTimedJob(
      const std::function<void()> &job,
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

  friend class Dmn_Singleton;

private:
  void addHighJob(const std::function<void()> &job);
  void addLowJob(const std::function<void()> &job);
  void addMediumJob(const std::function<void()> &job);

  template <class... U>
  static std::shared_ptr<Dmn_Runtime_Manager> createInstanceInternal(U &&...u);

  template <class Rep, class Period>
  void
  execRuntimeJobInInterval(const std::chrono::duration<Rep, Period> &duration);
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
                      JobComparator>
      m_timedQueue{};

  std::atomic_flag m_enter_high_atomic_flag{};
  std::atomic_flag m_enter_low_atomic_flag{};
  std::atomic_flag m_enter_medium_atomic_flag{};
  std::atomic_flag m_exit_atomic_flag{};

  std::shared_ptr<Dmn_Async_Wait> m_async_job_wait{};

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
