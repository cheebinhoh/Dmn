/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dbus-facade.cpp
 * @brief Private-bus tests for the Dmn_DMesgDbus composition facade.
 */

#include "dmn-test-dbus-support.hpp"
#include "dmn.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

using namespace std::chrono_literals;
using dmn_test_dbus::PrivateBus;

static_assert(!std::is_base_of_v<dmn::Dmn_DMesgNet, dmn::Dmn_DMesgDbus>);
static_assert(!std::is_copy_constructible_v<dmn::Dmn_DMesgDbus>);
static_assert(!std::is_move_constructible_v<dmn::Dmn_DMesgDbus>);

template <typename Predicate>
auto waitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = 5s) -> bool {
  return dmn_test_dbus::waitUntil(std::move(predicate), timeout);
}

auto makeConfig() -> dmn::Dmn_DbusConfig {
  dmn::Dmn_DbusConfig config{};
  config.signal_path = "/org/example/DmesgFacade";
  config.signal_interface = "org.example.DmesgFacade";
  config.signal_member = "Message";

  return config;
}

class BlockingInput final : public dmn::Dmn_Io<std::string> {
public:
  auto read() -> std::optional<std::string> override {
    std::unique_lock lock{m_mutex};
    m_stopped.wait(lock, [this] { return m_shutdown; });
    return std::nullopt;
  }

  void write([[maybe_unused]] const std::string &item) override {}
  void write([[maybe_unused]] std::string &&item) override {}

  void shutdown() override {
    {
      std::lock_guard lock{m_mutex};
      m_shutdown = true;
    }
    m_stopped.notify_all();
  }

private:
  std::condition_variable m_stopped;
  std::mutex m_mutex;
  bool m_shutdown{};
};

class FailingOutput final : public dmn::Dmn_Io<std::string> {
public:
  auto read() -> std::optional<std::string> override { return std::nullopt; }

  void write([[maybe_unused]] const std::string &item) override {
    ++m_attempts;
    throw std::runtime_error{"simulated output failure"};
  }

  void write([[maybe_unused]] std::string &&item) override {
    ++m_attempts;
    throw std::runtime_error{"simulated output failure"};
  }

  [[nodiscard]] auto attempts() const -> unsigned int { return m_attempts; }

private:
  std::atomic<unsigned int> m_attempts{};
};

class RecordingOutput final : public dmn::Dmn_Io<std::string> {
public:
  auto read() -> std::optional<std::string> override { return std::nullopt; }

  void write([[maybe_unused]] const std::string &item) override { ++m_writes; }

  void write([[maybe_unused]] std::string &&item) override { ++m_writes; }

  [[nodiscard]] auto writes() const -> unsigned int { return m_writes; }

private:
  std::atomic<unsigned int> m_writes{};
};

TEST(DmnDbusFacadeTest, RejectsInvalidConfigAndExplicitAddress) {
  auto config{makeConfig()};
  config.signal_path = "invalid/path";
  EXPECT_THROW((dmn::Dmn_DMesgDbus{"invalid-config", config}),
               std::invalid_argument);

  config = makeConfig();
  config.bus_address = "not-a-dbus-address";
  EXPECT_THROW((dmn::Dmn_DMesgDbus{"invalid-address", config}),
               std::system_error);
}

TEST(DmnDbusFacadeTest, ForwardsHandlerTopicAndStatusOperations) {
  dmn::Dmn_DMesgDbus node{"facade-api", makeConfig()};
  const auto initialStatus{node.status()};
  EXPECT_FALSE(initialStatus.input.terminal_error);
  EXPECT_FALSE(initialStatus.output.terminal_error);
  EXPECT_FALSE(node.getTopicLastMessage("facade-topic"));

  std::atomic<unsigned int> filterCalls{};
  std::promise<std::string> asyncMessage;
  auto asyncMessageFuture{asyncMessage.get_future()};
  std::promise<dmn::Dmn_DMesgDbus::HandlerEvent> handlerEvent;
  auto handlerEventFuture{handlerEvent.get_future()};
  auto receiverSpec{dmn::Dmn_DMesgDbus::HandlerSpec{
      "facade-receiver",
      "facade-topic",
      [&filterCalls](const dmn::DMesgPb &) {
        ++filterCalls;
        return true;
      },
      [&asyncMessage](dmn::DMesgPb message) {
        asyncMessage.set_value(message.body().message());
      },
      {},
      [&handlerEvent](const dmn::Dmn_DMesgDbus::HandlerEvent &event) {
        if (event.m_type == dmn::Dmn_DMesgDbus::HandlerEventType::kMessage) {
          handlerEvent.set_value(event);
        }
      }}};
  auto receiver{node.openHandler(receiverSpec)};
  ASSERT_TRUE(receiver);

  auto sender{node.openHandler(
      dmn::Dmn_DMesgDbus::HandlerSpec{"facade-sender", "facade-topic"})};
  ASSERT_TRUE(sender);

  dmn::DMesgPb message;
  message.set_type(dmn::DMesgTypePb::message);
  message.mutable_body()->set_message("facade-local");
  sender->write(message);

  ASSERT_EQ(asyncMessageFuture.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(asyncMessageFuture.get(), "facade-local");
  EXPECT_EQ(filterCalls.load(), 1U);
  ASSERT_EQ(handlerEventFuture.wait_for(5s), std::future_status::ready);
  const auto deliveredEvent{handlerEventFuture.get()};
  EXPECT_EQ(deliveredEvent.m_topic, "facade-topic");
  ASSERT_TRUE(deliveredEvent.m_message.has_value());
  EXPECT_EQ(deliveredEvent.m_message->body().message(), "facade-local");

  std::optional<dmn::DMesgPb> lastMessage{};
  ASSERT_TRUE(waitUntil([&node, &lastMessage] {
    lastMessage = node.getTopicLastMessage("facade-topic");

    return lastMessage.has_value();
  }));
  ASSERT_TRUE(lastMessage);
  EXPECT_EQ(lastMessage->topic(), "facade-topic");
  EXPECT_EQ(lastMessage->body().message(), "facade-local");

  EXPECT_NO_THROW(node.resetConflictStateWithLastTopicMessage("facade-topic"));
  const auto activeStatus{node.status()};
  EXPECT_GT(activeStatus.output.messages_written, 0U);

  node.closeHandler(receiver);
  EXPECT_FALSE(receiver);
  node.closeHandler(sender);
  EXPECT_FALSE(sender);
}

TEST(DmnDbusFacadeTest, FactoryForwardingPreservesHandlerSpec) {
  dmn::Dmn_DMesgDbus node{"facade-factory", makeConfig()};
  const dmn::Dmn_DMesgDbus::HandlerSpec spec{
      "factory-handler", "factory-topic", {}, {}, {{"custom", "value"}}};
  bool factoryCalled{};

  auto handler{node.openHandlerWithFactory(
      spec, [&factoryCalled](const dmn::Dmn_DMesgDbus::HandlerSpec &received) {
        factoryCalled = true;
        EXPECT_EQ(received.m_name, "factory-handler");
        EXPECT_EQ(received.m_topic, "factory-topic");
        EXPECT_EQ(received.m_configs.at("custom"), "value");

        return std::make_shared<dmn::Dmn_DMesg::Dmn_DMesgHandler>(
            received.m_name, received.m_topic, received.m_filter_fn,
            received.m_async_process_fn, received.m_configs);
      })};

  EXPECT_TRUE(factoryCalled);
  ASSERT_TRUE(handler);
  node.closeHandler(handler);
  EXPECT_FALSE(handler);

  EXPECT_THROW(node.openHandlerWithFactory(
                   spec,
                   [](const dmn::Dmn_DMesgDbus::HandlerSpec &)
                       -> std::shared_ptr<dmn::Dmn_DMesg::Dmn_DMesgHandler> {
                     throw std::runtime_error("factory failure");
                   }),
               std::runtime_error);
}

TEST(DmnDbusFacadeTest, DestructionSendsFinalHeartbeatBeforeReleasingOutput) {
  const auto config{makeConfig()};
  auto monitor{std::make_shared<dmn::Dmn_DbusInput>(config)};
  auto facade{std::make_unique<dmn::Dmn_DMesgDbus>("facade-shutdown", config)};
  std::atomic<bool> stopping{};
  std::atomic<bool> sawDestroyedHeartbeat{};
  std::atomic<bool> readerFailed{};

  std::thread reader{
      [&monitor, &stopping, &sawDestroyedHeartbeat, &readerFailed] {
        try {
          while (const auto payload = monitor->read()) {
            dmn::DMesgPb message;
            if (message.ParseFromString(*payload) &&
                message.type() == dmn::DMesgTypePb::sys &&
                message.body().sys().self().state() ==
                    dmn::DMesgStatePb::Destroyed) {
              sawDestroyedHeartbeat = true;

              break;
            }
          }
        } catch (const std::system_error &) {
          if (!stopping) {
            readerFailed = true;
          }
        }
      }};

  const bool receivedInitialHeartbeat{waitUntil(
      [&monitor] { return monitor->status().messages_received > 0; }, 20s)};
  if (!receivedInitialHeartbeat) {
    stopping = true;
    monitor->shutdown();
    reader.join();

    FAIL() << "private-bus monitor did not receive an initial heartbeat";

    return;
  }

  facade.reset();
  const bool receivedFinalHeartbeat{waitUntil(
      [&sawDestroyedHeartbeat] { return sawDestroyedHeartbeat.load(); })};

  stopping = true;
  monitor->shutdown();
  reader.join();

  EXPECT_TRUE(receivedFinalHeartbeat);
  EXPECT_FALSE(readerFailed);
}

TEST(DmnDbusFacadeTest, ExchangesDmesgWithOptionAEndpointPeer) {
  const auto config{makeConfig()};
  auto peerInput{std::make_shared<dmn::Dmn_DbusInput>(config)};
  auto peerOutput{std::make_shared<dmn::Dmn_DbusOutput>(config)};
  dmn::Dmn_DMesgNet peer{"option-a-peer", peerInput, peerOutput};
  dmn::Dmn_DMesgDbus facade{"option-b-facade", config};

  ASSERT_TRUE(waitUntil(
      [&peerInput, &facade] {
        return peerInput->status().messages_received > 0 &&
               facade.status().input.messages_received > 0;
      },
      20s));

  auto facadeReceiver{facade.openHandler(
      dmn::Dmn_DMesgDbus::HandlerSpec{"facade-receiver", "to-facade"})};
  auto facadeSender{facade.openHandler(
      dmn::Dmn_DMesgDbus::HandlerSpec{"facade-sender", "to-peer"})};
  auto peerReceiver{peer.openHandler("peer-receiver", "to-peer")};
  auto peerSender{peer.openHandler("peer-sender", "to-facade")};

  dmn::DMesgPb fromFacade;
  fromFacade.set_type(dmn::DMesgTypePb::message);
  fromFacade.mutable_body()->set_message("from facade");
  dmn::DMesgPb fromPeer;
  fromPeer.set_type(dmn::DMesgTypePb::message);
  fromPeer.mutable_body()->set_message("from Option A");

  auto facadeWrite{std::async(std::launch::async, [&facadeSender, &fromFacade] {
    facadeSender->write(fromFacade);
  })};
  auto peerWrite{std::async(std::launch::async, [&peerSender, &fromPeer] {
    peerSender->write(fromPeer);
  })};
  facadeWrite.get();
  peerWrite.get();

  auto facadeRead{std::async(std::launch::async, [&facadeReceiver] {
    return facadeReceiver->read();
  })};
  auto peerRead{std::async(std::launch::async,
                           [&peerReceiver] { return peerReceiver->read(); })};
  ASSERT_EQ(facadeRead.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(peerRead.wait_for(5s), std::future_status::ready);
  const auto receivedByFacade{facadeRead.get()};
  const auto receivedByPeer{peerRead.get()};
  ASSERT_TRUE(receivedByFacade);
  ASSERT_TRUE(receivedByPeer);
  EXPECT_EQ(receivedByFacade->body().message(), "from Option A");
  EXPECT_EQ(receivedByPeer->body().message(), "from facade");

  facade.closeHandler(facadeReceiver);
  facade.closeHandler(facadeSender);
  peer.closeHandler(peerReceiver);
  peer.closeHandler(peerSender);
}

TEST(DmnDbusFacadeTest, StatusReportsInputAndOutputDisconnect) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());

  auto config{makeConfig()};
  config.bus_address = bus.address();
  dmn::Dmn_DMesgDbus facade{"facade-disconnect", config};

  const auto initialStatus{facade.status()};
  EXPECT_FALSE(initialStatus.input.terminal_error);
  EXPECT_FALSE(initialStatus.output.terminal_error);

  bus.stop();
  ASSERT_TRUE(waitUntil(
      [&facade] {
        const auto currentStatus{facade.status()};

        return currentStatus.input.terminal_error &&
               currentStatus.output.terminal_error;
      },
      5s));

  const auto disconnectedStatus{facade.status()};
  EXPECT_EQ(disconnectedStatus.input.terminal_error,
            std::make_error_code(std::errc::connection_reset));
  EXPECT_EQ(disconnectedStatus.output.terminal_error,
            std::make_error_code(std::errc::connection_reset));

  PrivateBus replacementBus;
  ASSERT_TRUE(replacementBus.valid());
  auto replacementConfig{makeConfig()};
  replacementConfig.bus_address = replacementBus.address();
  auto replacementOutput{
      std::make_shared<dmn::Dmn_DbusOutput>(replacementConfig)};
  ASSERT_TRUE(facade.setOutputHandler(replacementOutput));

  const auto recoveredOutputStatus{facade.status().output};
  EXPECT_FALSE(recoveredOutputStatus.terminal_error);
  EXPECT_EQ(recoveredOutputStatus.messages_written, 0U);
}

TEST(DmnDbusFacadeTest, ReplacesFailedOutputWithoutShuttingDownNode) {
  auto input{std::make_shared<BlockingInput>()};
  auto failedOutput{std::make_shared<FailingOutput>()};
  dmn::Dmn_DMesgNet node{"replace-output", input, failedOutput};

  ASSERT_TRUE(
      waitUntil([&failedOutput] { return failedOutput->attempts() == 1; }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(failedOutput->attempts(), 1U);

  auto replacement{std::make_shared<RecordingOutput>()};
  ASSERT_TRUE(node.setOutputHandler(replacement));
  ASSERT_TRUE(waitUntil([&replacement] { return replacement->writes() > 0; }));
  EXPECT_EQ(failedOutput->attempts(), 1U);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  // Keep all facade bus operations isolated from the caller's session bus.
  PrivateBus bus;
  if (!bus.setAsSessionBus()) {
    return EXIT_FAILURE;
  }

  const int result{RUN_ALL_TESTS()};
  google::protobuf::ShutdownProtobufLibrary();

  return result;
}
