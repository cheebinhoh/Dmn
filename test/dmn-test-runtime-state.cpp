/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-runtime-state.cpp
 * @brief Unit tests for runtime-managed state lifecycle and scheduling.
 */

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "dmn-runtime-state.hpp"

namespace {

class Runtime_Main_Loop {
public:
  explicit Runtime_Main_Loop(
      std::shared_ptr<dmn::Dmn_Runtime_Manager<>> runtime)
      : m_runtime{std::move(runtime)},
        m_thread{[this]() { m_runtime->enterMainLoop(); }} {}

  ~Runtime_Main_Loop() { stop(); }

  Runtime_Main_Loop(const Runtime_Main_Loop &) = delete;
  Runtime_Main_Loop &operator=(const Runtime_Main_Loop &) = delete;

  void stop() {
    if (!m_stopped) {
      m_runtime->exitMainLoop();
      m_thread.join();
      m_stopped = true;
    }
  }

private:
  std::shared_ptr<dmn::Dmn_Runtime_Manager<>> m_runtime;
  std::thread m_thread;
  bool m_stopped{};
};

auto runtime() -> std::shared_ptr<dmn::Dmn_Runtime_Manager<>> {
  return dmn::Dmn_Runtime_Manager<>::createInstance();
}

auto stateManager() -> std::shared_ptr<dmn::Dmn_Runtime_State_Manager> {
  return dmn::Dmn_Runtime_State_Manager::createInstance();
}

} // namespace

TEST(DmnRuntimeState, CreatesSingletonManagerAndStateHandle) {
  auto first = stateManager();
  auto second = stateManager();

  EXPECT_NE(first, nullptr);
  EXPECT_EQ(first.get(), second.get());

  static_assert(std::is_base_of_v<dmn::Dmn_State, dmn::Dmn_Runtime_State>);
  EXPECT_NE(first->createState("state"), nullptr);
}

TEST(DmnRuntimeState, RejectsExternalMutationAfterSubmission) {
  using namespace std::chrono_literals;

  auto state = stateManager()->createState("frozen-after-run");
  state->setStateFnc([](dmn::Dmn_State &current) { current.setEnd(); });
  dmn::Dmn_State &base = *state;

  EXPECT_TRUE(state->run());
  EXPECT_THROW(state->setStateFnc([](dmn::Dmn_State &) {}), std::logic_error);
  EXPECT_THROW(state->setNext(1), std::logic_error);
  EXPECT_THROW(state->setEnd(), std::logic_error);
  EXPECT_THROW(base.runNext(), std::logic_error);

  state->cancel();
  Runtime_Main_Loop loop{runtime()};
  EXPECT_TRUE(state->wait_for(5s));
  loop.stop();
}

TEST(DmnRuntimeState, RuntimeCallbackCanObserveCancellationDirectly) {
  using namespace std::chrono_literals;

  std::promise<void> callbackStarted;
  auto callbackStartedFuture = callbackStarted.get_future();
  std::atomic_bool observedCancellation{};
  auto state = stateManager()->createState("runtime-aware-callback");
  state->setRuntimeStateFnc([&callbackStarted, &observedCancellation](
                                dmn::Dmn_Runtime_State &current) {
    callbackStarted.set_value();

    while (!current.isCancelled()) {
      std::this_thread::sleep_for(1ms);
    }

    observedCancellation = true;
    current.setEnd();
  });

  EXPECT_TRUE(state->run());
  Runtime_Main_Loop loop{runtime()};
  EXPECT_EQ(callbackStartedFuture.wait_for(5s), std::future_status::ready);
  EXPECT_THROW(state->setNext(1), std::logic_error);
  EXPECT_THROW(state->setEnd(), std::logic_error);
  state->cancel();
  EXPECT_TRUE(state->wait_for(5s));
  loop.stop();

  EXPECT_TRUE(observedCancellation.load());
  EXPECT_TRUE(state->isCancelled());
}

TEST(DmnRuntimeState, RejectsRecursiveRunNextFromRuntimeCallback) {
  using namespace std::chrono_literals;

  std::atomic_bool recursiveRunRejected{};
  std::atomic_int secondStateCount{};
  auto state = stateManager()->createState("recursive-run-next");

  state->setStateFnc(
      [&recursiveRunRejected](dmn::Dmn_State &current) {
        current.setNext();

        try {
          (void)current.runNext();
        } catch (const std::logic_error &) {
          recursiveRunRejected = true;
        }
      },
      1);
  state->setStateFnc(
      [&secondStateCount](dmn::Dmn_State &current) {
        ++secondStateCount;
        current.setEnd();
      },
      2);

  EXPECT_TRUE(state->run());
  Runtime_Main_Loop loop{runtime()};
  EXPECT_TRUE(state->wait_for(5s));
  loop.stop();

  EXPECT_TRUE(recursiveRunRejected.load());
  EXPECT_EQ(secondStateCount.load(), 1);
  EXPECT_TRUE(state->isCompleted());
}

TEST(DmnRuntimeState, RejectsUnconfiguredAndPreRunCancelledStates) {
  using namespace std::chrono_literals;

  auto manager = stateManager();
  auto unconfigured = manager->createState("unconfigured");
  auto unconfiguredFuture = unconfigured->getFuture();
  EXPECT_FALSE(unconfigured->run());
  EXPECT_EQ(unconfiguredFuture.wait_for(0ms), std::future_status::timeout);

  auto cancelled = manager->createState("cancelled");
  auto cancelledFuture = cancelled->getFuture();
  auto secondCancelledFuture = cancelled->getFuture();
  cancelled->cancel();
  cancelled->cancel();

  EXPECT_TRUE(cancelled->isCancelled());
  EXPECT_EQ(cancelledFuture.wait_for(0ms), std::future_status::ready);
  EXPECT_EQ(secondCancelledFuture.wait_for(0ms), std::future_status::ready);
  EXPECT_NO_THROW(cancelledFuture.get());
  EXPECT_NO_THROW(secondCancelledFuture.get());
  EXPECT_FALSE(cancelled->run());
}

TEST(DmnRuntimeState, ExecutesStatesAndReportsStateFailures) {
  using namespace std::chrono_literals;

  auto manager = stateManager();
  int stateCount{};
  auto state = manager->createState("count-to-three");
  state->setStateFnc([&stateCount](dmn::Dmn_State &current) {
    if (++stateCount >= 3) {
      current.setEnd();
    }
  });

  std::atomic_bool onErrorCalled{};
  auto failed = manager->createState("failed-state");
  failed->setStateFnc(
      [](dmn::Dmn_State &) { throw std::runtime_error{"state failure"}; });
  auto failedFuture = failed->getFuture();

  EXPECT_TRUE(state->run());
  EXPECT_TRUE(failed->run(dmn::Dmn_Runtime_Job::Priority::kMedium,
                          std::chrono::steady_clock::duration::zero(),
                          [&onErrorCalled](std::exception_ptr &failure) {
                            onErrorCalled = static_cast<bool>(failure);
                          }));

  Runtime_Main_Loop loop{runtime()};
  EXPECT_TRUE(state->wait_for(5s));
  EXPECT_TRUE(failed->wait_for(5s));
  loop.stop();

  EXPECT_TRUE(state->isCompleted());
  EXPECT_FALSE(state->isRunning());
  EXPECT_TRUE(static_cast<dmn::Dmn_State &>(*state).isInitialized());
  EXPECT_TRUE(static_cast<dmn::Dmn_State &>(*state).isFinalized());
  EXPECT_EQ(stateCount, 3);
  EXPECT_TRUE(failed->isFailed());
  EXPECT_TRUE(onErrorCalled);
  EXPECT_THROW(failedFuture.get(), std::runtime_error);
}

TEST(DmnRuntimeState, SerializesMultipleStateExecutions) {
  using namespace std::chrono_literals;

  constexpr int stateCount = 8;
  std::atomic_int activeSteps{};
  std::atomic_int completedSteps{};
  std::atomic_bool concurrentExecution{};
  std::atomic_bool executedInRuntimeThread{true};
  auto manager = stateManager();
  auto runtimeInstance = runtime();
  std::vector<dmn::DmnRuntimeStatePtr> states;
  states.reserve(stateCount);

  for (int index = 0; index < stateCount; ++index) {
    auto state = manager->createState("serialized-state");
    state->setStateFnc([&activeSteps, &completedSteps, &concurrentExecution,
                        &executedInRuntimeThread,
                        runtimeInstance](dmn::Dmn_State &current) {
      if (activeSteps.fetch_add(1) != 0) {
        concurrentExecution = true;
      }

      executedInRuntimeThread =
          executedInRuntimeThread && runtimeInstance->isRunInAsyncThread();
      std::this_thread::sleep_for(1ms);
      ++completedSteps;
      activeSteps.fetch_sub(1);
      current.setEnd();
    });
    EXPECT_TRUE(state->run());
    states.emplace_back(std::move(state));
  }

  Runtime_Main_Loop loop{runtimeInstance};
  for (const auto &state : states) {
    EXPECT_TRUE(state->wait_for(5s));
    EXPECT_TRUE(state->isCompleted());
  }
  loop.stop();

  EXPECT_EQ(completedSteps.load(), stateCount);
  EXPECT_EQ(activeSteps.load(), 0);
  EXPECT_FALSE(concurrentExecution.load());
  EXPECT_TRUE(executedInRuntimeThread.load());
}

TEST(DmnRuntimeState, IsolatesStateFailureFromOtherQueuedStates) {
  using namespace std::chrono_literals;

  std::atomic_bool errorCallbackCalled{};
  std::atomic_int successfulStepCount{};
  auto manager = stateManager();
  auto failed = manager->createState("isolated-failure");
  failed->setStateFnc(
      [](dmn::Dmn_State &) { throw std::runtime_error{"expected failure"}; });
  auto failedFuture = failed->getFuture();

  auto successful = manager->createState("isolated-success");
  successful->setStateFnc([&successfulStepCount](dmn::Dmn_State &current) {
    ++successfulStepCount;
    current.setEnd();
  });

  EXPECT_TRUE(failed->run(dmn::Dmn_Runtime_Job::Priority::kHigh,
                          std::chrono::steady_clock::duration::zero(),
                          [&errorCallbackCalled](std::exception_ptr &error) {
                            errorCallbackCalled = static_cast<bool>(error);
                          }));
  EXPECT_TRUE(successful->run(dmn::Dmn_Runtime_Job::Priority::kLow));

  Runtime_Main_Loop loop{runtime()};
  EXPECT_TRUE(failed->wait_for(5s));
  EXPECT_TRUE(successful->wait_for(5s));
  loop.stop();

  EXPECT_TRUE(failed->isFailed());
  EXPECT_THROW(failedFuture.get(), std::runtime_error);
  EXPECT_TRUE(errorCallbackCalled.load());
  EXPECT_TRUE(successful->isCompleted());
  EXPECT_EQ(successfulStepCount.load(), 1);
}

TEST(DmnRuntimeState, CancelsQueuedStateWithoutRunningUserStep) {
  using namespace std::chrono_literals;

  std::atomic_int stepCount{};
  auto state = stateManager()->createState("queued-cancelled-state");
  state->setStateFnc([&stepCount](dmn::Dmn_State &) { ++stepCount; });
  auto completion = state->getFuture();

  // No main loop is running, so cancellation occurs before this queued job can
  // enter its user-defined state step.
  EXPECT_TRUE(state->run());
  EXPECT_TRUE(state->isRunning());
  state->cancel();
  EXPECT_TRUE(state->isCancelled());
  EXPECT_EQ(completion.wait_for(0ms), std::future_status::timeout);

  Runtime_Main_Loop loop{runtime()};
  EXPECT_TRUE(state->wait_for(5s));
  loop.stop();

  EXPECT_TRUE(state->isCancelled());
  EXPECT_FALSE(state->isRunning());
  EXPECT_EQ(stepCount.load(), 0);
  EXPECT_TRUE(static_cast<dmn::Dmn_State &>(*state).isFinalized());
}

TEST(DmnRuntimeState, RetainsStateUntilCompletionAfterClientHandleReleased) {
  using namespace std::chrono_literals;

  std::atomic_int stepCount{};
  auto state = stateManager()->createState("manager-retained-state");
  state->setStateFnc([&stepCount](dmn::Dmn_State &current) {
    ++stepCount;
    current.setEnd();
  });
  auto completion = state->getFuture();
  std::weak_ptr<dmn::Dmn_Runtime_State> weakState{state};

  EXPECT_TRUE(state->run());
  state.reset();

  // Runtime jobs capture only weak ownership, so this must be the manager's
  // pending-state reference keeping the submitted state alive.
  EXPECT_FALSE(weakState.expired());

  Runtime_Main_Loop loop{runtime()};
  EXPECT_EQ(completion.wait_for(5s), std::future_status::ready);
  loop.stop();

  EXPECT_EQ(stepCount.load(), 1);
  EXPECT_TRUE(weakState.expired());
}

TEST(DmnRuntimeState, HonorsPriorityOrdering) {
  using namespace std::chrono_literals;

  std::vector<char> executionOrder;
  auto manager = stateManager();
  auto high = manager->createState("high-priority");
  auto medium = manager->createState("medium-priority");
  auto low = manager->createState("low-priority");
  const auto configure = [&executionOrder](dmn::DmnRuntimeStatePtr state,
                                           char marker) {
    state->setStateFnc([&executionOrder, marker](dmn::Dmn_State &current) {
      executionOrder.push_back(marker);
      current.setEnd();
    });
  };
  configure(high, 'H');
  configure(medium, 'M');
  configure(low, 'L');

  // Submit in reverse priority order so the observed order comes from runtime
  // priority scheduling rather than submission order.
  EXPECT_TRUE(low->run(dmn::Dmn_Runtime_Job::Priority::kLow));
  EXPECT_TRUE(medium->run(dmn::Dmn_Runtime_Job::Priority::kMedium));
  EXPECT_TRUE(high->run(dmn::Dmn_Runtime_Job::Priority::kHigh));

  Runtime_Main_Loop loop{runtime()};
  EXPECT_TRUE(high->wait_for(5s));
  EXPECT_TRUE(medium->wait_for(5s));
  EXPECT_TRUE(low->wait_for(5s));
  loop.stop();

  EXPECT_EQ(executionOrder, (std::vector<char>{'H', 'M', 'L'}));
}

TEST(DmnRuntimeState, DelaysInitialSubmission) {
  using namespace std::chrono_literals;

  constexpr auto delay = 100ms;
  dmn::Clock::time_point executedAt{};
  auto state = stateManager()->createState("delayed-state");
  state->setStateFnc([&executedAt](dmn::Dmn_State &current) {
    executedAt = dmn::Clock::now();
    current.setEnd();
  });

  const auto submittedAt = dmn::Clock::now();
  EXPECT_TRUE(state->run(dmn::Dmn_Runtime_Job::Priority::kMedium, delay));

  Runtime_Main_Loop loop{runtime()};
  EXPECT_EQ(state->getFuture().wait_for(delay / 2),
            std::future_status::timeout);
  EXPECT_TRUE(state->wait_for(5s));
  loop.stop();

  EXPECT_GE(executedAt - submittedAt, delay);
}

TEST(DmnRuntimeState, RejectsRunAndWaitOperationsFromRuntimeThread) {
  using namespace std::chrono_literals;

  auto state = stateManager()->createState("runtime-thread-state");
  std::atomic_bool runRejected{};
  std::atomic_bool waitRejected{};
  std::atomic_bool waitForRejected{};
  std::promise<void> completed;
  auto completedFuture = completed.get_future();
  auto runtimeInstance = runtime();

  runtimeInstance->addJob(
      [&state, &runRejected, &waitRejected, &waitForRejected,
       &completed](const dmn::Dmn_Runtime_Job &) -> dmn::Dmn_Runtime_Task {
        try {
          (void)state->run();
        } catch (const std::runtime_error &) {
          runRejected = true;
        }

        try {
          state->wait();
        } catch (const std::runtime_error &) {
          waitRejected = true;
        }

        try {
          (void)state->wait_for(0ms);
        } catch (const std::runtime_error &) {
          waitForRejected = true;
        }

        completed.set_value();
        co_return;
      });

  Runtime_Main_Loop loop{runtimeInstance};
  EXPECT_EQ(completedFuture.wait_for(5s), std::future_status::ready);
  loop.stop();

  EXPECT_TRUE(runRejected.load());
  EXPECT_TRUE(waitRejected.load());
  EXPECT_TRUE(waitForRejected.load());
}

TEST(DmnRuntimeState, HandlesConcurrentStateLifecycleOperations) {
  using namespace std::chrono_literals;

  constexpr int stateCount = 24;
  std::atomic_int completedUserSteps{};
  std::atomic_int cancelledUserSteps{};
  std::atomic_int submissionFailures{};
  std::atomic_int waitFailures{};
  std::barrier startSubmissions{stateCount};
  auto manager = stateManager();
  Runtime_Main_Loop loop{runtime()};
  std::vector<std::thread> clients;
  clients.reserve(stateCount);

  for (int index = 0; index < stateCount; ++index) {
    clients.emplace_back([index, &manager, &completedUserSteps,
                          &cancelledUserSteps, &submissionFailures,
                          &waitFailures, &startSubmissions]() {
      const bool cancelState = index % 2 == 0;
      auto state = manager->createState("concurrent-state");
      state->setStateFnc([cancelState, &completedUserSteps,
                          &cancelledUserSteps](dmn::Dmn_State &current) {
        if (cancelState) {
          ++cancelledUserSteps;
        } else {
          ++completedUserSteps;
        }

        current.setEnd();
      });

      startSubmissions.arrive_and_wait();
      if (!state->run(dmn::Dmn_Runtime_Job::Priority::kMedium,
                      cancelState ? 50ms : 0ms)) {
        ++submissionFailures;
        return;
      }

      if (cancelState) {
        state->cancel();
      }

      const auto completion = state->getFuture();
      if (completion.wait_for(5s) != std::future_status::ready ||
          (cancelState ? !state->isCancelled() : !state->isCompleted())) {
        ++waitFailures;
      }
    });
  }

  for (auto &client : clients) {
    client.join();
  }
  loop.stop();

  EXPECT_EQ(submissionFailures.load(), 0);
  EXPECT_EQ(waitFailures.load(), 0);
  EXPECT_EQ(completedUserSteps.load(), stateCount / 2);
  EXPECT_EQ(cancelledUserSteps.load(), 0);
}

TEST(DmnRuntimeState, ShutdownCancelsPendingStatesAndRejectsNewSubmissions) {
  using namespace std::chrono_literals;

  // Keep this test last: shutdown permanently disables the process-wide
  // runtime-state manager singleton.
  auto manager = stateManager();
  std::promise<void> blockingStepStarted;
  auto blockingStepStartedFuture = blockingStepStarted.get_future();
  std::promise<void> allowBlockingStepToFinish;
  auto allowBlockingStepToFinishFuture = allowBlockingStepToFinish.get_future();

  auto runningState = manager->createState("shutdown-running-state");
  runningState->setStateFnc(
      [&blockingStepStarted,
       &allowBlockingStepToFinishFuture](dmn::Dmn_State &current) {
        blockingStepStarted.set_value();
        allowBlockingStepToFinishFuture.wait();
        current.setEnd();
      });
  auto runningStateFuture = runningState->getFuture();

  constexpr int queuedStateCount = 32;
  std::atomic_int queuedStepCount{};
  std::vector<dmn::DmnRuntimeStatePtr> queuedStates;
  std::vector<std::shared_future<void>> queuedStateFutures;
  queuedStates.reserve(queuedStateCount);
  queuedStateFutures.reserve(queuedStateCount);
  for (int index = 0; index < queuedStateCount; ++index) {
    auto queuedState = manager->createState("shutdown-queued-state");
    queuedState->setStateFnc(
        [&queuedStepCount](dmn::Dmn_State &) { ++queuedStepCount; });
    queuedStateFutures.emplace_back(queuedState->getFuture());
    EXPECT_TRUE(queuedState->run(dmn::Dmn_Runtime_Job::Priority::kLow));
    queuedStates.emplace_back(std::move(queuedState));
  }

  EXPECT_TRUE(runningState->run(dmn::Dmn_Runtime_Job::Priority::kHigh));

  Runtime_Main_Loop loop{runtime()};
  EXPECT_EQ(blockingStepStartedFuture.wait_for(5s), std::future_status::ready);

  std::promise<void> shutdownReturned;
  auto shutdownReturnedFuture = shutdownReturned.get_future();
  std::thread shutdownThread{[&manager, &shutdownReturned]() {
    manager->shutdown();
    shutdownReturned.set_value();
  }};

  EXPECT_EQ(shutdownReturnedFuture.wait_for(50ms), std::future_status::timeout);
  allowBlockingStepToFinish.set_value();
  EXPECT_EQ(shutdownReturnedFuture.wait_for(5s), std::future_status::ready);
  shutdownThread.join();
  loop.stop();

  EXPECT_EQ(runningStateFuture.wait_for(0ms), std::future_status::ready);
  EXPECT_TRUE(runningState->isCancelled());
  for (const auto &queuedStateFuture : queuedStateFutures) {
    EXPECT_EQ(queuedStateFuture.wait_for(0ms), std::future_status::ready);
  }
  for (const auto &queuedState : queuedStates) {
    EXPECT_TRUE(queuedState->isCancelled());
  }
  EXPECT_EQ(queuedStepCount.load(), 0);

  auto postShutdownState = manager->createState("post-shutdown-state");
  postShutdownState->setStateFnc([](dmn::Dmn_State &) {});
  EXPECT_FALSE(postShutdownState->run());
}
