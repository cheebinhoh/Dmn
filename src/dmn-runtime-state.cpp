/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-runtime-state.cpp
 * @brief Runtime-state lifecycle, scheduling, and manager-retention
 *        implementation.
 *
 * Implementation Notes
 * --------------------
 * Lifecycle flags and the completion promise are synchronized by
 * Dmn_Runtime_State::m_mutex. The manager separately protects its retained
 * state handles while jobs are queued or running. Each job executes at most
 * one user-state callback; Dmn_State folds internal initialization and
 * finalization into that dispatch. Lifecycle hooks always run after the
 * lifecycle mutex is released so derived implementations can safely inspect
 * state or call other public APIs.
 */

#include "dmn-runtime-state.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace dmn {

Dmn_Runtime_State::Dmn_Runtime_State(std::string_view name)
    : Dmn_State{name},
      m_completionSharedFuture{m_completionPromise.get_future().share()} {}

Dmn_Runtime_State::~Dmn_Runtime_State() {}

void Dmn_Runtime_State::setRuntimeStateFnc(RuntimeStateFnc fnc, int index) {
  Dmn_State::setStateFnc(
      [fnc = std::move(fnc)](Dmn_State &state) mutable {
        fnc(static_cast<Dmn_Runtime_State &>(state));
      },
      index);
}

void Dmn_Runtime_State::beforeSetStateFnc() {
  std::lock_guard lock{m_mutex};

  if (m_queued || m_running || m_terminal) {
    throw std::logic_error(
        "Dmn_Runtime_State::setStateFnc cannot modify configuration after the "
        "state has been submitted or reached a terminal outcome");
  }
}

void Dmn_Runtime_State::beforeSetNext() {
  std::lock_guard lock{m_mutex};

  const bool internalCaller =
      m_internalExecutionDepth > 0 &&
      m_internalExecutionThread == std::this_thread::get_id();
  if ((m_queued || m_running || m_terminal) && !internalCaller) {
    throw std::logic_error(
        "Dmn_Runtime_State transition changes are reserved for the runtime "
        "callback after submission or a terminal outcome");
  }
}

void Dmn_Runtime_State::beforeSetEnd() {
  std::lock_guard lock{m_mutex};

  const bool internalCaller =
      m_internalExecutionDepth > 0 &&
      m_internalExecutionThread == std::this_thread::get_id();
  if ((m_queued || m_running || m_terminal) && !internalCaller) {
    throw std::logic_error(
        "Dmn_Runtime_State termination is reserved for the runtime callback "
        "after submission or a terminal outcome");
  }
}

void Dmn_Runtime_State::beforeRunNext() {
  std::lock_guard lock{m_mutex};

  const bool internalCaller =
      m_internalExecutionDepth > 0 &&
      m_internalExecutionThread == std::this_thread::get_id();
  if (!internalCaller || !m_runNextPermitted) {
    throw std::logic_error(
        "Dmn_Runtime_State::runNext is reserved for runtime-managed "
        "execution");
  }

  m_runNextPermitted = false;
}

void Dmn_Runtime_State::enterInternalExecution(bool permitRunNext) {
  std::lock_guard lock{m_mutex};

  if (m_internalExecutionDepth == 0) {
    m_internalExecutionThread = std::this_thread::get_id();
  } else {
    assert(m_internalExecutionThread == std::this_thread::get_id());
  }

  ++m_internalExecutionDepth;
  if (permitRunNext) {
    assert(!m_runNextPermitted);
    m_runNextPermitted = true;
  }
}

void Dmn_Runtime_State::leaveInternalExecution() noexcept {
  std::lock_guard lock{m_mutex};

  assert(m_internalExecutionDepth > 0);
  assert(m_internalExecutionThread == std::this_thread::get_id());

  --m_internalExecutionDepth;
  if (m_internalExecutionDepth == 0) {
    m_internalExecutionThread = {};
    m_runNextPermitted = false;
  }
}

bool Dmn_Runtime_State::runNextManaged() {
  struct Execution_Guard {
    Dmn_Runtime_State *state;
    ~Execution_Guard() { state->leaveInternalExecution(); }
  };

  enterInternalExecution(true);
  Execution_Guard guard{this};

  return Dmn_State::runNext();
}

void Dmn_Runtime_State::setEndManaged() {
  struct Execution_Guard {
    Dmn_Runtime_State *state;
    ~Execution_Guard() { state->leaveInternalExecution(); }
  };

  enterInternalExecution(false);
  Execution_Guard guard{this};
  Dmn_State::setEnd();
}

void Dmn_Runtime_State::cancel() {
  bool completeNow{};

  {
    std::lock_guard lock{m_mutex};

    if (m_terminal || m_cancelled) {
      return;
    }

    m_cancelled = true;
    completeNow = !m_queued;
  }

  if (completeNow) {
    complete(Terminal_State::kCancelled);
  }
}

bool Dmn_Runtime_State::run(Dmn_Runtime_Job::Priority priority,
                            const std::chrono::steady_clock::duration &delay,
                            OnErrorFnc onError) {
  auto runtime = Dmn_Runtime_Manager<>::createInstance();

  if (runtime->isRunInAsyncThread()) {
    throw std::runtime_error(
        "Dmn_Runtime_State::run cannot run in the runtime async thread");
  }

  if (!hasStateFncs()) {
    return false;
  }

  auto self = shared_from_this();

  {
    std::lock_guard lock{m_mutex};

    if (m_terminal || m_cancelled || m_queued) {
      return false;
    }

    m_queued = true;
  }

  if (Dmn_Runtime_State_Manager::createInstance()->enqueueState(
          std::move(self), priority, delay, std::move(onError))) {

    return true;
  }

  resetQueuedAfterSubmission();

  return false;
}

std::shared_future<void> Dmn_Runtime_State::getFuture() {
  std::lock_guard lock{m_mutex};

  return m_completionSharedFuture;
}

void Dmn_Runtime_State::wait() {
  if (Dmn_Runtime_Manager<>::createInstance()->isRunInAsyncThread()) {
    throw std::runtime_error(
        "Dmn_Runtime_State::wait cannot run in the runtime async thread");
  }

  getFuture().wait();
}

bool Dmn_Runtime_State::isCancelled() const {
  std::lock_guard lock{m_mutex};

  return m_cancelled;
}

bool Dmn_Runtime_State::isCompleted() const {
  std::lock_guard lock{m_mutex};

  return m_completed;
}

bool Dmn_Runtime_State::isFailed() const {
  std::lock_guard lock{m_mutex};

  return m_failed;
}

bool Dmn_Runtime_State::isRunning() const {
  std::lock_guard lock{m_mutex};

  return m_queued || m_running;
}

bool Dmn_Runtime_State::beginStep() {
  bool callOnStarted{};

  {
    std::lock_guard lock{m_mutex};

    if (m_terminal || m_cancelled) {
      return false;
    }

    m_running = true;

    if (!m_started) {
      m_started = true;
      callOnStarted = true;
    }
  }

  if (callOnStarted) {
    onStarted();
  }

  return true;
}

void Dmn_Runtime_State::complete(Terminal_State terminalState,
                                 std::exception_ptr failure) {
  bool callOnCompleted{};
  bool callOnFailed{};
  bool callOnCancelled{};

  {
    std::lock_guard lock{m_mutex};

    if (m_terminal) {
      return;
    }

    m_terminal = true;
    m_queued = false;
    m_running = false;

    // Cancellation wins over a step that returned normally during shutdown.
    if (terminalState == Terminal_State::kCompleted && m_cancelled) {
      terminalState = Terminal_State::kCancelled;
    }

    switch (terminalState) {
    case Terminal_State::kCompleted:
      m_completed = true;
      callOnCompleted = true;
      m_completionPromise.set_value();
      break;

    case Terminal_State::kFailed:
      m_failed = true;
      m_failure = failure;
      callOnFailed = true;
      m_completionPromise.set_exception(failure);
      break;

    case Terminal_State::kCancelled:
      m_cancelled = true;
      callOnCancelled = true;
      m_completionPromise.set_value();
      break;
    }
  }

  if (callOnCompleted) {
    onCompleted();
  } else if (callOnFailed) {
    onFailed(failure);
  } else if (callOnCancelled) {
    onCancelled();
  }
}

void Dmn_Runtime_State::resetQueuedAfterSubmission() {
  bool completeNow{};

  {
    std::lock_guard lock{m_mutex};

    m_queued = false;
    completeNow = m_cancelled && !m_terminal;
  }

  if (completeNow) {
    complete(Terminal_State::kCancelled);
  }
}

void Dmn_Runtime_State::onStarted() {}

void Dmn_Runtime_State::onCompleted() {}

void Dmn_Runtime_State::onFailed(std::exception_ptr ep) { (void)ep; }

void Dmn_Runtime_State::onCancelled() {}

/**
 * @brief Initialize the singleton manager's diagnostic name.
 *
 * Runtime state scheduling and manager-side ownership are initialized when a
 * state is submitted for execution.
 *
 * @param name Human-readable manager name for diagnostics.
 */
Dmn_Runtime_State_Manager::Dmn_Runtime_State_Manager(std::string_view name)
    : m_name{name} {}

/**
 * @brief Destroy the runtime state manager.
 */
Dmn_Runtime_State_Manager::~Dmn_Runtime_State_Manager() {}

/**
 * @brief Construct a client-owned runtime state handle.
 *
 * Manager retention begins after @ref Dmn_Runtime_State::run successfully
 * queues the state for execution.
 *
 * @param name Human-readable state name used for diagnostics.
 * @return A newly constructed runtime-managed state handle.
 */
DmnRuntimeStatePtr
Dmn_Runtime_State_Manager::createState(std::string_view name) {
  return std::make_shared<Dmn_Runtime_State>(name);
}

void Dmn_Runtime_State_Manager::shutdown() {
  auto runtime = Dmn_Runtime_Manager<>::createInstance();

  if (runtime->isRunInAsyncThread()) {
    throw std::runtime_error(
        "Dmn_Runtime_State_Manager::shutdown cannot run in the runtime async "
        "thread");
  }

  std::vector<DmnRuntimeStatePtr> pendingStates;
  {
    std::lock_guard lock{m_pendingStatesMutex};

    m_shutdown = true;
    pendingStates.reserve(m_pendingStates.size());

    for (const auto &[state, handle] : m_pendingStates) {
      (void)state;
      pendingStates.emplace_back(handle);
    }
  }

  for (const auto &state : pendingStates) {
    state->cancel();
  }

  for (const auto &state : pendingStates) {
    state->getFuture().wait();
  }
}

bool Dmn_Runtime_State_Manager::enqueueState(
    DmnRuntimeStatePtr state, Dmn_Runtime_Job::Priority priority,
    const std::chrono::steady_clock::duration &delay,
    Dmn_Runtime_State::OnErrorFnc onError) {
  {
    std::lock_guard lock{m_pendingStatesMutex};
    const auto existing = m_pendingStates.find(state.get());

    if (m_shutdown && existing == m_pendingStates.end()) {
      return false;
    }

    if (existing == m_pendingStates.end()) {
      m_pendingStates.emplace(state.get(), state);
    }
  }

  const std::weak_ptr<Dmn_Runtime_State> weakState{state};
  auto schedule = [this, weakState, priority,
                   onError](const Dmn_Runtime_Job &) mutable {
    executeStateStep(weakState, priority, std::move(onError));
  };

  try {
    auto runtime = Dmn_Runtime_Manager<>::createInstance();

    if (delay == std::chrono::steady_clock::duration::zero()) {
      runtime->addJob(std::move(schedule), priority, std::move(onError));
    } else {
      runtime->addTimedJob(std::move(schedule), delay, priority,
                           std::move(onError));
    }
  } catch (...) {
    releaseState(state.get());
    state->resetQueuedAfterSubmission();

    throw;
  }

  return true;
}

void Dmn_Runtime_State_Manager::executeStateStep(
    std::weak_ptr<Dmn_Runtime_State> weakState,
    Dmn_Runtime_Job::Priority priority, Dmn_Runtime_State::OnErrorFnc onError) {
  auto state = weakState.lock();

  if (!state) {
    return;
  }

  try {
    if (!state->beginStep()) {
      state->setEndManaged();
      (void)state->runNextManaged();
      state->complete(Dmn_Runtime_State::Terminal_State::kCancelled);
      releaseState(state.get());

      return;
    }

    if (!state->runNextManaged()) {
      state->complete(Dmn_Runtime_State::Terminal_State::kCompleted);
      releaseState(state.get());

      return;
    }
  } catch (...) {
    auto failure = std::current_exception();
    state->complete(Dmn_Runtime_State::Terminal_State::kFailed, failure);
    releaseState(state.get());

    std::rethrow_exception(failure);
  }

  (void)enqueueState(std::move(state), priority,
                     std::chrono::steady_clock::duration::zero(),
                     std::move(onError));
}

void Dmn_Runtime_State_Manager::releaseState(const Dmn_Runtime_State *state) {
  std::lock_guard lock{m_pendingStatesMutex};

  m_pendingStates.erase(state);
}

} // namespace dmn
