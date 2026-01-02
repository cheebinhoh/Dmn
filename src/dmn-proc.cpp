/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-proc.cpp
 * @brief The source implementation file for dmn-proc.
 */

#include "dmn-proc.hpp"

#include <pthread.h>
#include <sched.h>

#include <cassert>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dmn {

void cleanupFuncToUnlockPthreadMutex(void *arg) {
  auto *mutex = static_cast<pthread_mutex_t *>(arg);

  pthread_mutex_unlock(mutex);
}

Dmn_Proc::Dmn_Proc(std::string_view name, const Dmn_Proc::Task &fnc)
    : m_name{name} {
  setState(State::kNew);

  if (fnc) {
    setTask(fnc);
  }
}

Dmn_Proc::~Dmn_Proc() noexcept try {
  if (getState() == State::kRunning) {
    stopExec();
  }

  setState(State::kInvalid);
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

auto Dmn_Proc::exec(const Dmn_Proc::Task &fnc) -> bool {
  if (fnc) {
    setTask(fnc);
  }

  return runExec();
}

auto Dmn_Proc::getState() const -> Dmn_Proc::State { return m_state; }

auto Dmn_Proc::setState(State state) -> Dmn_Proc::State {
  const State old_state = this->m_state;

  this->m_state = state;

  return old_state;
}

void Dmn_Proc::setTask(Dmn_Proc::Task fnc) {
  assert(getState() == State::kNew || getState() == State::kReady);

  this->m_fnc = std::move(fnc);
  setState(State::kReady);
}

auto Dmn_Proc::wait() -> bool {
  int err{};
  void *ret{};

  if (getState() != State::kRunning) {
    throw std::runtime_error("No task is exec");
  }

  err = pthread_join(m_th, &ret);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  setState(State::kReady);

  return 0 == err;
}

void Dmn_Proc::yield() {
  pthread_testcancel();
  sched_yield();
}

auto Dmn_Proc::stopExec() -> bool {
  int err{};

  if (getState() != State::kRunning) {
    throw std::runtime_error("No task is exec");
  }

  err = pthread_cancel(m_th);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  return wait();
}

auto Dmn_Proc::runExec() -> bool {
  int err{};
  State old_state{};

  if (getState() != State::kReady) {
    throw std::runtime_error("No task is assigned to the Dmn_Proc (" + m_name +
                             ")");
  }

  old_state = setState(State::kRunning);
  err = pthread_create(&m_th, nullptr, &(Dmn_Proc::runFnInThreadHelper), this);
  if (0 != err) {
    setState(old_state);
    return false;
  }

  return true;
}

auto Dmn_Proc::runFnInThreadHelper(void *context) -> void * {
  int old_state{};
  int err{};

  err = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  err = pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_state);
  if (0 != err) {
    throw std::runtime_error(std::system_category().message(err));
  }

  auto *proc = static_cast<Dmn_Proc *>(context);
  proc->m_fnc();

  return nullptr;
}

} // namespace dmn
