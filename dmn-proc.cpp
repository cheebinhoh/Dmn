/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include "dmn-proc.hpp"

#include <pthread.h>
#include <sched.h>

#include <cassert>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dmn {

void cleanupFuncToUnlockPthreadMutex(void *arg) {
  pthread_mutex_t *mutex = (pthread_mutex_t *)arg;

  pthread_mutex_unlock(mutex);
}

Proc::Proc(std::string_view name, Proc::Task fn) : m_name{name} {
  setState(State::kNew);

  if (fn) {
    setTask(fn);
  }
}

Proc::~Proc() noexcept try {
  if (getState() == State::kRunning) {
    stopExec();
  }

  setState(State::kInvalid);
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

bool Proc::exec(Proc::Task fn) {
  if (fn) {
    setTask(fn);
  }

  return runExec();
}

Proc::State Proc::getState() const { return m_state; }

Proc::State Proc::setState(State state) {
  State old_state = this->m_state;

  this->m_state = state;

  return old_state;
}

void Proc::setTask(Proc::Task fn) {
  assert(getState() == State::kNew || getState() == State::kReady);

  this->m_fn = fn;
  setState(State::kReady);
}

bool Proc::wait() {
  int err{};
  void *pRet{};

  if (getState() != State::kRunning) {
    throw std::runtime_error("No task is exec");
  }

  err = pthread_join(m_th, &pRet);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  setState(State::kReady);

  return 0 == err;
}

void Proc::yield() {
  pthread_testcancel();
  sched_yield();
}

bool Proc::stopExec() {
  int err{};

  if (getState() != State::kRunning) {
    throw std::runtime_error("No task is exec");
  }

  err = pthread_cancel(m_th);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  return wait();
}

bool Proc::runExec() {
  int err{};
  State old_state{};

  if (getState() != State::kReady) {
    throw std::runtime_error("No task is assigned to the Proc (" + m_name +
                             ")");
  }

  old_state = setState(State::kRunning);
  err = pthread_create(&m_th, NULL, &(Proc::runFnInThreadHelper), this);
  if (err) {
    setState(old_state);
    return false;
  }

  return true;
}

void *Proc::runFnInThreadHelper(void *context) {
  Proc *proc{};
  int old_state{};
  int err{};

  err = pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  err = pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &old_state);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  proc = (Proc *)context;
  proc->m_fn();

  return NULL;
}

} // namespace dmn
