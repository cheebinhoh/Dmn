/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dbus-io.cpp
 * @brief Private-session-bus tests for the D-Bus Dmn_Io endpoints.
 */

#include "dmn-dbus-io.hpp"

#include "dmn-dmesgnet.hpp"
#include "dmn-test-dbus-support.hpp"

#include <dbus/dbus.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using dmn_test_dbus::DbusConnectionPtr;
using dmn_test_dbus::DbusErrorGuard;
using dmn_test_dbus::DbusMessagePtr;
using dmn_test_dbus::makeConfig;
using dmn_test_dbus::PrivateBus;
using dmn_test_dbus::waitUntil;

auto makeSignal(std::string_view path, std::string_view interface,
                std::string_view member) -> DBusMessage * {
  return dbus_message_new_signal(std::string{path}.c_str(),
                                 std::string{interface}.c_str(),
                                 std::string{member}.c_str());
}

class InputThreadGuard {
public:
  InputThreadGuard(dmn::Dmn_DbusInput &input, std::thread &worker)
      : m_input{input}, m_worker{worker} {}

  ~InputThreadGuard() {
    m_input.shutdown();
    if (m_worker.joinable()) {
      m_worker.join();
    }
  }

private:
  dmn::Dmn_DbusInput &m_input;
  std::thread &m_worker;
};

void sendMalformedSignal() {
  DbusErrorGuard error;
  DbusConnectionPtr connection{
      dbus_bus_get_private(DBUS_BUS_SESSION, error.get())};
  ASSERT_NE(connection.get(), nullptr)
      << (error.get()->message ? error.get()->message : "");
  dbus_connection_set_exit_on_disconnect(connection.get(), FALSE);

  DbusMessagePtr message{
      makeSignal("/org/dmn/DMesg1", "org.dmn.DMesg1.Transport", "Message")};
  ASSERT_NE(message.get(), nullptr);
  const char *invalidPayload{"not-an-array"};
  ASSERT_TRUE(dbus_message_append_args(message.get(), DBUS_TYPE_STRING,
                                       &invalidPayload, DBUS_TYPE_INVALID));
  ASSERT_TRUE(dbus_connection_send(connection.get(), message.get(), nullptr));
  dbus_connection_flush(connection.get());
}

void sendMethodCall() {
  DbusErrorGuard error;
  DbusConnectionPtr connection{
      dbus_bus_get_private(DBUS_BUS_SESSION, error.get())};
  ASSERT_NE(connection.get(), nullptr)
      << (error.get()->message ? error.get()->message : "");
  dbus_connection_set_exit_on_disconnect(connection.get(), FALSE);

  DbusMessagePtr message{
      dbus_message_new_method_call("org.freedesktop.DBus", "/org/dmn/DMesg1",
                                   "org.dmn.DMesg1.Transport", "Message")};
  ASSERT_NE(message.get(), nullptr);
  ASSERT_TRUE(dbus_connection_send(connection.get(), message.get(), nullptr));
  dbus_connection_flush(connection.get());
}

void sendByteSignal(std::string_view path, const std::string &payload,
                    std::string_view interface = "org.dmn.DMesg1.Transport",
                    std::string_view member = "Message",
                    bool appendExtraArgument = false) {
  DbusErrorGuard error;
  DbusConnectionPtr connection{
      dbus_bus_get_private(DBUS_BUS_SESSION, error.get())};
  ASSERT_NE(connection.get(), nullptr)
      << (error.get()->message ? error.get()->message : "");
  dbus_connection_set_exit_on_disconnect(connection.get(), FALSE);

  DbusMessagePtr message{makeSignal(path, interface, member)};
  ASSERT_NE(message.get(), nullptr);
  DBusMessageIter iterator;
  dbus_message_iter_init_append(message.get(), &iterator);
  DBusMessageIter array;
  ASSERT_TRUE(dbus_message_iter_open_container(
      &iterator, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &array));
  const auto *bytes{reinterpret_cast<const unsigned char *>(payload.data())};
  ASSERT_TRUE(payload.empty() || dbus_message_iter_append_fixed_array(
                                     &array, DBUS_TYPE_BYTE, &bytes,
                                     static_cast<int>(payload.size())));
  ASSERT_TRUE(dbus_message_iter_close_container(&iterator, &array));
  if (appendExtraArgument) {
    const char *extra{"extra"};
    ASSERT_TRUE(dbus_message_append_args(message.get(), DBUS_TYPE_STRING,
                                         &extra, DBUS_TYPE_INVALID));
  }

  ASSERT_TRUE(dbus_connection_send(connection.get(), message.get(), nullptr));
  dbus_connection_flush(connection.get());
}

TEST(DmnDbusIoTest, RejectsInvalidQueueLimitsBeforeOpeningBus) {
  auto config = makeConfig();
  config.max_message_bytes = 0;
  EXPECT_THROW(dmn::Dmn_DbusInput input{config}, std::invalid_argument);

  config = makeConfig();
  config.max_queued_messages = 0;
  EXPECT_THROW(dmn::Dmn_DbusOutput output{config}, std::invalid_argument);

  config = makeConfig();
  config.max_queued_bytes = 0;
  EXPECT_THROW(dmn::Dmn_DbusInput input{config}, std::invalid_argument);

  config = makeConfig();
  config.max_message_bytes = 8;
  config.max_queued_bytes = 7;
  EXPECT_THROW(dmn::Dmn_DbusOutput output{config}, std::invalid_argument);

  config = makeConfig();
  config.max_message_bytes =
      static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
  EXPECT_THROW(dmn::Dmn_DbusInput input{config}, std::invalid_argument);
}

TEST(DmnDbusIoTest, RejectsInvalidSignalTupleBeforeOpeningBus) {
  auto config = makeConfig();
  config.signal_path = "not/an/object/path";
  EXPECT_THROW(dmn::Dmn_DbusInput input{config}, std::invalid_argument);

  config = makeConfig();
  config.signal_interface = "org.invalid-interface";
  EXPECT_THROW(dmn::Dmn_DbusOutput output{config}, std::invalid_argument);
}

TEST(DmnDbusIoTest, RejectsInvalidExplicitBusAddressWithoutFallback) {
  auto config = makeConfig();
  config.bus_address = "not-a-dbus-address";
  EXPECT_THROW(dmn::Dmn_DbusInput input{config}, std::system_error);
  EXPECT_THROW(dmn::Dmn_DbusOutput output{config}, std::system_error);
}

TEST(DmnDbusIoTest, ConfiguredSignalTupleIsIsolatedFromDefaultTuple) {
  auto customConfig = makeConfig();
  customConfig.signal_path = "/org/example/ByteIo";
  customConfig.signal_interface = "org.example.ByteIo";
  customConfig.signal_member = "Payload";

  dmn::Dmn_DbusInput customInput{customConfig};
  dmn::Dmn_DbusInput defaultInput{makeConfig()};
  dmn::Dmn_DbusOutput customOutput{customConfig};
  auto customRead = std::async(std::launch::async,
                               [&customInput] { return customInput.read(); });
  auto defaultRead = std::async(
      std::launch::async, [&defaultInput] { return defaultInput.read(); });
  const std::string payload{"custom-route"};

  customOutput.write(payload);
  if (customRead.wait_for(3s) != std::future_status::ready) {
    customInput.shutdown();
    defaultInput.shutdown();
    EXPECT_THROW(customRead.get(), std::system_error);
    FAIL() << "custom signal tuple did not deliver its payload";
  }

  EXPECT_EQ(customRead.get(), payload);
  EXPECT_EQ(defaultRead.wait_for(100ms), std::future_status::timeout);

  defaultInput.shutdown();
  EXPECT_THROW(defaultRead.get(), std::system_error);
}

TEST(DmnDbusIoTest, DeliversBinaryPayloadToEveryMatchingSubscriber) {
  auto config = makeConfig();
  dmn::Dmn_DbusInput first{config};
  dmn::Dmn_DbusInput second{config};
  dmn::Dmn_DbusOutput output{config};
  const std::string payload{"\0\x01\x7f\x80\xff", 5};

  output.write(payload);

  auto firstRead =
      std::async(std::launch::async, [&first] { return first.read(); });
  auto secondRead =
      std::async(std::launch::async, [&second] { return second.read(); });

  ASSERT_EQ(firstRead.wait_for(3s), std::future_status::ready);
  ASSERT_EQ(secondRead.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(firstRead.get(), payload);
  EXPECT_EQ(secondRead.get(), payload);
  EXPECT_EQ(first.status().messages_received, 1U);
  EXPECT_EQ(output.status().messages_written, 1U);

  output.write(std::string{});
  auto firstEmptyRead =
      std::async(std::launch::async, [&first] { return first.read(); });
  auto secondEmptyRead =
      std::async(std::launch::async, [&second] { return second.read(); });
  ASSERT_EQ(firstEmptyRead.wait_for(3s), std::future_status::ready);
  ASSERT_EQ(secondEmptyRead.wait_for(3s), std::future_status::ready);
  const auto firstEmpty = firstEmptyRead.get();
  const auto secondEmpty = secondEmptyRead.get();
  ASSERT_TRUE(firstEmpty);
  ASSERT_TRUE(secondEmpty);
  EXPECT_TRUE(firstEmpty->empty());
  EXPECT_TRUE(secondEmpty->empty());
}

TEST(DmnDbusIoTest, ReportsMalformedBodyAndIgnoresNonmatchingSignal) {
  auto config = makeConfig();
  dmn::Dmn_DbusInput input{config};

  sendMalformedSignal();
  sendByteSignal("/org/dmn/DMesg1", "x", "org.dmn.Other", "Message");
  sendByteSignal("/org/dmn/DMesg1", "x", "org.dmn.DMesg1.Transport", "Other");
  sendByteSignal("/org/dmn/Other", "x");
  sendByteSignal("/org/dmn/DMesg1", "x", "org.dmn.DMesg1.Transport", "Message",
                 true);
  sendMethodCall();

  ASSERT_TRUE(
      waitUntil([&input] { return input.status().malformed_received == 2; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(input.status().messages_received, 0U);
  EXPECT_EQ(input.status().malformed_received, 2U);
}

TEST(DmnDbusIoTest, RejectsOversizedSignalBeforeQueueAdmission) {
  auto config = makeConfig();
  config.max_message_bytes = 4;
  config.max_queued_bytes = 4;
  dmn::Dmn_DbusInput input{config};

  sendByteSignal("/org/dmn/DMesg1", "12345");
  ASSERT_TRUE(
      waitUntil([&input] { return input.status().oversized_received == 1; }));
  EXPECT_EQ(input.status().messages_received, 0U);
}

TEST(DmnDbusIoTest, EnforcesPayloadLimitAndUnsupportedDirections) {
  auto config = makeConfig();
  config.max_message_bytes = 4;
  config.max_queued_bytes = 8;
  dmn::Dmn_DbusInput input{config};
  dmn::Dmn_DbusOutput output{config};

  const std::string inputWrite{"x"};
  EXPECT_THROW(input.write(inputWrite), std::system_error);
  EXPECT_THROW(input.write(std::string{"x"}), std::system_error);
  EXPECT_THROW(output.read(), std::system_error);
  EXPECT_THROW(output.write("12345"), std::system_error);

  std::string lvalue{"1234"};
  output.write(lvalue);
  EXPECT_TRUE(
      waitUntil([&input] { return input.status().messages_received == 1; }));
  EXPECT_EQ(input.read(), "1234");

  output.write(std::string{"move"});
  EXPECT_TRUE(
      waitUntil([&input] { return input.status().messages_received == 2; }));
  EXPECT_EQ(input.read(), "move");
}

TEST(DmnDbusIoTest, DropsNewestWhenInputMessageQueueIsFull) {
  auto config = makeConfig();
  config.max_message_bytes = 4;
  config.max_queued_messages = 1;
  config.max_queued_bytes = 8;
  dmn::Dmn_DbusInput input{config};
  dmn::Dmn_DbusOutput output{config};

  output.write("one!");
  ASSERT_TRUE(waitUntil(
      [&output] { return output.status().messages_queued_to_libdbus == 1; }));
  output.write("two!");

  ASSERT_TRUE(waitUntil([&input] {
    const auto status = input.status();

    return status.messages_received + status.input_queue_drops == 2;
  }));
  EXPECT_EQ(input.status().messages_received, 1U);
  EXPECT_EQ(input.status().input_queue_drops, 1U);
  EXPECT_EQ(input.status().pending_input_messages, 1U);
  EXPECT_EQ(input.status().pending_input_bytes, 4U);
  EXPECT_EQ(input.read(), "one!");
  EXPECT_EQ(input.status().pending_input_messages, 0U);
  EXPECT_EQ(input.status().pending_input_bytes, 0U);
}

TEST(DmnDbusIoTest, EnforcesInputByteLimitIndependentlyOfMessageCount) {
  auto config = makeConfig();
  config.max_message_bytes = 4;
  config.max_queued_messages = 4;
  config.max_queued_bytes = 4;
  dmn::Dmn_DbusInput input{config};
  auto outputConfig = config;
  outputConfig.max_queued_bytes = 8;
  dmn::Dmn_DbusOutput output{outputConfig};

  output.write("full");
  ASSERT_TRUE(waitUntil(
      [&output] { return output.status().messages_queued_to_libdbus == 1; }));
  output.write("xy");

  ASSERT_TRUE(waitUntil([&input] {
    const auto status = input.status();

    return status.messages_received + status.input_queue_drops == 2;
  }));
  EXPECT_EQ(input.status().messages_received, 1U);
  EXPECT_EQ(input.status().input_queue_drops, 1U);
  EXPECT_EQ(input.status().pending_input_messages, 1U);
  EXPECT_EQ(input.status().pending_input_bytes, 4U);
  EXPECT_EQ(input.read(), "full");
  EXPECT_EQ(input.status().pending_input_messages, 0U);
  EXPECT_EQ(input.status().pending_input_bytes, 0U);
}

TEST(DmnDbusIoTest, OutputQueueLimitsRejectSynchronouslyAndStayBounded) {
  constexpr std::size_t kPayloadBytes{256 * 1024};
  const std::string payload(kPayloadBytes, 'x');

  auto byteConfig = makeConfig();
  byteConfig.max_message_bytes = kPayloadBytes;
  byteConfig.max_queued_messages = 16;
  byteConfig.max_queued_bytes = kPayloadBytes;
  dmn::Dmn_DbusOutput byteLimited{byteConfig};
  std::uint64_t byteRejections{};
  for (int index = 0; index < 200; ++index) {
    try {
      byteLimited.write(payload);
    } catch (const std::system_error &error) {
      ASSERT_EQ(error.code(), std::make_error_code(std::errc::no_buffer_space));
      ++byteRejections;
    }
  }

  EXPECT_GT(byteRejections, 0U);
  EXPECT_EQ(byteLimited.status().output_queue_rejections, byteRejections);
  EXPECT_LE(byteLimited.status().pending_output_bytes, kPayloadBytes);
  byteLimited.shutdown();

  auto countConfig = byteConfig;
  countConfig.max_queued_messages = 1;
  countConfig.max_queued_bytes = 2 * kPayloadBytes;
  dmn::Dmn_DbusOutput countLimited{countConfig};
  std::uint64_t countRejections{};
  for (int index = 0; index < 200; ++index) {
    try {
      countLimited.write(std::string{"x"});
    } catch (const std::system_error &error) {
      ASSERT_EQ(error.code(), std::make_error_code(std::errc::no_buffer_space));
      ++countRejections;
    }
  }

  EXPECT_GT(countRejections, 0U);
  EXPECT_EQ(countLimited.status().output_queue_rejections, countRejections);
  EXPECT_LE(countLimited.status().pending_output_messages, 1U);
  countLimited.shutdown();
}

TEST(DmnDbusIoTest, ShutdownDrainsQueuedInputThenCancelsBlockedRead) {
  auto config = makeConfig();
  dmn::Dmn_DbusInput input{config};
  dmn::Dmn_DbusOutput output{config};

  output.write("queued");
  ASSERT_TRUE(
      waitUntil([&input] { return input.status().messages_received == 1; }));
  input.shutdown();
  input.shutdown();

  EXPECT_EQ(input.read(), "queued");
  try {
    static_cast<void>(input.read());
    FAIL() << "read after input shutdown must fail";
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(),
              std::make_error_code(std::errc::operation_canceled));
  }

  dmn::Dmn_DbusInput blockedInput{config};
  auto read = std::async(std::launch::async,
                         [&blockedInput] { return blockedInput.read(); });
  blockedInput.shutdown();
  EXPECT_EQ(read.wait_for(2s), std::future_status::ready);
  EXPECT_THROW(read.get(), std::system_error);

  output.shutdown();
  EXPECT_THROW(output.write(std::string{"late"}), std::system_error);
}

TEST(DmnDbusIoTest, DrainsQueuedInputBeforeReportingBusDisconnect) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  auto config = makeConfig();
  config.bus_address = bus.address();
  dmn::Dmn_DbusInput input{config};
  dmn::Dmn_DbusOutput output{config};

  output.write("before-disconnect");
  ASSERT_TRUE(
      waitUntil([&input] { return input.status().messages_received == 1; }));
  bus.stop();
  ASSERT_TRUE(waitUntil(
      [&input] { return static_cast<bool>(input.status().terminal_error); }));
  ASSERT_TRUE(waitUntil(
      [&output] { return static_cast<bool>(output.status().terminal_error); }));

  EXPECT_EQ(input.read(), "before-disconnect");
  try {
    static_cast<void>(input.read());
    FAIL() << "read after bus disconnect must preserve its terminal error";
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(), std::make_error_code(std::errc::connection_reset));
  }

  EXPECT_THROW(output.write(std::string{"after-disconnect"}),
               std::system_error);
}

TEST(DmnDbusIoTest, PrivateBusesDoNotExchangeSignalsOrReplayHistory) {
  PrivateBus publisherBus;
  PrivateBus subscriberBus;
  ASSERT_TRUE(publisherBus.valid());
  ASSERT_TRUE(subscriberBus.valid());

  auto outputConfig = makeConfig();
  outputConfig.bus_address = publisherBus.address();
  dmn::Dmn_DbusOutput output{outputConfig};
  output.write("private");
  ASSERT_TRUE(waitUntil(
      [&output] { return output.status().messages_queued_to_libdbus == 1; }));

  auto inputConfig = makeConfig();
  inputConfig.bus_address = subscriberBus.address();
  dmn::Dmn_DbusInput input{inputConfig};
  auto read = std::async(std::launch::async, [&input] { return input.read(); });
  std::this_thread::sleep_for(100ms);
  input.shutdown();
  ASSERT_EQ(read.wait_for(2s), std::future_status::ready);
  EXPECT_THROW(read.get(), std::system_error);
}

TEST(DmnDbusIoTest, ConcurrentSignalDeliveryAndInputShutdownCompleteSafely) {
  auto config = makeConfig();
  dmn::Dmn_DbusInput input{config};
  dmn::Dmn_DbusOutput output{config};
  std::thread publisher{[&output] {
    for (int index = 0; index < 100; ++index) {
      output.write(std::string{"concurrent"});
    }
  }};

  input.shutdown();
  publisher.join();
  output.shutdown();
  try {
    while (true) {
      static_cast<void>(input.read());
    }
  } catch (const std::system_error &error) {
    EXPECT_EQ(error.code(),
              std::make_error_code(std::errc::operation_canceled));
  }

  EXPECT_EQ(input.status().terminal_error, std::error_code{});
}

TEST(DmnDbusIoTest,
     DmnMesgNetExchangesApplicationMessagesOverInjectedEndpoints) {
  auto config = makeConfig();
  dmn::Dmn_DbusInput observer{config};
  auto inputA = std::make_shared<dmn::Dmn_DbusInput>(config);
  auto outputA = std::make_shared<dmn::Dmn_DbusOutput>(config);
  auto inputB = std::make_shared<dmn::Dmn_DbusInput>(config);
  auto outputB = std::make_shared<dmn::Dmn_DbusOutput>(config);

  std::atomic<bool> readyA{};
  std::atomic<bool> readyB{};
  std::atomic<bool> destroyedA{};
  std::atomic<bool> destroyedB{};
  std::atomic<std::uint64_t> messagesFromA{};
  std::atomic<std::uint64_t> messagesFromB{};
  std::thread observerWorker{[&] {
    while (true) {
      try {
        const auto payload = observer.read();
        if (!payload) {
          continue;
        }

        dmn::DMesgPb message;
        if (!message.ParseFromString(*payload)) {
          continue;
        }

        if (message.topic() == "from-a") {
          ++messagesFromA;
          continue;
        }

        if (message.topic() == "from-b") {
          ++messagesFromB;
          continue;
        }

        if (message.type() != dmn::DMesgTypePb::sys) {
          continue;
        }

        const auto &self = message.body().sys().self();
        if (self.identifier() == "dbus-node-a") {
          if (self.state() == dmn::DMesgStatePb::Ready) {
            readyA = true;
          } else if (self.state() == dmn::DMesgStatePb::Destroyed) {
            destroyedA = true;
          }
        } else if (self.identifier() == "dbus-node-b") {
          if (self.state() == dmn::DMesgStatePb::Ready) {
            readyB = true;
          } else if (self.state() == dmn::DMesgStatePb::Destroyed) {
            destroyedB = true;
          }
        }
      } catch (const std::system_error &) {
        break;
      }
    }
  }};
  InputThreadGuard observerGuard{observer, observerWorker};

  auto nodeA =
      std::make_unique<dmn::Dmn_DMesgNet>("dbus-node-a", inputA, outputA);
  auto nodeB =
      std::make_unique<dmn::Dmn_DMesgNet>("dbus-node-b", inputB, outputB);

  const bool ready{waitUntil(
      [&readyA, &readyB] { return readyA.load() && readyB.load(); }, 20s)};
  ASSERT_TRUE(ready)
      << "input A received=" << inputA->status().messages_received
      << " output A submitted=" << outputA->status().messages_queued_to_libdbus
      << " input B received=" << inputB->status().messages_received
      << " output B submitted=" << outputB->status().messages_queued_to_libdbus;

  auto receiverA = nodeA->openHandler("dbus-receiver-a", "from-b");
  auto receiverB = nodeB->openHandler("dbus-receiver-b", "from-a");
  auto senderA = nodeA->openHandler("dbus-sender-a");
  auto senderB = nodeB->openHandler("dbus-sender-b");
  dmn::DMesgPb messageA;
  messageA.set_topic("from-a");
  messageA.set_type(dmn::DMesgTypePb::message);
  messageA.mutable_body()->set_message("A to B");
  dmn::DMesgPb messageB;
  messageB.set_topic("from-b");
  messageB.set_type(dmn::DMesgTypePb::message);
  messageB.mutable_body()->set_message("B to A");

  auto sendA = std::async(std::launch::async,
                          [&senderA, &messageA] { senderA->write(messageA); });
  auto sendB = std::async(std::launch::async,
                          [&senderB, &messageB] { senderB->write(messageB); });
  sendA.get();
  sendB.get();
  auto receivedA = std::async(std::launch::async,
                              [&receiverA] { return receiverA->read(); });
  auto receivedB = std::async(std::launch::async,
                              [&receiverB] { return receiverB->read(); });
  ASSERT_EQ(receivedA.wait_for(3s), std::future_status::ready);
  ASSERT_EQ(receivedB.wait_for(3s), std::future_status::ready);
  const auto messageReadA = receivedA.get();
  const auto messageReadB = receivedB.get();
  ASSERT_TRUE(messageReadA);
  ASSERT_TRUE(messageReadB);
  EXPECT_EQ(messageReadA->body().message(), "B to A");
  EXPECT_EQ(messageReadB->body().message(), "A to B");
  ASSERT_TRUE(waitUntil([&messagesFromA, &messagesFromB] {
    return messagesFromA.load() >= 1 && messagesFromB.load() >= 1;
  }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(messagesFromA.load(), 1U);
  EXPECT_EQ(messagesFromB.load(), 1U);

  const auto writesBeforeDestroy{outputA->status().messages_written};
  nodeA.reset();
  ASSERT_TRUE(waitUntil([&outputA, writesBeforeDestroy] {
    return outputA->status().messages_written > writesBeforeDestroy;
  }));
  ASSERT_TRUE(waitUntil([&outputA] {
    const auto status = outputA->status();

    return status.messages_queued_to_libdbus == status.messages_written;
  }));
  ASSERT_TRUE(waitUntil([&destroyedA] { return destroyedA.load(); }));

  nodeB.reset();
  ASSERT_TRUE(waitUntil([&destroyedB] { return destroyedB.load(); }));
  observer.shutdown();
  observerWorker.join();
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  // Keep session-bus tests isolated from the caller's environment.
  PrivateBus bus;
  if (!bus.setAsSessionBus()) {
    return EXIT_FAILURE;
  }

  const int result{RUN_ALL_TESTS()};

  return result;
}
