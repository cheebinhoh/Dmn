/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesg-conflict.cpp
 * @brief Deterministic regression coverage for handler-scoped DMesg conflicts.
 */

#include "dmn-dmesg.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class HandlerEventCollector {
public:
  void add(const dmn::Dmn_DMesg::HandlerEvent &event) {
    const std::lock_guard lock{m_mutex};
    m_events.push_back(event);
  }

  auto snapshot() const -> std::vector<dmn::Dmn_DMesg::HandlerEvent> {
    const std::lock_guard lock{m_mutex};
    return m_events;
  }

private:
  mutable std::mutex m_mutex{};
  std::vector<dmn::Dmn_DMesg::HandlerEvent> m_events{};
};

template <typename HandlerProxy>
auto snapshotEvents(const HandlerProxy &handler,
                    const HandlerEventCollector &collector)
    -> std::vector<dmn::Dmn_DMesg::HandlerEvent> {
  // This synchronous handler query drains callback tasks already queued ahead
  // of it on the handler's async context.
  static_cast<void>(handler->isInConflict());
  return collector.snapshot();
}

auto makeMessage(std::string body) -> dmn::DMesgPb {
  dmn::DMesgPb message;
  message.set_topic("orders");
  message.set_type(dmn::DMesgTypePb::message);
  message.mutable_body()->set_message(std::move(body));
  return message;
}

auto openObservedHandler(dmn::Dmn_DMesg &publisher, std::string_view name,
                         HandlerEventCollector &events)
    -> dmn::Dmn_DMesg::HandlerType {
  dmn::Dmn_DMesg::HandlerSpec spec{
      name,
      "orders",
      {},
      {},
      {},
      [&events](const dmn::Dmn_DMesg::HandlerEvent &event) {
        events.add(event);
      }};

  return publisher.openHandlerWithFactory(
      spec, [](const dmn::Dmn_DMesg::HandlerSpec &handlerSpec) {
        return std::make_shared<dmn::Dmn_DMesg::Dmn_DMesgHandler>(
            handlerSpec.m_name, handlerSpec.m_topic, handlerSpec.m_filter_fn,
            handlerSpec.m_async_process_fn, handlerSpec.m_configs);
      });
}

TEST(DmnDMesgConflictTest, AppliesConfiguredTopicBeforeIncrementingItsCounter) {
  dmn::Dmn_DMesg publisher{"configured-topic-counter-test"};
  std::promise<void> publisherPaused;
  std::promise<void> resumePublisher;
  auto resumePublisherFuture = resumePublisher.get_future().share();
  auto writer = publisher.openHandler("writer", "orders");
  // Hold publisher delivery so the writer can issue its next write first.
  auto observer = publisher.openHandler(
      "observer", "orders",
      [&publisherPaused, &resumePublisherFuture](const dmn::DMesgPb &message) {
        if ("first" == message.body().message()) {
          publisherPaused.set_value();
          resumePublisherFuture.wait();
        }

        return true;
      });
  ASSERT_TRUE(writer);
  ASSERT_TRUE(observer);

  auto firstMessage = makeMessage("first");
  firstMessage.clear_topic();
  writer->write(firstMessage);

  const auto pauseStatus = publisherPaused.get_future().wait_for(5s);
  if (std::future_status::ready != pauseStatus) {
    resumePublisher.set_value();
    FAIL() << "publisher did not pause on first message";
    return;
  }

  writer->write(makeMessage("second"));
  EXPECT_EQ(writer->getTopicRunningCounter("orders"), 2U);

  resumePublisher.set_value();
  publisher.waitForEmpty();
  EXPECT_FALSE(writer->isInConflict("orders"));
  EXPECT_EQ(writer->getTopicRunningCounter("orders"), 2U);

  publisher.closeHandler(writer);
  publisher.closeHandler(observer);
}

TEST(DmnDMesgConflictTest,
     NotifiesOnlyHandlersWithAnEstablishedMatchingTopicCounter) {
  dmn::Dmn_DMesg publisher{"conflict-test"};
  auto writer = publisher.openHandler("writer", "orders");
  auto observer = publisher.openHandler("observer", "orders");
  auto filteredObserver =
      publisher.openHandler("filtered-observer", "orders",
                            [](const dmn::DMesgPb &) { return false; });

  ASSERT_TRUE(writer);
  ASSERT_TRUE(observer);
  ASSERT_TRUE(filteredObserver);

  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("initial")));
  publisher.waitForEmpty();

  ASSERT_EQ(observer->getTopicRunningCounter("orders"), 1U);
  ASSERT_EQ(filteredObserver->getTopicRunningCounter("orders"), 0U);

  std::promise<dmn::DMesgPb> writerConflict;
  std::promise<dmn::DMesgPb> observerConflict;
  auto writerConflictResult = writerConflict.get_future();
  auto observerConflictResult = observerConflict.get_future();
  std::atomic<unsigned int> filteredObserverConflictCount{};

  writer->setConflictCallbackTask(
      [&writerConflict](dmn::Dmn_DMesg::Dmn_DMesgHandler &,
                        const dmn::DMesgPb &message) {
        writerConflict.set_value(message);
      });
  observer->setConflictCallbackTask(
      [&observerConflict](dmn::Dmn_DMesg::Dmn_DMesgHandler &,
                          const dmn::DMesgPb &message) {
        observerConflict.set_value(message);
      });
  filteredObserver->setConflictCallbackTask(
      [&filteredObserverConflictCount](dmn::Dmn_DMesg::Dmn_DMesgHandler &,
                                       const dmn::DMesgPb &) {
        ++filteredObserverConflictCount;
      });

  writer->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(writer->writeAndCheckConflict(makeMessage("stale")));
  publisher.waitForEmpty();

  ASSERT_EQ(writerConflictResult.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(observerConflictResult.wait_for(5s), std::future_status::ready);
  const auto writerConflictMessage = writerConflictResult.get();
  const auto observerConflictMessage = observerConflictResult.get();
  EXPECT_TRUE(writerConflictMessage.conflict());
  EXPECT_TRUE(observerConflictMessage.conflict());
  EXPECT_EQ(writerConflictMessage.topic(), "orders");
  EXPECT_EQ(observerConflictMessage.topic(), "orders");
  EXPECT_EQ(writerConflictMessage.body().message(), "stale");
  EXPECT_EQ(observerConflictMessage.body().message(), "stale");

  EXPECT_TRUE(writer->isInConflict("orders"));
  EXPECT_TRUE(observer->isInConflict("orders"));
  EXPECT_FALSE(filteredObserver->isInConflict("orders"));
  EXPECT_EQ(filteredObserverConflictCount.load(), 0U);

  publisher.resetConflictStateWithLastTopicMessage("orders");
  publisher.waitForEmpty();
  EXPECT_FALSE(writer->isInConflict("orders"));
  EXPECT_FALSE(observer->isInConflict("orders"));

  publisher.closeHandler(writer);
  publisher.closeHandler(observer);
  publisher.closeHandler(filteredObserver);
}

TEST(DmnDMesgConflictTest,
     ObservesOrderedMessageAndConflictStateTransitionsForSameTopic) {
  dmn::Dmn_DMesg publisher{"handler-event-test"};
  HandlerEventCollector writerEvents;
  HandlerEventCollector observerEvents;
  auto writer = openObservedHandler(publisher, "writer", writerEvents);
  auto observer = openObservedHandler(publisher, "observer", observerEvents);

  ASSERT_TRUE(writer);
  ASSERT_TRUE(observer);
  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("initial")));
  publisher.waitForEmpty();

  auto observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  ASSERT_EQ(observerEventsSnapshot.size(), 1U);
  EXPECT_EQ(observerEventsSnapshot[0].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kMessage);
  EXPECT_EQ(observerEventsSnapshot[0].m_topic, "orders");
  EXPECT_EQ(observerEventsSnapshot[0].m_handler_running_counter, 1U);
  EXPECT_EQ(observerEventsSnapshot[0].m_conflict_generation, 0U);
  ASSERT_TRUE(observerEventsSnapshot[0].m_message.has_value());
  EXPECT_EQ(observerEventsSnapshot[0].m_message->body().message(), "initial");

  writer->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(writer->writeAndCheckConflict(makeMessage("stale")));
  publisher.waitForEmpty();

  auto writerEventsSnapshot = snapshotEvents(writer, writerEvents);
  observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  ASSERT_EQ(writerEventsSnapshot.size(), 1U);
  ASSERT_EQ(observerEventsSnapshot.size(), 2U);
  for (const auto *events : {&writerEventsSnapshot, &observerEventsSnapshot}) {
    const auto &event = events->back();
    EXPECT_EQ(event.m_type, dmn::Dmn_DMesg::HandlerEventType::kConflictEntered);
    EXPECT_EQ(event.m_topic, "orders");
    EXPECT_EQ(event.m_conflict_generation, 1U);
    ASSERT_TRUE(event.m_message.has_value());
    EXPECT_TRUE(event.m_message->conflict());
    EXPECT_EQ(event.m_message->body().message(), "stale");
  }

  publisher.resetConflictStateWithLastTopicMessage("orders");
  writerEventsSnapshot = snapshotEvents(writer, writerEvents);
  observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  ASSERT_EQ(writerEventsSnapshot.size(), 2U);
  ASSERT_EQ(observerEventsSnapshot.size(), 3U);
  for (const auto *events : {&writerEventsSnapshot, &observerEventsSnapshot}) {
    const auto &event = events->back();
    EXPECT_EQ(event.m_type,
              dmn::Dmn_DMesg::HandlerEventType::kConflictResolved);
    EXPECT_EQ(event.m_topic, "orders");
    EXPECT_EQ(event.m_handler_running_counter, 1U);
    EXPECT_EQ(event.m_conflict_generation, 2U);
    ASSERT_TRUE(event.m_message.has_value());
    EXPECT_TRUE(event.m_message->force());
    EXPECT_EQ(event.m_message->body().message(), "initial");
  }

  writer->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(writer->writeAndCheckConflict(makeMessage("stale-again")));
  publisher.waitForEmpty();
  writerEventsSnapshot = snapshotEvents(writer, writerEvents);
  observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  ASSERT_EQ(writerEventsSnapshot.size(), 3U);
  ASSERT_EQ(observerEventsSnapshot.size(), 4U);

  writer->resolveConflict("orders");
  publisher.waitForEmpty();
  writerEventsSnapshot = snapshotEvents(writer, writerEvents);
  EXPECT_EQ(writerEventsSnapshot.size(), 4U);
  EXPECT_EQ(writerEventsSnapshot.back().m_type,
            dmn::Dmn_DMesg::HandlerEventType::kConflictResolved);
  EXPECT_EQ(writerEventsSnapshot.back().m_conflict_generation, 4U);
  EXPECT_FALSE(writerEventsSnapshot.back().m_message.has_value());

  auto accepted = makeMessage("newer");
  EXPECT_TRUE(writer->writeAndCheckConflict(accepted));
  publisher.waitForEmpty();
  observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  ASSERT_EQ(observerEventsSnapshot.size(), 6U);
  EXPECT_EQ(observerEventsSnapshot[4].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kConflictResolved);
  EXPECT_EQ(observerEventsSnapshot[4].m_conflict_generation, 4U);
  ASSERT_TRUE(observerEventsSnapshot[4].m_message.has_value());
  EXPECT_EQ(observerEventsSnapshot[4].m_message->body().message(), "newer");
  EXPECT_EQ(observerEventsSnapshot[5].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kMessage);
  EXPECT_EQ(observerEventsSnapshot[5].m_conflict_generation, 4U);
  ASSERT_TRUE(observerEventsSnapshot[5].m_message.has_value());
  EXPECT_EQ(observerEventsSnapshot[5].m_message->body().message(), "newer");

  publisher.closeHandler(writer);
  publisher.closeHandler(observer);
}

TEST(DmnDMesgConflictTest, RepeatedConflictDoesNotEmitAnotherStateTransition) {
  dmn::Dmn_DMesg publisher{"repeated-conflict-event-test"};
  HandlerEventCollector firstWriterEvents;
  HandlerEventCollector secondWriterEvents;
  HandlerEventCollector observerEvents;
  auto firstWriter =
      openObservedHandler(publisher, "first-writer", firstWriterEvents);
  auto observer = openObservedHandler(publisher, "observer", observerEvents);

  ASSERT_TRUE(firstWriter);
  ASSERT_TRUE(observer);
  ASSERT_TRUE(firstWriter->writeAndCheckConflict(makeMessage("initial")));
  publisher.waitForEmpty();

  firstWriter->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(firstWriter->writeAndCheckConflict(makeMessage("first-stale")));

  auto secondWriter =
      openObservedHandler(publisher, "second-writer", secondWriterEvents);
  ASSERT_TRUE(secondWriter);
  secondWriter->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(
      secondWriter->writeAndCheckConflict(makeMessage("second-stale")));
  publisher.waitForEmpty();

  auto observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  auto firstWriterEventsSnapshot =
      snapshotEvents(firstWriter, firstWriterEvents);
  auto secondWriterEventsSnapshot =
      snapshotEvents(secondWriter, secondWriterEvents);
  ASSERT_EQ(observerEventsSnapshot.size(), 2U);
  EXPECT_EQ(observerEventsSnapshot[0].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kMessage);
  EXPECT_EQ(observerEventsSnapshot[1].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kConflictEntered);
  EXPECT_EQ(observerEventsSnapshot[1].m_conflict_generation, 1U);
  EXPECT_EQ(firstWriterEventsSnapshot.size(), 1U);
  ASSERT_EQ(secondWriterEventsSnapshot.size(), 2U);
  EXPECT_EQ(secondWriterEventsSnapshot.back().m_type,
            dmn::Dmn_DMesg::HandlerEventType::kConflictEntered);

  publisher.closeHandler(firstWriter);
  publisher.closeHandler(secondWriter);
  publisher.closeHandler(observer);
}

TEST(DmnDMesgConflictTest, InitialPlaybackEventPrecedesSubsequentDelivery) {
  dmn::Dmn_DMesg publisher{"initial-playback-event-test"};
  auto writer = publisher.openHandler("writer", "orders");
  ASSERT_TRUE(writer);
  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("cached")));

  HandlerEventCollector events;
  auto observer = openObservedHandler(publisher, "observer", events);
  ASSERT_TRUE(observer);

  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("newer")));
  publisher.waitForEmpty();

  const auto eventsSnapshot = snapshotEvents(observer, events);
  ASSERT_EQ(eventsSnapshot.size(), 2U);
  EXPECT_EQ(eventsSnapshot[0].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kMessage);
  EXPECT_EQ(eventsSnapshot[0].m_topic, "orders");
  ASSERT_TRUE(eventsSnapshot[0].m_message.has_value());
  EXPECT_TRUE(eventsSnapshot[0].m_message->playback());
  EXPECT_EQ(eventsSnapshot[0].m_message->body().message(), "cached");
  EXPECT_EQ(eventsSnapshot[1].m_type,
            dmn::Dmn_DMesg::HandlerEventType::kMessage);
  ASSERT_TRUE(eventsSnapshot[1].m_message.has_value());
  EXPECT_FALSE(eventsSnapshot[1].m_message->playback());
  EXPECT_EQ(eventsSnapshot[1].m_message->body().message(), "newer");

  publisher.closeHandler(observer);
  publisher.closeHandler(writer);
}

TEST(DmnDMesgConflictTest, ForcedHandlerWriteEmitsRepairTransition) {
  dmn::Dmn_DMesg publisher{"forced-handler-event-test"};
  HandlerEventCollector writerEvents;
  HandlerEventCollector observerEvents;
  auto writer = openObservedHandler(publisher, "writer", writerEvents);
  auto observer = openObservedHandler(publisher, "observer", observerEvents);

  ASSERT_TRUE(writer);
  ASSERT_TRUE(observer);
  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("initial")));
  publisher.waitForEmpty();
  writer->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(writer->writeAndCheckConflict(makeMessage("stale")));
  publisher.waitForEmpty();

  auto repair = makeMessage("forced-repair");
  dmn::Dmn_DMesg::Dmn_DMesgHandler::WriteFlags flags;
  flags.set(dmn::Dmn_DMesg::Dmn_DMesgHandler::kForce);
  writer->write(repair, flags);

  auto writerEventsSnapshot = snapshotEvents(writer, writerEvents);
  auto observerEventsSnapshot = snapshotEvents(observer, observerEvents);
  ASSERT_EQ(writerEventsSnapshot.size(), 2U);
  EXPECT_EQ(writerEventsSnapshot.back().m_type,
            dmn::Dmn_DMesg::HandlerEventType::kConflictResolved);
  EXPECT_EQ(writerEventsSnapshot.back().m_conflict_generation, 2U);
  ASSERT_TRUE(writerEventsSnapshot.back().m_message.has_value());
  EXPECT_TRUE(writerEventsSnapshot.back().m_message->force());
  EXPECT_EQ(writerEventsSnapshot.back().m_message->body().message(),
            "forced-repair");

  ASSERT_EQ(observerEventsSnapshot.size(), 3U);
  EXPECT_EQ(observerEventsSnapshot.back().m_type,
            dmn::Dmn_DMesg::HandlerEventType::kConflictResolved);
  EXPECT_EQ(observerEventsSnapshot.back().m_conflict_generation, 2U);
  ASSERT_TRUE(observerEventsSnapshot.back().m_message.has_value());
  EXPECT_TRUE(observerEventsSnapshot.back().m_message->force());
  EXPECT_EQ(observerEventsSnapshot.back().m_message->body().message(),
            "forced-repair");
  EXPECT_FALSE(writer->isInConflict("orders"));

  publisher.closeHandler(writer);
  publisher.closeHandler(observer);
}

TEST(DmnDMesgConflictTest,
     ProcessingCallbacksRunOnHandlerContextInPublisherEventOrder) {
  dmn::Dmn_DMesg publisher{"handler-callback-context-test"};
  auto writer = publisher.openHandler("writer", "orders");
  ASSERT_TRUE(writer);

  std::promise<std::thread::id> filterThread;
  std::promise<std::thread::id> messageEventThread;
  std::promise<std::thread::id> asyncProcessThread;
  std::promise<std::thread::id> conflictEventThread;
  std::promise<std::thread::id> conflictCallbackThread;
  std::promise<int> messageEventOrder;
  std::promise<int> asyncProcessOrder;
  auto filterThreadResult = filterThread.get_future();
  auto messageEventThreadResult = messageEventThread.get_future();
  auto asyncProcessThreadResult = asyncProcessThread.get_future();
  auto conflictEventThreadResult = conflictEventThread.get_future();
  auto conflictCallbackThreadResult = conflictCallbackThread.get_future();
  auto messageEventOrderResult = messageEventOrder.get_future();
  auto asyncProcessOrderResult = asyncProcessOrder.get_future();
  std::atomic<int> callbackOrder{};

  dmn::Dmn_DMesg::HandlerSpec spec{
      "observer",
      "orders",
      [&filterThread](const dmn::DMesgPb &) {
        filterThread.set_value(std::this_thread::get_id());
        return true;
      },
      [&asyncProcessThread, &asyncProcessOrder, &callbackOrder](dmn::DMesgPb) {
        asyncProcessThread.set_value(std::this_thread::get_id());
        asyncProcessOrder.set_value(++callbackOrder);
      },
      {},
      [&messageEventThread, &messageEventOrder, &conflictEventThread,
       &callbackOrder](const dmn::Dmn_DMesg::HandlerEvent &event) {
        if (event.m_type == dmn::Dmn_DMesg::HandlerEventType::kMessage) {
          messageEventThread.set_value(std::this_thread::get_id());
          messageEventOrder.set_value(++callbackOrder);
        } else if (event.m_type ==
                   dmn::Dmn_DMesg::HandlerEventType::kConflictEntered) {
          conflictEventThread.set_value(std::this_thread::get_id());
        }
      }};
  auto observer = publisher.openHandler(spec);
  ASSERT_TRUE(observer);

  observer->setConflictCallbackTask(
      [&conflictCallbackThread](dmn::Dmn_DMesg::Dmn_DMesgHandler &,
                                const dmn::DMesgPb &) {
        conflictCallbackThread.set_value(std::this_thread::get_id());
      });

  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("initial")));
  publisher.waitForEmpty();
  static_cast<void>(observer->isInConflict());

  ASSERT_EQ(filterThreadResult.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(messageEventThreadResult.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(asyncProcessThreadResult.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(messageEventOrderResult.get(), 1);
  ASSERT_EQ(asyncProcessOrderResult.get(), 2);

  const auto publisherContext = filterThreadResult.get();
  const auto messageEventContext = messageEventThreadResult.get();
  const auto asyncProcessContext = asyncProcessThreadResult.get();
  EXPECT_NE(publisherContext, messageEventContext);
  EXPECT_EQ(messageEventContext, asyncProcessContext);

  writer->setTopicRunningCounter("orders", 0U);
  EXPECT_FALSE(writer->writeAndCheckConflict(makeMessage("stale")));
  publisher.waitForEmpty();
  static_cast<void>(observer->isInConflict());

  ASSERT_EQ(conflictEventThreadResult.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(conflictCallbackThreadResult.wait_for(5s),
            std::future_status::ready);
  EXPECT_EQ(conflictEventThreadResult.get(), asyncProcessContext);
  EXPECT_EQ(conflictCallbackThreadResult.get(), asyncProcessContext);

  publisher.closeHandler(observer);
  publisher.closeHandler(writer);
}

TEST(DmnDMesgConflictTest, ObserverExceptionDoesNotInterruptDelivery) {
  dmn::Dmn_DMesg publisher{"throwing-handler-event-test"};
  auto writer = publisher.openHandler("writer", "orders");
  ASSERT_TRUE(writer);

  std::promise<void> observerCallbackRan;
  auto observerCallbackResult = observerCallbackRan.get_future();
  dmn::Dmn_DMesg::HandlerSpec spec{
      "observer",
      "orders",
      {},
      {},
      {},
      [&observerCallbackRan](const dmn::Dmn_DMesg::HandlerEvent &) {
        observerCallbackRan.set_value();
        throw std::runtime_error("observer failure");
      }};
  auto observer = publisher.openHandlerWithFactory(
      spec, [](const dmn::Dmn_DMesg::HandlerSpec &handlerSpec) {
        return std::make_shared<dmn::Dmn_DMesg::Dmn_DMesgHandler>(
            handlerSpec.m_name, handlerSpec.m_topic, handlerSpec.m_filter_fn,
            handlerSpec.m_async_process_fn, handlerSpec.m_configs);
      });
  ASSERT_TRUE(observer);

  ASSERT_TRUE(writer->writeAndCheckConflict(makeMessage("delivered")));
  publisher.waitForEmpty();
  auto received = observer->read();
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->body().message(), "delivered");
  ASSERT_EQ(observerCallbackResult.wait_for(5s), std::future_status::ready);

  publisher.closeHandler(observer);
  publisher.closeHandler(writer);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  google::protobuf::ShutdownProtobufLibrary();
  return result;
}
