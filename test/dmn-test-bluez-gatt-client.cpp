/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluez-gatt-client.cpp
 * @brief Private-bus tests for the BlueZ remote GATT client.
 */

#include "dmn-bluez-gatt-client.hpp"
#include "dmn-proc.hpp"
#include "dmn-test-dbus-support.hpp"

#include <dbus/dbus.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using dmn_test_dbus::DbusConnectionPtr;
using dmn_test_dbus::DbusErrorGuard;
using dmn_test_dbus::DbusMessagePtr;
using dmn_test_dbus::PrivateBus;

constexpr char kBluez[] = "org.bluez";
constexpr char kDevicePath[] = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";
constexpr char kOtherDevicePath[] = "/org/bluez/hci0/dev_11_22_33_44_55_66";
constexpr char kServicePath[] =
    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service001";
constexpr char kCharacteristicPath[] =
    "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF/service001/char001";
constexpr char kOtherServicePath[] =
    "/org/bluez/hci0/dev_11_22_33_44_55_66/service001";

static_assert(std::is_move_constructible_v<dmn::Dmn_GattNotification>);
static_assert(!std::is_move_assignable_v<dmn::Dmn_GattNotification>);
static_assert(std::has_virtual_destructor_v<dmn::Dmn_GattNotification>);
static_assert(std::has_virtual_destructor_v<dmn::Dmn_GattClient>);
static_assert(std::has_virtual_destructor_v<dmn::Dmn_BluezGattClient>);

void appendStringVariant(DBusMessageIter &properties, const char *name,
                         const char *value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_STRING_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&properties, &entry);
}

void appendBoolVariant(DBusMessageIter &properties, const char *name,
                       bool value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  const dbus_bool_t encoded = value ? TRUE : FALSE;
  dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &encoded);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&properties, &entry);
}

void appendObjectPathVariant(DBusMessageIter &properties, const char *name,
                             const char *value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_OBJECT_PATH_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&properties, &entry);
}

void appendByteArrayVariant(DBusMessageIter &properties, const char *name,
                            const std::vector<std::uint8_t> &value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  DBusMessageIter bytes;
  dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
  dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
                                   DBUS_TYPE_BYTE_AS_STRING, &bytes);
  if (!value.empty()) {
    const auto *data = value.data();
    dbus_message_iter_append_fixed_array(&bytes, DBUS_TYPE_BYTE, &data,
                                         static_cast<int>(value.size()));
  }

  dbus_message_iter_close_container(&variant, &bytes);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&properties, &entry);
}

void appendStringArrayVariant(DBusMessageIter &properties, const char *name,
                              const std::vector<std::string> &values) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  DBusMessageIter array;
  dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
  dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
                                   DBUS_TYPE_STRING_AS_STRING, &array);
  for (const auto &value : values) {
    const char *text = value.c_str();
    dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &text);
  }

  dbus_message_iter_close_container(&variant, &array);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&properties, &entry);
}

void openObject(DBusMessageIter &objects, const char *path,
                DBusMessageIter &object, DBusMessageIter &interfaces) {
  dbus_message_iter_open_container(&objects, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &object);
  dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH, &path);
  dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                   &interfaces);
}

void appendInterfaceStart(DBusMessageIter &interfaces, const char *name,
                          DBusMessageIter &entry, DBusMessageIter &properties) {
  dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}",
                                   &properties);
}

void appendDevice(DBusMessageIter &objects, const char *path, bool connected,
                  bool resolved) {
  DBusMessageIter object;
  DBusMessageIter interfaces;
  DBusMessageIter entry;
  DBusMessageIter properties;
  openObject(objects, path, object, interfaces);
  appendInterfaceStart(interfaces, "org.bluez.Device1", entry, properties);
  appendBoolVariant(properties, "Connected", connected);
  appendBoolVariant(properties, "ServicesResolved", resolved);
  dbus_message_iter_close_container(&entry, &properties);
  dbus_message_iter_close_container(&interfaces, &entry);
  dbus_message_iter_close_container(&object, &interfaces);
  dbus_message_iter_close_container(&objects, &object);
}

void appendService(DBusMessageIter &objects, const char *path,
                   const char *devicePath, const char *uuid,
                   bool primary = true) {
  DBusMessageIter object;
  DBusMessageIter interfaces;
  DBusMessageIter entry;
  DBusMessageIter properties;
  openObject(objects, path, object, interfaces);
  appendInterfaceStart(interfaces, "org.bluez.GattService1", entry, properties);
  appendObjectPathVariant(properties, "Device", devicePath);
  appendStringVariant(properties, "UUID", uuid);
  appendBoolVariant(properties, "Primary", primary);
  dbus_message_iter_close_container(&entry, &properties);
  dbus_message_iter_close_container(&interfaces, &entry);
  dbus_message_iter_close_container(&object, &interfaces);
  dbus_message_iter_close_container(&objects, &object);
}

void appendCharacteristic(DBusMessageIter &objects, const char *path,
                          const char *servicePath, const char *uuid,
                          const std::vector<std::string> &flags) {
  DBusMessageIter object;
  DBusMessageIter interfaces;
  DBusMessageIter entry;
  DBusMessageIter properties;
  openObject(objects, path, object, interfaces);
  appendInterfaceStart(interfaces, "org.bluez.GattCharacteristic1", entry,
                       properties);
  appendObjectPathVariant(properties, "Service", servicePath);
  appendStringVariant(properties, "UUID", uuid);
  appendStringArrayVariant(properties, "Flags", flags);
  appendByteArrayVariant(properties, "Value", {1, 2, 3});
  dbus_message_iter_close_container(&entry, &properties);
  dbus_message_iter_close_container(&interfaces, &entry);
  dbus_message_iter_close_container(&object, &interfaces);
  dbus_message_iter_close_container(&objects, &object);
}

class FakeBluez {
public:
  explicit FakeBluez(const std::string &address, bool connected = true,
                     bool resolved = true) {
    DbusErrorGuard error;
    m_connection.reset(
        dbus_connection_open_private(address.c_str(), error.get()));
    if (m_connection == nullptr ||
        !dbus_bus_register(m_connection.get(), error.get())) {
      throw std::runtime_error("unable to connect fake BlueZ service");
    }

    dbus_connection_set_exit_on_disconnect(m_connection.get(), FALSE);
    if (dbus_bus_request_name(m_connection.get(), kBluez,
                              DBUS_NAME_FLAG_DO_NOT_QUEUE, error.get()) !=
            DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        !dbus_connection_add_filter(m_connection.get(), filter, this,
                                    nullptr)) {
      throw std::runtime_error("unable to register fake BlueZ service");
    }

    m_connected = connected;
    m_resolved = resolved;
    if (!m_worker.exec([this] { dispatch(); })) {
      throw std::runtime_error("unable to start fake BlueZ worker");
    }
  }

  ~FakeBluez() {
    m_stopping.store(true, std::memory_order_release);
    (void)m_worker.wait();
  }

  auto counts() const -> std::pair<int, int> {
    std::lock_guard lock{m_mutex};

    return {m_startCount, m_stopCount};
  }

  auto propertyGetCount() const -> int {
    std::lock_guard lock{m_mutex};

    return m_propertyGetCount;
  }

  auto writeType() const -> std::string {
    std::lock_guard lock{m_mutex};

    return m_writeType;
  }

  auto writtenBytes() const -> std::vector<std::uint8_t> {
    std::lock_guard lock{m_mutex};

    return m_writtenBytes;
  }

  void setStartFailure(bool value) {
    std::lock_guard lock{m_mutex};
    m_failStart = value;
  }

  void setStopFailure(bool value) {
    std::lock_guard lock{m_mutex};
    m_failStop = value;
  }

  void setCharacteristicFlags(std::vector<std::string> flags) {
    std::lock_guard lock{m_mutex};
    m_characteristicFlags = std::move(flags);
  }

  void emitValueOnStart() {
    std::lock_guard lock{m_mutex};
    m_emitValueOnStart = true;
  }

  void setWriteError(std::string name) {
    std::lock_guard lock{m_mutex};
    m_writeError = std::move(name);
  }

  void setResolved(bool value) {
    {
      std::lock_guard lock{m_mutex};
      m_resolved = value;
    }

    emitDeviceState(m_connected, value);
  }

  void emitDeviceState(bool connected, bool resolved) {
    {
      std::lock_guard lock{m_mutex};
      m_connected = connected;
      m_resolved = resolved;
    }

    emitBooleanProperties(
        kDevicePath, "org.bluez.Device1",
        std::vector<std::pair<std::string, bool>>{
            {"Connected", connected}, {"ServicesResolved", resolved}});
  }

  void emitValue(const std::vector<std::uint8_t> &bytes) {
    DbusMessagePtr signal{dbus_message_new_signal(
        kCharacteristicPath, "org.freedesktop.DBus.Properties",
        "PropertiesChanged")};
    DBusMessageIter out;
    DBusMessageIter changed;
    DBusMessageIter invalidated;
    dbus_message_iter_init_append(signal.get(), &out);
    const char *interfaceName = "org.bluez.GattCharacteristic1";
    dbus_message_iter_append_basic(&out, DBUS_TYPE_STRING, &interfaceName);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &changed);
    appendByteArrayVariant(changed, "Value", bytes);
    dbus_message_iter_close_container(&out, &changed);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING_AS_STRING, &invalidated);
    dbus_message_iter_close_container(&out, &invalidated);
    sendSignal(signal.get());
  }

  void emitObjectRemoved(
      const char *path,
      const char *interfaceName = "org.bluez.GattCharacteristic1") {
    DbusMessagePtr signal{dbus_message_new_signal(
        "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")};
    DBusMessageIter out;
    DBusMessageIter interfaces;
    dbus_message_iter_init_append(signal.get(), &out);
    dbus_message_iter_append_basic(&out, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING_AS_STRING, &interfaces);
    dbus_message_iter_append_basic(&interfaces, DBUS_TYPE_STRING,
                                   &interfaceName);
    dbus_message_iter_close_container(&out, &interfaces);
    sendSignal(signal.get());
  }

private:
  template <typename Pairs>
  void emitBooleanProperties(const char *path, const char *interfaceName,
                             const Pairs &properties) {
    DbusMessagePtr signal{dbus_message_new_signal(
        path, "org.freedesktop.DBus.Properties", "PropertiesChanged")};
    DBusMessageIter out;
    DBusMessageIter changed;
    DBusMessageIter invalidated;
    dbus_message_iter_init_append(signal.get(), &out);
    dbus_message_iter_append_basic(&out, DBUS_TYPE_STRING, &interfaceName);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &changed);
    for (const auto &[name, value] : properties) {
      appendBoolVariant(changed, name.c_str(), value);
    }

    dbus_message_iter_close_container(&out, &changed);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING_AS_STRING, &invalidated);
    dbus_message_iter_close_container(&out, &invalidated);
    sendSignal(signal.get());
  }

  void sendSignal(DBusMessage *signal) {
    std::lock_guard lock{m_sendMutex};
    dbus_connection_send(m_connection.get(), signal, nullptr);
    dbus_connection_flush(m_connection.get());
  }

  auto managedObjectsReply(DBusMessage *request) -> DBusMessage * {
    DbusMessagePtr reply{dbus_message_new_method_return(request)};
    DBusMessageIter out;
    DBusMessageIter objects;
    dbus_message_iter_init_append(reply.get(), &out);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
                                     &objects);
    bool connected{};
    bool resolved{};
    {
      std::lock_guard lock{m_mutex};
      connected = m_connected;
      resolved = m_resolved;
    }

    appendDevice(objects, kDevicePath, connected, resolved);
    appendService(objects, kServicePath, kDevicePath,
                  "7905f431-b5ce-4e99-a40f-4b1e122d00d0");
    std::vector<std::string> characteristicFlags;
    {
      std::lock_guard lock{m_mutex};
      characteristicFlags = m_characteristicFlags;
    }

    appendCharacteristic(objects, kCharacteristicPath, kServicePath,
                         "9fbf120d-6301-42d9-8c58-25e699a21dbd",
                         characteristicFlags);
    appendService(objects, kOtherServicePath, kOtherDevicePath,
                  "0000180F-0000-1000-8000-00805F9B34FB");
    dbus_message_iter_close_container(&out, &objects);

    return reply.release();
  }

  static auto filter(DBusConnection *, DBusMessage *message,
                     void *data) -> DBusHandlerResult {
    auto *self = static_cast<FakeBluez *>(data);
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply{};
    if (dbus_message_is_method_call(message,
                                    "org.freedesktop.DBus.ObjectManager",
                                    "GetManagedObjects")) {
      reply = self->managedObjectsReply(message);
    } else if (dbus_message_is_method_call(
                   message, "org.freedesktop.DBus.Properties", "Get")) {
      const char *interfaceName{};
      const char *propertyName{};
      DBusMessageIter args;
      if (!dbus_message_iter_init(message, &args)) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }

      dbus_message_iter_get_basic(&args, &interfaceName);
      dbus_message_iter_next(&args);
      dbus_message_iter_get_basic(&args, &propertyName);
      if (interfaceName == nullptr || propertyName == nullptr ||
          std::string{interfaceName} != "org.bluez.Device1" ||
          dbus_message_get_path(message) == nullptr ||
          std::string{dbus_message_get_path(message)} != kDevicePath) {
        reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_OBJECT,
                                       "unknown device property");
      } else {
        bool value{};
        {
          std::lock_guard lock{self->m_mutex};
          ++self->m_propertyGetCount;
          value = std::string{propertyName} == "Connected" ? self->m_connected
                                                           : self->m_resolved;
        }

        reply = dbus_message_new_method_return(message);
        DBusMessageIter out;
        DBusMessageIter variant;
        const dbus_bool_t encoded = value ? TRUE : FALSE;
        dbus_message_iter_init_append(reply, &out);
        dbus_message_iter_open_container(&out, DBUS_TYPE_VARIANT,
                                         DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &encoded);
        dbus_message_iter_close_container(&out, &variant);
      }
    } else if (dbus_message_is_method_call(
                   message, "org.bluez.GattCharacteristic1", "StartNotify")) {
      bool fail{};
      bool emitValue{};
      {
        std::lock_guard lock{self->m_mutex};
        ++self->m_startCount;
        fail = self->m_failStart;
        emitValue = std::exchange(self->m_emitValueOnStart, false);
      }

      if (emitValue && !fail) {
        self->emitValue({0xA1, 0xB2});
      }

      reply = fail ? dbus_message_new_error(message,
                                            "org.bluez.Error.NotAuthorized",
                                            "notification authorization denied")
                   : dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(
                   message, "org.bluez.GattCharacteristic1", "StopNotify")) {
      bool fail{};
      {
        std::lock_guard lock{self->m_mutex};
        ++self->m_stopCount;
        fail = self->m_failStop;
      }

      reply = fail ? dbus_message_new_error(message, "org.bluez.Error.Failed",
                                            "injected StopNotify failure")
                   : dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(
                   message, "org.bluez.GattCharacteristic1", "WriteValue")) {
      std::string writeError;
      DBusMessageIter args;
      if (!dbus_message_iter_init(message, &args)) {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
      }

      DBusMessageIter bytes;
      dbus_message_iter_recurse(&args, &bytes);
      std::vector<std::uint8_t> value;
      while (dbus_message_iter_get_arg_type(&bytes) == DBUS_TYPE_BYTE) {
        unsigned char byte{};
        dbus_message_iter_get_basic(&bytes, &byte);
        value.push_back(byte);
        dbus_message_iter_next(&bytes);
      }

      dbus_message_iter_next(&args);
      DBusMessageIter options;
      dbus_message_iter_recurse(&args, &options);
      std::string type;
      if (dbus_message_iter_get_arg_type(&options) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        DBusMessageIter variant;
        const char *key{};
        dbus_message_iter_recurse(&options, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        dbus_message_iter_recurse(&entry, &variant);
        const char *requestedType{};
        dbus_message_iter_get_basic(&variant, &requestedType);
        type = requestedType == nullptr ? "" : requestedType;
      }

      {
        std::lock_guard lock{self->m_mutex};
        self->m_writtenBytes = std::move(value);
        self->m_writeType = std::move(type);
        writeError = self->m_writeError;
      }

      reply = writeError.empty()
                  ? dbus_message_new_method_return(message)
                  : dbus_message_new_error(message, writeError.c_str(),
                                           "injected GATT write failure");
    } else {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    if (reply != nullptr) {
      dbus_connection_send(self->m_connection.get(), reply, nullptr);
      dbus_message_unref(reply);
      dbus_connection_flush(self->m_connection.get());

      return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  void dispatch() {
    while (!m_stopping) {
      dbus_connection_read_write_dispatch(m_connection.get(), 20);
    }
  }

  DbusConnectionPtr m_connection;
  std::atomic_bool m_stopping{};
  dmn::Dmn_Proc m_worker{"fake-bluez"};
  mutable std::mutex m_mutex;
  std::mutex m_sendMutex;
  bool m_connected{true};
  bool m_resolved{true};
  bool m_failStart{};
  bool m_failStop{};
  bool m_emitValueOnStart{};
  std::vector<std::string> m_characteristicFlags{"notify", "read", "write"};
  int m_startCount{};
  int m_stopCount{};
  int m_propertyGetCount{};
  std::string m_writeError;
  std::string m_writeType;
  std::vector<std::uint8_t> m_writtenBytes;
};

auto configFor(const std::string &address) -> dmn::Dmn_BluezClientConfig {
  dmn::Dmn_BluezClientConfig config;
  config.test_bus_address = address;

  return config;
}

TEST(DmnBluezGattClient, DiscoversServicesAndCharacteristicsForSelectedDevice) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  const auto discovered = client.discover(kDevicePath, 1s);
  ASSERT_TRUE(discovered) << discovered.error().message;
  ASSERT_EQ(discovered->size(), 1U);
  EXPECT_EQ(discovered->front().id, kServicePath);
  EXPECT_EQ(discovered->front().uuid, "7905F431-B5CE-4E99-A40F-4B1E122D00D0");
  ASSERT_EQ(discovered->front().characteristics.size(), 1U);
  EXPECT_EQ(discovered->front().characteristics.front().id,
            kCharacteristicPath);
  EXPECT_EQ(discovered->front().characteristics.front().flags,
            (std::vector<std::string>{"notify", "read", "write"}));
}

TEST(DmnBluezGattClient, WaitsForServiceResolutionWithFiniteDeadline) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address(), true, false};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  auto waiting = std::async(std::launch::async, [&] {
    return client.waitForServicesResolved(kDevicePath, 2s);
  });
  ASSERT_TRUE(dmn_test_dbus::waitUntil(
      [&] { return waiting.wait_for(0ms) == std::future_status::timeout; }));
  service.setResolved(true);
  const auto result = waiting.get();
  ASSERT_TRUE(result) << result.error().message;

  service.setResolved(false);
  const auto timedOut = client.waitForServicesResolved(kDevicePath, 40ms);
  ASSERT_FALSE(timedOut);
  EXPECT_EQ(timedOut.error().kind, dmn::Dmn_GattErrorKind::Timeout);
}

TEST(DmnBluezGattClient, ServiceWaitFailsWhenDeviceIsRemoved) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address(), true, false};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  auto waiting = std::async(std::launch::async, [&] {
    return client.waitForServicesResolved(kDevicePath, 2s);
  });
  ASSERT_TRUE(dmn_test_dbus::waitUntil(
      [&] { return service.propertyGetCount() >= 2; }));

  service.emitObjectRemoved(kDevicePath, "org.bluez.Device1");
  ASSERT_EQ(waiting.wait_for(1s), std::future_status::ready);
  const auto result = waiting.get();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_GattErrorKind::NotReady);
  EXPECT_EQ(result.error().message,
            "selected BlueZ device was removed during service wait");
}

TEST(DmnBluezGattClient, ServiceWaitFailsWhenBluezOwnerIsLost) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  auto service = std::make_unique<FakeBluez>(bus.address(), true, false);
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  auto waiting = std::async(std::launch::async, [&] {
    return client.waitForServicesResolved(kDevicePath, 2s);
  });
  ASSERT_TRUE(dmn_test_dbus::waitUntil(
      [&] { return service->propertyGetCount() >= 2; }));

  service.reset();
  ASSERT_EQ(waiting.wait_for(1s), std::future_status::ready);
  const auto result = waiting.get();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_GattErrorKind::Backend);
}

TEST(DmnBluezGattClient, RejectsDiscoveryUntilConnectedAndResolved) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address(), true, false};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  const auto result = client.discover(kDevicePath, 1s);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_GattErrorKind::NotReady);
}

TEST(DmnBluezGattClient, StartsNotifyBeforeDeliveringValuesAndStopsExplicitly) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  service.emitValueOnStart();
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<dmn::Dmn_GattEvent> events;

  auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath,
      [&](const auto &event) {
        std::lock_guard lock{mutex};
        events.push_back(event);
        condition.notify_all();
      },
      1s);
  ASSERT_TRUE(notification) << notification.error().message;
  EXPECT_EQ(service.counts().first, 1);

  {
    std::unique_lock lock{mutex};
    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return !events.empty(); }));
  }

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front().kind, dmn::Dmn_GattEvent::Kind::ValueChanged);
  EXPECT_EQ(events.front().value, (std::vector<std::uint8_t>{0xA1, 0xB2}));

  const auto stopped = notification->stop();
  ASSERT_TRUE(stopped) << stopped.error().message;
  EXPECT_EQ(service.counts(), (std::pair{1, 1}));
}

TEST(DmnBluezGattClient, ReportsDisconnectAndStopsWithoutCallingBluezAgain) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<dmn::Dmn_GattEvent> events;

  auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath,
      [&](const auto &event) {
        std::lock_guard lock{mutex};
        events.push_back(event);
        condition.notify_all();
      },
      1s);
  ASSERT_TRUE(notification) << notification.error().message;

  service.emitDeviceState(false, false);
  {
    std::unique_lock lock{mutex};
    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return !events.empty(); }));
  }

  EXPECT_EQ(events.front().kind, dmn::Dmn_GattEvent::Kind::Invalidated);
  ASSERT_TRUE(notification->stop());
  EXPECT_EQ(service.counts(), (std::pair{1, 0}));
}

TEST(DmnBluezGattClient, ReportsCharacteristicRemovalAsInvalidation) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<dmn::Dmn_GattEvent> events;

  auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath,
      [&](const auto &event) {
        std::lock_guard lock{mutex};
        events.push_back(event);
        condition.notify_all();
      },
      1s);
  ASSERT_TRUE(notification) << notification.error().message;

  service.emitObjectRemoved(kCharacteristicPath);
  {
    std::unique_lock lock{mutex};
    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return !events.empty(); }));
  }

  EXPECT_EQ(events.front().kind, dmn::Dmn_GattEvent::Kind::Invalidated);
  ASSERT_TRUE(notification->stop());
  EXPECT_EQ(service.counts(), (std::pair{1, 0}));
}

TEST(DmnBluezGattClient, ReportsBluezOwnerLossAsInvalidation) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  auto service = std::make_unique<FakeBluez>(bus.address());
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<dmn::Dmn_GattEvent> events;

  auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath,
      [&](const auto &event) {
        std::lock_guard lock{mutex};
        events.push_back(event);
        condition.notify_all();
      },
      1s);
  ASSERT_TRUE(notification) << notification.error().message;

  service.reset();
  {
    std::unique_lock lock{mutex};
    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return !events.empty(); }));
  }

  EXPECT_EQ(events.front().kind, dmn::Dmn_GattEvent::Kind::Invalidated);
  EXPECT_EQ(events.front().message, "BlueZ service owner changed");
  ASSERT_TRUE(notification->stop());
}

TEST(DmnBluezGattClient, StopNotifyFailureCanBeReportedAndRetried) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath, [](const auto &) {}, 1s);
  ASSERT_TRUE(notification) << notification.error().message;
  service.setStopFailure(true);

  const auto failedStop = notification->stop();
  ASSERT_FALSE(failedStop);
  EXPECT_EQ(failedStop.error().kind, dmn::Dmn_GattErrorKind::Remote);
  EXPECT_EQ(failedStop.error().remote_name, "org.bluez.Error.Failed");

  service.setStopFailure(false);
  ASSERT_TRUE(notification->stop());
  EXPECT_EQ(service.counts(), (std::pair{1, 2}));
}

TEST(DmnBluezGattClient, NotificationTokenKeepsBackendAliveForCleanup) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  std::optional<dmn::Dmn_GattNotification> notification;
  {
    dmn::Dmn_BluezGattClient client{configFor(bus.address())};
    auto started = client.startNotify(
        kDevicePath, kCharacteristicPath, [](const auto &) {}, 1s);
    ASSERT_TRUE(started) << started.error().message;
    notification.emplace(std::move(*started));
  }

  ASSERT_TRUE(notification->stop());
  EXPECT_EQ(service.counts(), (std::pair{1, 1}));
}

TEST(DmnGattNotification, ConvertsCleanupExceptionToTypedError) {
  dmn::Dmn_GattNotification notification{[]() -> dmn::Dmn_GattResult {
    throw std::runtime_error{"injected cleanup exception"};
  }};

  const auto result = notification.stop();
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_GattErrorKind::Backend);
  EXPECT_EQ(result.error().message, "injected cleanup exception");
}

TEST(DmnBluezGattClient, RejectsNotificationForNonNotifiableCharacteristic) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  service.setCharacteristicFlags({"read", "write"});
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  const auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath, [](const auto &) {}, 1s);
  ASSERT_FALSE(notification);
  EXPECT_EQ(notification.error().kind, dmn::Dmn_GattErrorKind::InvalidArgument);
  EXPECT_EQ(service.counts(), (std::pair{0, 0}));
}

TEST(DmnBluezGattClient, PropagatesNotificationAuthorizationFailure) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  service.setStartFailure(true);
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};

  auto notification = client.startNotify(
      kDevicePath, kCharacteristicPath, [](const auto &) {}, 1s);
  ASSERT_FALSE(notification);
  EXPECT_EQ(notification.error().kind, dmn::Dmn_GattErrorKind::Remote);
  EXPECT_EQ(notification.error().remote_name, "org.bluez.Error.NotAuthorized");
}

TEST(DmnBluezGattClient, WritesWithResponseAndPreservesRemotePermissionError) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};
  const auto written =
      client.writeWithResponse(kDevicePath, kCharacteristicPath, {9, 8, 7}, 1s);
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_EQ(service.writeType(), "request");
  EXPECT_EQ(service.writtenBytes(), (std::vector<std::uint8_t>{9, 8, 7}));

  service.setWriteError("org.bluez.Error.NotPermitted");
  const auto result =
      client.writeWithResponse(kDevicePath, kCharacteristicPath, {9, 8, 7}, 1s);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_GattErrorKind::Remote);
  EXPECT_EQ(result.error().remote_name, "org.bluez.Error.NotPermitted");
}

TEST(DmnBluezGattClient, RejectsForeignCharacteristicPath) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluez service{bus.address()};
  dmn::Dmn_BluezGattClient client{configFor(bus.address())};
  const auto result = client.writeWithResponse(
      kDevicePath, "/org/bluez/hci0/dev_11_22_33_44_55_66/service001/char001",
      {1}, 1s);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind, dmn::Dmn_GattErrorKind::InvalidArgument);
}

} // namespace
