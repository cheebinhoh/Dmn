/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.hpp
 * @brief The header file for dmn-runtime which implements singleton instance to
 *        manage POSIX signal and asynchronous tasks in general.
 */

#ifndef DMN_RUNTIME_HPP_

#define DMN_RUNTIME_HPP_

#include <atomic>
#include <csignal>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "dmn-async.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

class Dmn_Runtime_Manager : public Dmn_Singleton, private Dmn_Async {
  using SignalHandler = std::function<void(int signo)>;

public:
  Dmn_Runtime_Manager();
  virtual ~Dmn_Runtime_Manager() noexcept;

  Dmn_Runtime_Manager(const Dmn_Runtime_Manager &obj) = delete;
  const Dmn_Runtime_Manager &operator=(const Dmn_Runtime_Manager &obj) = delete;
  Dmn_Runtime_Manager(Dmn_Runtime_Manager &&obj) = delete;
  Dmn_Runtime_Manager &operator=(Dmn_Runtime_Manager &&obj) = delete;

  void enterMainLoop();
  void exitMainLoop();
  void registerSignalHandler(int signo, SignalHandler handler);

  friend class Dmn_Singleton;

private:
  void exitMainLoopInternal();
  void registerSignalHandlerInternal(int signo, SignalHandler handler);
  void execSignalHandlerInternal(int signo);

  template <class... U>
  static std::shared_ptr<Dmn_Runtime_Manager> createInstanceInternal(U &&...u);

  /**
   * data members for internal logic.
   */
  std::unique_ptr<Dmn_Proc> m_signalWaitProc{};
  sigset_t m_mask{};
  std::unordered_map<int, SignalHandler> m_signal_handlers{};
  std::unordered_map<int, std::vector<SignalHandler>> m_ext_signal_handlers{};

  std::atomic_flag                                    m_exit_atomic_flag{};

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

          err =
              pthread_sigmask(SIG_BLOCK, &Dmn_Runtime_Manager::s_mask, &old_mask);
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
