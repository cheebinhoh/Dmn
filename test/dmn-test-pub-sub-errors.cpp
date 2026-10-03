/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-pub-sub-errors.cpp
 * @brief Verify callback failure recording and notification continuation.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dmn-pub-sub.hpp"

namespace {

using Publisher = dmn::Dmn_Pub<std::string>;

class Receiver : public Publisher::Dmn_Sub {
public:
  explicit Receiver(bool shouldThrow = false) : m_shouldThrow{shouldThrow} {}

  void notify(const std::string &item, Publisher *) override {
    m_received.push_back(item);
    if (m_shouldThrow) {
      throw std::runtime_error{"receiver failure"};
    }
  }

  bool m_shouldThrow{};
  std::vector<std::string> m_received{};
};

class ReplayReceiver : public Publisher::Dmn_Sub {
public:
  explicit ReplayReceiver(ssize_t replayQuantity)
      : Publisher::Dmn_Sub{replayQuantity} {}

  void notify(const std::string &item, Publisher *) override {
    m_received.push_back(item);
  }

  std::vector<std::string> m_received{};
};

struct DerivedPublisherState {
  std::mutex m_mutex{};
  std::condition_variable m_condition{};
  bool m_entered{};
  bool m_released{};
  bool m_derivedStateAlive{true};
};

class DerivedPublisher : public Publisher {
public:
  explicit DerivedPublisher(std::shared_ptr<DerivedPublisherState> state)
      : Publisher{"derived-publisher", 0}, m_state{std::move(state)} {}

  ~DerivedPublisher() override {
    waitForEmpty();

    {
      std::lock_guard lock{m_state->m_mutex};
      m_state->m_derivedStateAlive = false;
    }

    m_state->m_condition.notify_all();
  }

protected:
  void publishInternal(const std::string &item) override {
    {
      std::lock_guard lock{m_state->m_mutex};
      m_state->m_entered = true;
    }

    m_state->m_condition.notify_all();

    {
      std::unique_lock lock{m_state->m_mutex};
      m_state->m_condition.wait(lock, [this] { return m_state->m_released; });
      EXPECT_TRUE(m_state->m_derivedStateAlive);
    }

    Publisher::publishInternal(item);
  }

private:
  std::shared_ptr<DerivedPublisherState> m_state;
};

class NamedReceiver : public Receiver {
public:
  using Receiver::Receiver;

  auto GetName() const noexcept -> std::string_view override {
    return "named receiver";
  }
};

class BlockingReceiver : public Publisher::Dmn_Sub {
public:
  void notify(const std::string &item, Publisher *) override {
    {
      std::lock_guard lock{m_mutex};
      m_received.push_back(item);
      m_entered = true;
    }

    m_condition.notify_all();

    std::unique_lock lock{m_mutex};
    m_condition.wait(lock, [this] { return m_released; });
  }

  void waitUntilEntered() {
    std::unique_lock lock{m_mutex};
    m_condition.wait(lock, [this] { return m_entered; });
  }

  void release() {
    {
      std::lock_guard lock{m_mutex};
      m_released = true;
    }

    m_condition.notify_all();
  }

  auto received() -> std::vector<std::string> {
    std::lock_guard lock{m_mutex};

    return m_received;
  }

private:
  std::mutex m_mutex{};
  std::condition_variable m_condition{};
  bool m_entered{};
  bool m_released{};
  std::vector<std::string> m_received{};
};

void expectFailureMessage(const Publisher::CallbackFailure &failure,
                          const std::string &expected) {
  ASSERT_NE(failure.m_exception, nullptr);
  try {
    std::rethrow_exception(failure.m_exception);
  } catch (const std::runtime_error &error) {
    EXPECT_EQ(error.what(), expected);
  } catch (...) {
    FAIL() << "Recorded callback failure had an unexpected exception type";
  }
}

TEST(PubSubCallbackErrors, RecordsFailuresAndContinuesDelivery) {
  Receiver *filterFailureReceiver{};
  Publisher publisher{
      "callback-errors", 2,
      [&filterFailureReceiver](const Publisher::Dmn_Sub *subscriber,
                               const std::string &) {
        if (subscriber == filterFailureReceiver) {
          throw std::runtime_error{"filter failure"};
        }

        return true;
      }};

  auto throwingReceiver = publisher.registerSubscriber<Receiver>(true);
  auto filterSkippedReceiver = publisher.registerSubscriber<NamedReceiver>();

  filterFailureReceiver = filterSkippedReceiver.get();
  auto laterReceiver = publisher.registerSubscriber<Receiver>();

  EXPECT_THROW(publisher.publish("blocking", true), std::runtime_error);
  EXPECT_EQ(throwingReceiver->m_received,
            (std::vector<std::string>{"blocking"}));
  EXPECT_TRUE(filterSkippedReceiver->m_received.empty());
  EXPECT_EQ(laterReceiver->m_received, (std::vector<std::string>{"blocking"}));

  auto blockingFailures = publisher.takeCallbackFailures();
  ASSERT_EQ(blockingFailures.size(), 2U);
  EXPECT_EQ(blockingFailures[0].m_kind,
            Publisher::CallbackFailureKind::kNotify);
  EXPECT_EQ(blockingFailures[0].m_subscriberName, "unknown");
  expectFailureMessage(blockingFailures[0], "receiver failure");
  EXPECT_EQ(blockingFailures[1].m_kind,
            Publisher::CallbackFailureKind::kFilter);
  EXPECT_EQ(blockingFailures[1].m_subscriberName, "named receiver");
  expectFailureMessage(blockingFailures[1], "filter failure");
  EXPECT_TRUE(publisher.takeCallbackFailures().empty());

  publisher.publish("non-blocking");
  publisher.waitForEmpty();

  auto nonBlockingFailures = publisher.takeCallbackFailures();
  ASSERT_EQ(nonBlockingFailures.size(), 2U);
  EXPECT_EQ(throwingReceiver->m_received.size(), 2U);
  EXPECT_TRUE(filterSkippedReceiver->m_received.empty());
  EXPECT_EQ(laterReceiver->m_received.size(), 2U);
}

TEST(PubSubCallbackErrors, ReplaysAfterFailureAndKeepsRegistrationSuccessful) {
  Publisher publisher{"replay-errors", 2};
  publisher.publish("cached", true);

  auto throwingReceiver = publisher.registerSubscriber<NamedReceiver>(true);
  auto laterReceiver = publisher.registerSubscriber<Receiver>();

  EXPECT_EQ(throwingReceiver->m_received, (std::vector<std::string>{"cached"}));
  EXPECT_EQ(laterReceiver->m_received, (std::vector<std::string>{"cached"}));

  auto failures = publisher.takeCallbackFailures();
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_EQ(failures.front().m_kind, Publisher::CallbackFailureKind::kNotify);
  EXPECT_EQ(failures.front().m_subscriberName, "named receiver");
  expectFailureMessage(failures.front(), "receiver failure");
}

TEST(PubSubCallbackErrors, BoundsStoredFailuresAndEvictsOldest) {
  Publisher publisher{"bounded-errors", 0, {}, 2};
  auto receiver = publisher.registerSubscriber<NamedReceiver>(true);

  EXPECT_THROW(publisher.publish("first", true), std::runtime_error);
  EXPECT_THROW(publisher.publish("second", true), std::runtime_error);
  EXPECT_THROW(publisher.publish("third", true), std::runtime_error);

  auto failures = publisher.takeCallbackFailures();
  ASSERT_EQ(failures.size(), 2U);
  EXPECT_EQ(failures[0].m_subscriberName, "named receiver");
  EXPECT_EQ(failures[1].m_subscriberName, "named receiver");
  expectFailureMessage(failures[0], "receiver failure");
  expectFailureMessage(failures[1], "receiver failure");
  EXPECT_EQ(receiver->m_received,
            (std::vector<std::string>{"first", "second", "third"}));
}

TEST(PubSubCallbackErrors, ZeroCapacityDisablesFailureStorage) {
  Publisher publisher{"no-stored-errors", 0, {}, 0};
  publisher.registerSubscriber<NamedReceiver>(true);

  EXPECT_THROW(publisher.publish("not retained", true), std::runtime_error);

  EXPECT_TRUE(publisher.takeCallbackFailures().empty());
}

TEST(PubSubCallbackErrors, ReplaysConfiguredSuffixIncludingZeroLimit) {
  Publisher publisher{"replay-limits", 3};
  publisher.publish("one", true);
  publisher.publish("two", true);
  publisher.publish("three", true);

  auto replayAll = publisher.registerSubscriber<ReplayReceiver>(-1);
  auto replayNone = publisher.registerSubscriber<ReplayReceiver>(0);
  auto replayLastTwo = publisher.registerSubscriber<ReplayReceiver>(2);

  EXPECT_EQ(replayAll->m_received,
            (std::vector<std::string>{"one", "two", "three"}));
  EXPECT_TRUE(replayNone->m_received.empty());
  EXPECT_EQ(replayLastTwo->m_received,
            (std::vector<std::string>{"two", "three"}));
}

TEST(PubSubCallbackErrors, DuplicateRegistrationDoesNotReplayOrNotifyTwice) {
  Publisher publisher{"duplicate-registration", 2};
  publisher.publish("cached", true);

  auto receiver = std::make_shared<ReplayReceiver>(-1);
  publisher.registerSubscriber(receiver);
  publisher.registerSubscriber(receiver);
  publisher.publish("live", true);

  EXPECT_EQ(receiver->m_received, (std::vector<std::string>{"cached", "live"}));
}

TEST(PubSubCallbackErrors, SlowSubscriberDelaysLaterPublisherWork) {
  Publisher publisher{"slow-subscriber", 0};
  auto slowReceiver = publisher.registerSubscriber<BlockingReceiver>();
  auto laterReceiver = publisher.registerSubscriber<ReplayReceiver>(0);

  publisher.publish("first");
  slowReceiver->waitUntilEntered();

  std::mutex publishMutex;
  std::condition_variable publishCondition;
  bool publishStarted{};
  bool publishComplete{};

  std::thread laterPublisher([&] {
    {
      std::lock_guard lock{publishMutex};
      publishStarted = true;
    }

    publishCondition.notify_all();

    publisher.publish("second", true);

    {
      std::lock_guard lock{publishMutex};
      publishComplete = true;
    }

    publishCondition.notify_all();
  });

  {
    std::unique_lock lock{publishMutex};
    publishCondition.wait(lock, [&publishStarted] { return publishStarted; });

    EXPECT_FALSE(publishCondition.wait_for(
        lock, std::chrono::milliseconds{20},
        [&publishComplete] { return publishComplete; }));
  }

  slowReceiver->release();
  laterPublisher.join();

  EXPECT_TRUE(publishComplete);
  EXPECT_EQ(slowReceiver->received(),
            (std::vector<std::string>{"first", "second"}));
  EXPECT_EQ(laterReceiver->m_received,
            (std::vector<std::string>{"first", "second"}));
}

TEST(PubSubCallbackErrors,
     SubscriberDestructionWaitsForEarlierQueuedPublication) {
  Publisher publisher{"subscriber-destruction", 0};
  auto receiver = publisher.registerSubscriber<BlockingReceiver>();
  auto receiverWeak = std::weak_ptr<BlockingReceiver>{receiver};
  auto receiverRaw = receiver.get();

  publisher.publish("in-flight");
  receiver->waitUntilEntered();
  receiver.reset();
  EXPECT_FALSE(receiverWeak.expired());

  std::mutex unregisterMutex;
  std::condition_variable unregisterCondition;
  bool unregisterStarted{};
  bool unregisterComplete{};

  std::thread unregisterer([&] {
    {
      std::lock_guard lock{unregisterMutex};
      unregisterStarted = true;
    }

    unregisterCondition.notify_all();

    publisher.unregisterSubscriber(receiverRaw);

    {
      std::lock_guard lock{unregisterMutex};
      unregisterComplete = true;
    }

    unregisterCondition.notify_all();
  });

  {
    std::unique_lock lock{unregisterMutex};
    unregisterCondition.wait(
        lock, [&unregisterStarted] { return unregisterStarted; });
    EXPECT_FALSE(unregisterCondition.wait_for(
        lock, std::chrono::milliseconds{20},
        [&unregisterComplete] { return unregisterComplete; }));
  }

  receiverRaw->release();
  unregisterer.join();

  EXPECT_TRUE(unregisterComplete);
  EXPECT_TRUE(receiverWeak.expired());
}

TEST(PubSubCallbackErrors, PublisherDestructionWaitsForQueuedNotification) {
  auto publisher = std::make_unique<Publisher>("publisher-destruction", 0);
  auto receiver = publisher->registerSubscriber<BlockingReceiver>();
  auto receiverWeak = std::weak_ptr<BlockingReceiver>{receiver};
  publisher->publish("in-flight");
  receiver->waitUntilEntered();

  std::mutex destructionMutex;
  std::condition_variable destructionCondition;
  bool destructionStarted{};
  bool destructionComplete{};

  std::thread destroyer([publisher = std::move(publisher), &destructionMutex,
                         &destructionCondition, &destructionStarted,
                         &destructionComplete]() mutable {
    {
      std::lock_guard lock{destructionMutex};
      destructionStarted = true;
    }

    destructionCondition.notify_all();
    publisher.reset();

    {
      std::lock_guard lock{destructionMutex};
      destructionComplete = true;
    }

    destructionCondition.notify_all();
  });

  {
    std::unique_lock lock{destructionMutex};
    destructionCondition.wait(
        lock, [&destructionStarted] { return destructionStarted; });
    EXPECT_FALSE(destructionCondition.wait_for(
        lock, std::chrono::milliseconds{20},
        [&destructionComplete] { return destructionComplete; }));
  }

  receiver->release();
  destroyer.join();

  EXPECT_TRUE(destructionComplete);
  EXPECT_EQ(receiver->received(), (std::vector<std::string>{"in-flight"}));
  receiver.reset();
  EXPECT_TRUE(receiverWeak.expired());
}

TEST(PubSubCallbackErrors, DerivedPublisherDrainsBeforeDestroyingDerivedState) {
  auto state = std::make_shared<DerivedPublisherState>();
  auto publisher = std::make_unique<DerivedPublisher>(state);
  auto receiver = publisher->registerSubscriber<Receiver>();
  publisher->publish("in-flight");

  {
    std::unique_lock lock{state->m_mutex};
    state->m_condition.wait(lock, [&state] { return state->m_entered; });
  }

  std::mutex destructionMutex;
  std::condition_variable destructionCondition;
  bool destructionStarted{};
  bool destructionComplete{};
  std::thread destroyer([publisher = std::move(publisher), &destructionMutex,
                         &destructionCondition, &destructionStarted,
                         &destructionComplete]() mutable {
    {
      std::lock_guard lock{destructionMutex};
      destructionStarted = true;
    }

    destructionCondition.notify_all();
    publisher.reset();

    {
      std::lock_guard lock{destructionMutex};
      destructionComplete = true;
    }

    destructionCondition.notify_all();
  });

  {
    std::unique_lock lock{destructionMutex};
    destructionCondition.wait(
        lock, [&destructionStarted] { return destructionStarted; });
    EXPECT_FALSE(destructionCondition.wait_for(
        lock, std::chrono::milliseconds{20},
        [&destructionComplete] { return destructionComplete; }));
  }

  {
    std::lock_guard lock{state->m_mutex};
    state->m_released = true;
  }

  state->m_condition.notify_all();
  destroyer.join();

  EXPECT_TRUE(destructionComplete);
  EXPECT_FALSE(state->m_derivedStateAlive);
  EXPECT_EQ(receiver->m_received, (std::vector<std::string>{"in-flight"}));
}

} // namespace
