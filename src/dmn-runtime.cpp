/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime.cpp
 * @brief The source implementation file for dmn-runtime.
 */

#include "dmn-runtime.hpp"

#include <chrono>
#include <thread>
#include <csignal>
#include <memory>
#include <stdexcept>

#include "dmn-async.hpp"
#include "dmn-buffer.hpp"
#include "dmn-proc.hpp"

namespace dmn {

std::once_flag Dmn_Runtime_Manager::s_init_once{};
std::shared_ptr<Dmn_Runtime_Manager> Dmn_Runtime_Manager::s_instance{};
sigset_t Dmn_Runtime_Manager::s_mask{};

Dmn_Runtime_Manager::Dmn_Runtime_Manager()
    : Dmn_Singleton{}, Dmn_Async{"Dmn_Runtime_Manager"},
      m_mask{Dmn_Runtime_Manager::s_mask} {
  // default and to be overridden if needed
  m_signal_handlers[SIGTERM] = [this]([[maybe_unused]] int signo) {
    this->exitMainLoopInternal();
  };

  m_signal_handlers[SIGINT] = [this]([[maybe_unused]] int signo) {
    this->exitMainLoopInternal();
  };

  m_signalWaitProc = std::make_unique<Dmn_Proc>("DmnRuntimeManager_SignalWait");

  m_signalWaitProc->exec([this]() {
    while (true) {
      int signo{};
      int err{};

      err = sigwait(&m_mask, &signo);
      if (err) {
        throw std::runtime_error("Error in sigwait: " +
                                 std::string(strerror(errno)));
      }

      DMN_ASYNC_CALL_WITH_CAPTURE({ this->execSignalHandlerInternal(signo); },
                                  this, signo);
    }
  });
}

Dmn_Runtime_Manager::~Dmn_Runtime_Manager() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

/**
 * @brief The method will add a priority asynchronous job to be run in runtime context.
 *
 * @param job The asychronous job
 * @param priority The priority of the asychronous job
 */
void Dmn_Runtime_Manager::addJob(const std::function<void()> & job, Dmn_Runtime_Job::Priority priority) {
  switch (priority) {
    case Dmn_Runtime_Job::kHigh:
      this->addHighJob(job);
      break;

    case Dmn_Runtime_Job::kMedium:
      this->addMediumJob(job);
      break;

    case Dmn_Runtime_Job::kLow:
      this->addLowJob(job);
      break;
  }
}

/**
 * @brief The method will add high priority asynchronous job.
 *
 * @param job The high priority asynchronous job
 */
void Dmn_Runtime_Manager::addHighJob(const std::function<void()> & job) {
  while (!m_enter_high_atomic_flag.test()) {
    m_enter_high_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{Dmn_Runtime_Job::kHigh, job};
  m_highQueue.push(rjob);
}

/**
 * @brief The method will add medium priority asynchronous job.
 *
 * @param job The medium priority asynchronous job
 */
void Dmn_Runtime_Manager::addMediumJob(const std::function<void()> & job) {
  while (!m_enter_medium_atomic_flag.test()) {
    m_enter_medium_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{Dmn_Runtime_Job::kMedium, job};
  m_mediumQueue.push(rjob);
}

/**
 * @brief The method will add low priority asynchronous job.
 *
 * @param job The low priority asynchronous job
 */
void Dmn_Runtime_Manager::addLowJob(const std::function<void()> & job) {
  while (!m_enter_low_atomic_flag.test()) {
    m_enter_low_atomic_flag.wait(false, std::memory_order_relaxed);
  }

  Dmn_Runtime_Job rjob{Dmn_Runtime_Job::kLow, job};
  m_lowQueue.push(rjob);
}

/**
 * @brief The method will add an asynchronous task to run the job.
 *
 * @param duration The duration after that to run execRuntimeJobInternal in interval
 */
template <class Rep, class Period>
void Dmn_Runtime_Manager::execRuntimeJobInInterval(const std::chrono::duration<Rep, Period> &duration) {
  this->addExecTaskAfter(duration, [this](){ this->execRuntimeJobInternal(); });
}

/**
 * @brief The method will execute the job continously.
 */
void Dmn_Runtime_Manager::execRuntimeJobInternal(void) {
  // This place allows us to implement stagnant avoidance logic,
  // one potential example is that:
  // - if there is a high priority and medium job, we execute
  //   the high priority job
  // - but we then alevate the medium job to a buffer between hgh and medium
  // - if in next iteration, we have no high priority job, we execute the elevate medium job
  //   before medium or low priority jobs.
  // - if there is still high priority job, we add the elevated medium job into end of
  //   of high priority queue.

  auto item = m_highQueue.popNoWait();
  if (item) {
  } else if ((item = m_mediumQueue.popNoWait())) {
  } else if ((item = m_lowQueue.popNoWait())) {
  }

  if (item && (*item).m_job != nullptr) {
    (*item).m_job();
  }

  this->execRuntimeJobInInterval(std::chrono::milliseconds(1));
}

/**
 * @brief The method will exit the Dmn_Runtime_Manager mainloop, returns control
 *        (usually the mainthread) to the main() function to be continued.
 */
void Dmn_Runtime_Manager::exitMainLoop() {
  DMN_ASYNC_CALL_WITH_REF_CAPTURE({ this->exitMainLoopInternal(); });
}

/**
 * @brief The method will exit the Dmn_Runtime_Manager mainloop, returns control
 *        (usually the mainthread) to the main() function to be continued.
 *        This is private method to be called in the Dmn_Runtime_Manager instance
 *        asynchronous thread context.
 */
void Dmn_Runtime_Manager::exitMainLoopInternal() {
  m_signalWaitProc = {};

  m_exit_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_exit_atomic_flag.notify_all();
}

/**
 * @brief The method will enter the Dmn_Runtime_Manager mainloop, and wait
 *        for runtime loop to be exited. this is usually called by the main()
 *        method.
 */
void Dmn_Runtime_Manager::enterMainLoop() {
  m_enter_high_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_enter_high_atomic_flag.notify_all();
  Dmn_Proc::yield();

  m_enter_medium_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_enter_medium_atomic_flag.notify_all();
  Dmn_Proc::yield();

  m_enter_low_atomic_flag.test_and_set(std::memory_order_relaxed);
  m_enter_low_atomic_flag.notify_all();
  Dmn_Proc::yield();

  this->execRuntimeJobInInterval(std::chrono::milliseconds(1));

  while (!m_exit_atomic_flag.test()) {
    m_exit_atomic_flag.wait(false, std::memory_order_relaxed);
  }
}

/**
 * @brief The method executes the signal handlers in asynchronous thread context
 *
 * @param signo signal number that is raised
 */
void Dmn_Runtime_Manager::execSignalHandlerInternal(int signo) {
  auto extHandlers = m_ext_signal_handlers.find(signo);
  if (m_ext_signal_handlers.end() != extHandlers) {
    for (auto &handler : extHandlers->second) {
      handler(signo);
    }
  }

  auto handler = m_signal_handlers.find(signo);
  if (m_signal_handlers.end() != handler) {
    handler->second(signo);
  }
}

/**
 * @brief The method registers signal handler for the signal number. Note that
 *        SIGKILL and SIGSTOP can NOT be handled.
 *
 * @param signo   The POSIX signal number
 * @param handler The signal handler to be called when the signal is raised.
 */
void Dmn_Runtime_Manager::registerSignalHandler(int signo,
                                                SignalHandler handler) {
  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->registerSignalHandlerInternal(signo, handler); }, this, signo,
      handler);
}

/**
 * @brief The method registers external signal handler for the signal number.
 *        The external signal handlers are executed before default handler from
 *        Dmn_Runtime_Manager. Note that SIGKILL and SIGSTOP can NOT be handled.
 *        This is private method to be called in the Dmn_Runtime_Manager instance
 *        asynchronous thread context.
 *
 * @param signo   The POSIX signal number
 * @param handler The signal handler to be called when the signal is raised.
 */
void Dmn_Runtime_Manager::registerSignalHandlerInternal(int signo,
                                                        SignalHandler handler) {
  auto &extHandlers = m_ext_signal_handlers[signo];
  extHandlers.push_back(handler);
}

} // namespace dmn
