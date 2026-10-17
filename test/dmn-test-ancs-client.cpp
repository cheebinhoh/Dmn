/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-ancs-client.cpp
 * @brief Deterministic fake-GATT tests for ANCS session behavior.
 */

#include <gtest/gtest.h>

#include "dmn-ancs-client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;

constexpr char kDeviceId[] = "device-1";
constexpr char kNotificationSourceId[] = "notification-source";
constexpr char kControlPointId[] = "control-point";
constexpr char kDataSourceId[] = "data-source";
constexpr char kNotificationSourceUuid[] =
    "9FBF120D-6301-42D9-8C58-25E699A21DBD";
constexpr char kControlPointUuid[] = "69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9";
constexpr char kDataSourceUuid[] = "22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB";
constexpr char kAncsServiceUuid[] = "7905F431-B5CE-4E99-A40F-4B1E122D00D0";

class FakeGattClient final : public dmn::Dmn_GattClient {
public:
  auto waitForServicesResolved(const std::string &, std::chrono::milliseconds)
      -> dmn::Dmn_GattResult override {
    return {};
  }

  auto discover(const std::string &, std::chrono::milliseconds)
      -> dmn::Dmn_GattValueResult<std::vector<dmn::Dmn_GattService>> override {
    if (missingAncsService) {
      return std::vector<dmn::Dmn_GattService>{};
    }

    std::vector<dmn::Dmn_GattCharacteristic> characteristics{
        {kNotificationSourceId, kNotificationSourceUuid, {"notify"}},
        {kControlPointId, kControlPointUuid, {"write"}},
        {kDataSourceId, kDataSourceUuid, {"notify"}}};
    if (missingDataSource) {
      characteristics.pop_back();
    }

    return std::vector<dmn::Dmn_GattService>{
        {"/service/ancs", kAncsServiceUuid, true, std::move(characteristics)}};
  }

  auto startNotify(const std::string &, const std::string &characteristicId,
                   NotificationHandler handler, std::chrono::milliseconds)
      -> dmn::Dmn_GattValueResult<dmn::Dmn_GattNotification> override {
    {
      std::lock_guard lock{mutex};
      subscriptionOrder.push_back(characteristicId);
      if (characteristicId == failSubscriptionFor) {
        condition.notify_all();

        return std::unexpected(dmn::Dmn_GattError{
            dmn::Dmn_GattErrorKind::Remote, "subscription rejected",
            "org.bluez.Error.NotAuthorized"});
      }

      handlers[characteristicId] = std::move(handler);
    }

    condition.notify_all();

    if (characteristicId == kNotificationSourceId && initialNotification) {
      emit(characteristicId, *initialNotification);
    }

    return dmn::Dmn_GattNotification{[this, characteristicId] {
      {
        std::lock_guard lock{mutex};
        ++cleanupAttempts[characteristicId];
        stoppedSubscriptions.push_back(characteristicId);
        auto &failures = cleanupFailuresRemaining[characteristicId];
        if (failures > 0) {
          --failures;
          condition.notify_all();

          return dmn::Dmn_GattResult{std::unexpected(dmn::Dmn_GattError{
              dmn::Dmn_GattErrorKind::Remote, "cleanup rejected",
              "org.bluez.Error.Failed"})};
        }

        handlers.erase(characteristicId);
      }

      condition.notify_all();

      return dmn::Dmn_GattResult{};
    }};
  }

  auto
  writeWithResponse(const std::string &, const std::string &,
                    const std::vector<std::uint8_t> &value,
                    std::chrono::milliseconds) -> dmn::Dmn_GattResult override {
    {
      std::unique_lock lock{mutex};
      writes.push_back(value);
      writeEntered = true;
      condition.notify_all();
      condition.wait(lock, [&] {
        return !blockControlPointWrite || releaseControlPointWrite;
      });
    }

    condition.notify_all();

    if (rejectControlPointWrite) {
      return std::unexpected(
          dmn::Dmn_GattError{dmn::Dmn_GattErrorKind::Remote, "request rejected",
                             "org.bluez.Error.InvalidArguments"});
    }

    return {};
  }

  void emit(const std::string &characteristicId, const Bytes &value) {
    emitEvent(
        characteristicId,
        dmn::Dmn_GattEvent{dmn::Dmn_GattEvent::Kind::ValueChanged, value, {}});
  }

  void emitEvent(const std::string &characteristicId,
                 const dmn::Dmn_GattEvent &event) {
    NotificationHandler handler;
    {
      std::lock_guard lock{mutex};
      const auto found = handlers.find(characteristicId);
      if (found != handlers.end()) {
        handler = found->second;
      }
    }

    if (handler) {
      handler(event);
    }
  }

  auto waitForWriteCount(std::size_t count) -> bool {
    std::unique_lock lock{mutex};

    return condition.wait_for(lock, 2s, [&] { return writes.size() >= count; });
  }

  auto waitForWriteEntered() -> bool {
    std::unique_lock lock{mutex};

    return condition.wait_for(lock, 2s, [&] { return writeEntered; });
  }

  void releaseBlockedWrite() {
    {
      std::lock_guard lock{mutex};
      releaseControlPointWrite = true;
    }

    condition.notify_all();
  }

  auto waitForEvent(dmn::Dmn_AncsEvent::Kind kind) -> bool {
    std::unique_lock lock{mutex};

    return condition.wait_for(lock, 2s, [&] {
      return std::any_of(
          events.begin(), events.end(),
          [kind](const auto &event) { return event.kind == kind; });
    });
  }

  auto waitForEventCount(dmn::Dmn_AncsEvent::Kind kind,
                         std::size_t count) -> bool {
    std::unique_lock lock{mutex};

    return condition.wait_for(lock, 2s, [&] {
      const auto eventCount = static_cast<std::size_t>(std::count_if(
          events.begin(), events.end(),
          [kind](const auto &event) { return event.kind == kind; }));
      return eventCount >= count;
    });
  }

  auto waitForStoppedSubscriptions(std::size_t count) -> bool {
    std::unique_lock lock{mutex};

    return condition.wait_for(
        lock, 2s, [&] { return stoppedSubscriptions.size() >= count; });
  }

  auto waitForCleanupAttempts(const std::string &characteristicId,
                              std::size_t count) -> bool {
    std::unique_lock lock{mutex};

    return condition.wait_for(
        lock, 2s, [&] { return cleanupAttempts[characteristicId] >= count; });
  }

  auto copyEvents() -> std::vector<dmn::Dmn_AncsEvent> {
    std::lock_guard lock{mutex};

    return events;
  }

  void recordEvent(const dmn::Dmn_AncsEvent &event) {
    {
      std::lock_guard lock{mutex};
      events.push_back(event);
    }

    condition.notify_all();
  }

  bool missingAncsService{};
  bool missingDataSource{};
  bool rejectControlPointWrite{};
  bool blockControlPointWrite{};
  bool releaseControlPointWrite{};
  bool writeEntered{};
  std::string failSubscriptionFor;
  std::map<std::string, std::size_t> cleanupFailuresRemaining;
  std::optional<Bytes> initialNotification;
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::string> subscriptionOrder;
  std::vector<std::string> stoppedSubscriptions;
  std::map<std::string, std::size_t> cleanupAttempts;
  std::vector<Bytes> writes;
  std::vector<dmn::Dmn_AncsEvent> events;
  std::map<std::string, NotificationHandler> handlers;
};

void appendUint16(Bytes &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void appendUint32(Bytes &bytes, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void appendAttribute(Bytes &bytes, std::uint8_t id, const std::string &value) {
  bytes.push_back(id);
  appendUint16(bytes, static_cast<std::uint16_t>(value.size()));
  for (const auto character : value) {
    bytes.push_back(static_cast<std::uint8_t>(character));
  }
}

auto makeResponse(std::uint32_t uid,
                  const std::string &date = "2026-10-17") -> Bytes {
  Bytes response{0};
  appendUint32(response, uid);
  appendAttribute(response, 0, "com.example.mail");
  appendAttribute(response, 1, "Subject");
  appendAttribute(response, 2, "");
  appendAttribute(response, 3, "Body");
  appendAttribute(response, 5, date);

  return response;
}

auto notificationSourceValue(std::uint8_t eventId, std::uint32_t uid) -> Bytes {
  Bytes value{eventId, 0x01, 6, 1};
  appendUint32(value, uid);

  return value;
}

TEST(DmnAncsClientTest, SubscribesDataSourceBeforeNotificationSource) {
  FakeGattClient gatt;
  constexpr std::uint32_t kUid{0x78563412};
  gatt.initialNotification = notificationSourceValue(0, kUid);

  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));

  const auto started = client.start(kDeviceId);

  ASSERT_TRUE(started) << started.error().message;
  {
    std::lock_guard lock{gatt.mutex};
    ASSERT_EQ(gatt.subscriptionOrder.size(), 2);
    EXPECT_EQ(gatt.subscriptionOrder[0], kDataSourceId);
    EXPECT_EQ(gatt.subscriptionOrder[1], kNotificationSourceId);
  }

  ASSERT_TRUE(gatt.waitForWriteCount(1));
  {
    std::lock_guard lock{gatt.mutex};
    const Bytes expected{0, 0x12, 0x34, 0x56, 0x78, 0, 1,  0,
                         1, 2,    0,    1,    3,    0, 16, 5};
    EXPECT_EQ(gatt.writes.front(), expected);
  }

  gatt.emit(kDataSourceId, makeResponse(kUid));
  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::NotificationAdded));
  const auto events = gatt.copyEvents();
  const auto readyEvent =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.kind == dmn::Dmn_AncsEvent::Kind::StateChanged &&
               event.state == dmn::Dmn_AncsState::Ready;
      });
  const auto addedEvent =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.kind == dmn::Dmn_AncsEvent::Kind::NotificationAdded;
      });
  ASSERT_NE(readyEvent, events.end());
  ASSERT_NE(addedEvent, events.end());
  EXPECT_LT(readyEvent, addedEvent);
  ASSERT_TRUE(addedEvent->notification.has_value());
  EXPECT_EQ(addedEvent->notification->app_identifier, "com.example.mail");
  EXPECT_EQ(addedEvent->notification->title, "Subject");
  EXPECT_EQ(addedEvent->notification->message, "Body");

  ASSERT_TRUE(client.stop());
  ASSERT_TRUE(gatt.waitForStoppedSubscriptions(2));
}

TEST(DmnAncsClientTest, RequiresHandlerAndValidFiniteTimeouts) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};

  const auto noHandler = client.start(kDeviceId);
  ASSERT_FALSE(noHandler);
  EXPECT_EQ(noHandler.error().kind, dmn::Dmn_AncsErrorKind::InvalidArgument);

  ASSERT_TRUE(client.setEventHandler([](const auto &) {}));
  dmn::Dmn_AncsConfig config;
  config.gatt_operation_timeout = 0ms;
  const auto invalidConfig = client.start(kDeviceId, config);
  ASSERT_FALSE(invalidConfig);
  EXPECT_EQ(invalidConfig.error().kind,
            dmn::Dmn_AncsErrorKind::InvalidArgument);
}

TEST(DmnAncsClientTest, MissingRequiredServiceFailsAndCanBeStopped) {
  FakeGattClient gatt;
  gatt.missingAncsService = true;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler([](const auto &) {}));

  const auto started = client.start(kDeviceId);

  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, dmn::Dmn_AncsErrorKind::Unsupported);
  EXPECT_EQ(started.error().message,
            "ANCS service was not found in the resolved GATT services");
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  ASSERT_TRUE(client.stop());
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Stopped);
}

TEST(DmnAncsClientTest, MissingRequiredCharacteristicIsReportedPrecisely) {
  FakeGattClient gatt;
  gatt.missingDataSource = true;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler([](const auto &) {}));

  const auto started = client.start(kDeviceId);

  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, dmn::Dmn_AncsErrorKind::Unsupported);
  EXPECT_EQ(started.error().message,
            "ANCS service is missing one or more required characteristics");
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest, RollsBackDataSubscriptionWhenSecondSubscriptionFails) {
  FakeGattClient gatt;
  gatt.failSubscriptionFor = kNotificationSourceId;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));

  const auto started = client.start(kDeviceId);

  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, dmn::Dmn_AncsErrorKind::Authorization);
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  ASSERT_TRUE(gatt.waitForCleanupAttempts(kDataSourceId, 1));
  {
    std::lock_guard lock{gatt.mutex};
    EXPECT_EQ(gatt.cleanupAttempts[kNotificationSourceId], 0);
    EXPECT_FALSE(gatt.handlers.contains(kDataSourceId));
  }

  ASSERT_TRUE(client.stop());
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Stopped);
}

TEST(DmnAncsClientTest, RetriesFailedSubscriptionCleanupOnNextStop) {
  FakeGattClient gatt;
  gatt.cleanupFailuresRemaining[kNotificationSourceId] = 1;
  gatt.cleanupFailuresRemaining[kDataSourceId] = 1;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  ASSERT_TRUE(client.start(kDeviceId));

  const auto firstStop = client.stop();

  ASSERT_FALSE(firstStop);
  EXPECT_EQ(firstStop.error().kind, dmn::Dmn_AncsErrorKind::Cleanup);
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  {
    std::lock_guard lock{gatt.mutex};
    EXPECT_EQ(gatt.cleanupAttempts[kNotificationSourceId], 1);
    EXPECT_EQ(gatt.cleanupAttempts[kDataSourceId], 1);
    EXPECT_TRUE(gatt.handlers.contains(kNotificationSourceId));
    EXPECT_TRUE(gatt.handlers.contains(kDataSourceId));
  }

  ASSERT_TRUE(client.stop());
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Stopped);
  {
    std::lock_guard lock{gatt.mutex};
    EXPECT_EQ(gatt.cleanupAttempts[kNotificationSourceId], 2);
    EXPECT_EQ(gatt.cleanupAttempts[kDataSourceId], 2);
    EXPECT_FALSE(gatt.handlers.contains(kNotificationSourceId));
    EXPECT_FALSE(gatt.handlers.contains(kDataSourceId));
  }
}

TEST(DmnAncsClientTest, InvalidatedGattSessionStopsBothSubscriptions) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  ASSERT_TRUE(client.start(kDeviceId));

  gatt.emitEvent(kDataSourceId,
                 dmn::Dmn_GattEvent{dmn::Dmn_GattEvent::Kind::Invalidated,
                                    {},
                                    "device disconnected"});

  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::Error));
  ASSERT_TRUE(gatt.waitForStoppedSubscriptions(2));
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  {
    std::lock_guard lock{gatt.mutex};
    EXPECT_FALSE(gatt.handlers.contains(kNotificationSourceId));
    EXPECT_FALSE(gatt.handlers.contains(kDataSourceId));
  }

  ASSERT_TRUE(client.stop());
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Stopped);
}

TEST(DmnAncsClientTest,
     DefaultBacklogLimitKeepsNewestNotificationsAndReportsDiscard) {
  FakeGattClient gatt;
  gatt.blockControlPointWrite = true;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  ASSERT_TRUE(client.start(kDeviceId));

  constexpr std::size_t kBurstSize{600};
  constexpr std::size_t kDefaultPendingLimit{255};
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 1));
  ASSERT_TRUE(gatt.waitForWriteEntered());
  for (std::size_t index = 0; index < kBurstSize; ++index) {
    const auto uid = static_cast<std::uint32_t>(index + 2);
    gatt.emit(kNotificationSourceId, notificationSourceValue(0, uid));
  }

  gatt.emit(kNotificationSourceId, notificationSourceValue(2, 9999));

  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::NotificationRemoved));
  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::Error));
  auto events = gatt.copyEvents();
  const auto warning =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.kind == dmn::Dmn_AncsEvent::Kind::Error;
      });
  ASSERT_NE(warning, events.end());
  ASSERT_TRUE(warning->error.has_value());
  EXPECT_FALSE(warning->error->fatal);
  EXPECT_NE(warning->error->message.find("oldest pending notification"),
            std::string::npos);

  gatt.releaseBlockedWrite();
  constexpr std::size_t kExpectedRequests{kDefaultPendingLimit + 1};
  for (std::size_t index = 0; index < kExpectedRequests; ++index) {
    const auto uid =
        index == 0 ? 1U
                   : static_cast<std::uint32_t>(
                         kBurstSize + 2 - kDefaultPendingLimit + index - 1);
    ASSERT_TRUE(gatt.waitForWriteCount(index + 1))
        << "attribute request was not sent for UID " << uid;
    gatt.emit(kDataSourceId, makeResponse(uid));
    ASSERT_TRUE(gatt.waitForEventCount(
        dmn::Dmn_AncsEvent::Kind::NotificationAdded, index + 1))
        << "notification attributes were not delivered for UID " << uid;
  }

  events = gatt.copyEvents();
  const auto addedCount =
      std::count_if(events.begin(), events.end(), [](const auto &event) {
        return event.kind == dmn::Dmn_AncsEvent::Kind::NotificationAdded;
      });
  EXPECT_EQ(addedCount, kExpectedRequests);
  const auto hasAddedUid = [&events](std::uint32_t uid) {
    return std::any_of(events.begin(), events.end(), [uid](const auto &event) {
      return event.kind == dmn::Dmn_AncsEvent::Kind::NotificationAdded &&
             event.notification && event.notification->uid == uid;
    });
  };
  EXPECT_FALSE(hasAddedUid(2));
  EXPECT_TRUE(hasAddedUid(347));
  EXPECT_TRUE(hasAddedUid(601));
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Ready);
  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest, ProcessesMoreThanFiveHundredQueuedNotifications) {
  FakeGattClient gatt;
  gatt.blockControlPointWrite = true;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  dmn::Dmn_AncsConfig config;
  config.maximum_pending_notifications = 0;
  ASSERT_TRUE(client.start(kDeviceId, config));

  constexpr std::size_t kBurstSize{600};
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 1));
  ASSERT_TRUE(gatt.waitForWriteEntered());
  for (std::size_t index = 0; index < kBurstSize; ++index) {
    const auto uid = static_cast<std::uint32_t>(index + 2);
    gatt.emit(kNotificationSourceId, notificationSourceValue(0, uid));
  }

  gatt.releaseBlockedWrite();
  for (std::size_t index = 0; index <= kBurstSize; ++index) {
    const auto uid = static_cast<std::uint32_t>(index + 1);
    ASSERT_TRUE(gatt.waitForWriteCount(index + 1))
        << "attribute request was not sent for UID " << uid;
    gatt.emit(kDataSourceId, makeResponse(uid));
    ASSERT_TRUE(gatt.waitForEventCount(
        dmn::Dmn_AncsEvent::Kind::NotificationAdded, index + 1))
        << "notification attributes were not delivered for UID " << uid;
  }

  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Ready);
  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest, CanSuppressNotificationsPredatingSessionStart) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  dmn::Dmn_AncsConfig config;
  config.notification_date_cutoff = std::chrono::system_clock::now();
  ASSERT_TRUE(client.start(kDeviceId, config));

  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 1));
  ASSERT_TRUE(gatt.waitForWriteCount(1));
  gatt.emit(kDataSourceId, makeResponse(1, "20000101T000000"));

  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 2));
  ASSERT_TRUE(gatt.waitForWriteCount(2));
  gatt.emit(kDataSourceId, makeResponse(2, "29991231T235959"));
  ASSERT_TRUE(
      gatt.waitForEventCount(dmn::Dmn_AncsEvent::Kind::NotificationAdded, 1));

  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 3));
  ASSERT_TRUE(gatt.waitForWriteCount(3));
  gatt.emit(kDataSourceId, makeResponse(3, "unparseable"));
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 4));
  ASSERT_TRUE(gatt.waitForWriteCount(4));
  gatt.emit(kDataSourceId, makeResponse(4, "29991231T235959"));
  ASSERT_TRUE(
      gatt.waitForEventCount(dmn::Dmn_AncsEvent::Kind::NotificationAdded, 2));

  const auto events = gatt.copyEvents();
  EXPECT_FALSE(std::any_of(events.begin(), events.end(), [](const auto &event) {
    return (event.kind == dmn::Dmn_AncsEvent::Kind::NotificationAdded ||
            event.kind == dmn::Dmn_AncsEvent::Kind::NotificationUpdated) &&
           event.notification &&
           (event.notification->uid == 1 || event.notification->uid == 3);
  }));
  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest,
     SuppressesRemovalForNotificationNotDeliveredBeforeSessionCutoff) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  dmn::Dmn_AncsConfig config;
  config.notification_date_cutoff = std::chrono::system_clock::now();
  ASSERT_TRUE(client.start(kDeviceId, config));

  constexpr std::uint32_t kUid{1};
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, kUid));
  ASSERT_TRUE(gatt.waitForWriteCount(1));
  gatt.emit(kNotificationSourceId, notificationSourceValue(2, kUid));
  gatt.emit(kDataSourceId, makeResponse(kUid, "20000101T000000"));
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, kUid + 1));
  ASSERT_TRUE(gatt.waitForWriteCount(2));
  ASSERT_TRUE(client.stop());

  const auto events = gatt.copyEvents();
  EXPECT_FALSE(std::any_of(events.begin(), events.end(), [](const auto &event) {
    return event.notification && event.notification->uid == kUid &&
           (event.kind == dmn::Dmn_AncsEvent::Kind::NotificationAdded ||
            event.kind == dmn::Dmn_AncsEvent::Kind::NotificationUpdated ||
            event.kind == dmn::Dmn_AncsEvent::Kind::NotificationRemoved);
  }));
}

TEST(DmnAncsClientTest, SerializesCallbacksOffGattThreadAndStopsCleanly) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  std::mutex callbackMutex;
  std::condition_variable callbackCondition;
  std::vector<std::thread::id> callbackThreads;
  bool readySeen{};
  ASSERT_TRUE(client.setEventHandler([&](const auto &event) {
    {
      std::lock_guard lock{callbackMutex};
      callbackThreads.push_back(std::this_thread::get_id());
      readySeen =
          readySeen || (event.kind == dmn::Dmn_AncsEvent::Kind::StateChanged &&
                        event.state == dmn::Dmn_AncsState::Ready);
    }

    callbackCondition.notify_all();
    gatt.recordEvent(event);
  }));

  const auto callerThread = std::this_thread::get_id();
  ASSERT_TRUE(client.start(kDeviceId));
  {
    std::unique_lock lock{callbackMutex};
    ASSERT_TRUE(
        callbackCondition.wait_for(lock, 2s, [&] { return readySeen; }));
  }

  gatt.emit(kNotificationSourceId, notificationSourceValue(2, 42));
  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::NotificationRemoved));
  ASSERT_TRUE(client.stop());

  std::size_t callbackCountAfterStop{};
  {
    std::lock_guard lock{callbackMutex};
    ASSERT_FALSE(callbackThreads.empty());
    EXPECT_NE(callbackThreads.front(), callerThread);
    EXPECT_TRUE(std::all_of(callbackThreads.begin(), callbackThreads.end(),
                            [&](const auto threadId) {
                              return threadId == callbackThreads.front();
                            }));
    callbackCountAfterStop = callbackThreads.size();
  }

  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 43));
  {
    std::lock_guard lock{callbackMutex};
    EXPECT_EQ(callbackThreads.size(), callbackCountAfterStop);
  }
}

TEST(DmnAncsClientTest, RemovesNotificationWithoutRequestingAttributes) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  ASSERT_TRUE(client.start(kDeviceId));

  constexpr std::uint32_t kUid{0x10203040};
  gatt.emit(kNotificationSourceId, notificationSourceValue(2, kUid));

  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::NotificationRemoved));
  {
    std::lock_guard lock{gatt.mutex};
    EXPECT_TRUE(gatt.writes.empty());
  }

  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest, SerializesQueuedAttributeRequests) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  ASSERT_TRUE(client.start(kDeviceId));

  constexpr std::uint32_t kFirstUid{0x11111111};
  constexpr std::uint32_t kSecondUid{0x22222222};
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, kFirstUid));
  gatt.emit(kNotificationSourceId, notificationSourceValue(0, kSecondUid));
  ASSERT_TRUE(gatt.waitForWriteCount(1));

  {
    std::lock_guard lock{gatt.mutex};
    ASSERT_EQ(gatt.writes.size(), 1);
    EXPECT_EQ(gatt.writes.front()[1], 0x11);
  }

  gatt.emit(kDataSourceId, makeResponse(kFirstUid));
  ASSERT_TRUE(gatt.waitForWriteCount(2));
  {
    std::lock_guard lock{gatt.mutex};
    ASSERT_EQ(gatt.writes.size(), 2);
    EXPECT_EQ(gatt.writes.back()[1], 0x22);
  }

  gatt.emit(kDataSourceId, makeResponse(kSecondUid));
  ASSERT_TRUE(
      gatt.waitForEventCount(dmn::Dmn_AncsEvent::Kind::NotificationAdded, 2));
  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest, FailsSessionOnControlPointWriteError) {
  FakeGattClient gatt;
  gatt.rejectControlPointWrite = true;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  ASSERT_TRUE(client.start(kDeviceId));

  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 17));

  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::Error));
  const auto events = gatt.copyEvents();
  const auto error =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.kind == dmn::Dmn_AncsEvent::Kind::Error;
      });
  ASSERT_NE(error, events.end());
  ASSERT_TRUE(error->error.has_value());
  EXPECT_TRUE(error->error->fatal);
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  ASSERT_TRUE(client.stop());
}

TEST(DmnAncsClientTest, FailsSessionWhenAttributeResponseTimesOut) {
  FakeGattClient gatt;
  dmn::Dmn_AncsClient client{gatt};
  ASSERT_TRUE(client.setEventHandler(
      [&](const auto &event) { gatt.recordEvent(event); }));
  dmn::Dmn_AncsConfig config;
  config.attribute_response_timeout = 30ms;
  ASSERT_TRUE(client.start(kDeviceId, config));

  gatt.emit(kNotificationSourceId, notificationSourceValue(0, 23));

  ASSERT_TRUE(gatt.waitForEventCount(
      dmn::Dmn_AncsEvent::Kind::AttributesUnavailable, 1));
  ASSERT_TRUE(gatt.waitForEvent(dmn::Dmn_AncsEvent::Kind::Error));
  EXPECT_EQ(client.state(), dmn::Dmn_AncsState::Failed);
  {
    std::lock_guard lock{gatt.mutex};
    EXPECT_EQ(gatt.writes.size(), 1);
  }

  ASSERT_TRUE(client.stop());
}

} // namespace
