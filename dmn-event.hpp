/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This class implements singleton instance to manage POSIX signal, timer
 * and programatic events in general.
 */

#ifndef DMN_EVENT_HPP_

#define DMN_EVENT_HPP_

#include <csignal>
#include <functional>
#include <map>
#include <mutex>

#include "dmn-async.hpp"
#include "dmn-singleton.hpp"

namespace dmn {

class Event_Manager : public Singleton, private Async {
  using SignalHandler = std::function<void(int signo)>;

public:
  Event_Manager();
  virtual ~Event_Manager() noexcept;

  Event_Manager(const Event_Manager &dmnEventMgr) = delete;
  const Event_Manager &operator=(const Event_Manager &dmnEventMgr) = delete;
  Event_Manager(Event_Manager &&dmnEventMgr) = delete;
  Event_Manager &operator=(Event_Manager &&dmnEventManager) = delete;

  void enterMainLoop();
  void exitMainLoop();
  void registerSignalHandler(int signo, SignalHandler handler);

  friend class Singleton;

private:
  void exitMainLoopInternal();
  void registerSignalHandlerInternal(int signo, SignalHandler handler);
  void execSignalHandlerInternal(int signo);

  template <class... U>
  static std::shared_ptr<Event_Manager> createInstanceInternal(U &&...u);

  /**
   * data members for internal logic.
   */
  Proc m_signalWaitProc{"DmnEventManager_SignalWait"};
  sigset_t m_mask{};
  std::map<int, SignalHandler> m_signal_handlers{};
  std::map<int, std::vector<SignalHandler>> m_ext_signal_handlers{};

  /**
   * static variables for the global singleton instance
   */
  static std::once_flag s_init_once;
  static std::shared_ptr<Event_Manager> s_instance;
  static sigset_t s_mask;
}; // class Event_Manager

template <class... U>
std::shared_ptr<Event_Manager> Event_Manager::createInstanceInternal(U &&...u) {
  if (!Event_Manager::s_instance) {
    std::call_once(
        s_init_once,
        [](U &&...arg) {
          // We need to mask off signals before any thread is created, so that
          // all created threads will inherit the same signal mask, and block
          // the signals.
          //
          // We can NOT sigmask the signals in Event_Manager constructor
          // as its parent class Async thread has been created by the time
          // the Event_Manager constructor is run.
          sigset_t oldmask{};
          int err{};

          sigemptyset(&Event_Manager::s_mask);
          sigaddset(&Event_Manager::s_mask, SIGINT);
          sigaddset(&Event_Manager::s_mask, SIGTERM);
          sigaddset(&Event_Manager::s_mask, SIGQUIT);
          sigaddset(&Event_Manager::s_mask, SIGHUP);

          err = pthread_sigmask(SIG_BLOCK, &Event_Manager::s_mask, &oldmask);
          if (0 != err) {
            throw std::runtime_error("Error in pthread_sigmask: " +
                                     std::string(strerror(errno)));
          }

          Event_Manager::s_instance =
              std::make_shared<Event_Manager>(std::forward<U>(arg)...);
        },
        std::forward<U>(u)...);
  }

  return Event_Manager::s_instance;
} // static method createInstanceInternal()

} // namespace dmn

#endif // DMN_EVENT_HPP_
