/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluez-device-list.cpp
 * @brief Private-bus integration tests for the BlueZ client.
 */

#include "dmn-bluez-client.hpp"
#include "dmn-test-dbus-support.hpp"

#include <dbus/dbus.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <future>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace {

static_assert(!std::is_copy_constructible_v<dmn::Dmn_BluezClient>);
static_assert(!std::is_copy_assignable_v<dmn::Dmn_BluezClient>);
static_assert(!std::is_move_constructible_v<dmn::Dmn_BluezClient>);
static_assert(!std::is_move_assignable_v<dmn::Dmn_BluezClient>);

using namespace std::chrono_literals;
using dmn_test_dbus::DbusConnectionPtr;
using dmn_test_dbus::DbusErrorGuard;
using dmn_test_dbus::DbusMessagePtr;
using dmn_test_dbus::PrivateBus;

constexpr char kService[] = "org.bluez";
constexpr char kAdapterPath[] = "/org/bluez/hci0";
constexpr char kDevicePath[] = "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF";

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

void appendInt16Variant(DBusMessageIter &properties, const char *name,
                        dbus_int16_t value) {
  DBusMessageIter entry;
  DBusMessageIter variant;
  dbus_message_iter_open_container(&properties, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &entry);
  dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                   DBUS_TYPE_INT16_AS_STRING, &variant);
  dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT16, &value);
  dbus_message_iter_close_container(&entry, &variant);
  dbus_message_iter_close_container(&properties, &entry);
}

void appendInterface(DBusMessageIter &interfaces, const char *name,
                     bool adapter, int deviceNameMode = 0,
                     const char *address = nullptr, bool powered = true,
                     bool connected = false, bool paired = true) {
  DBusMessageIter interfaceEntry;
  DBusMessageIter properties;
  dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &interfaceEntry);
  dbus_message_iter_append_basic(&interfaceEntry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&interfaceEntry, DBUS_TYPE_ARRAY, "{sv}",
                                   &properties);
  if (adapter) {
    appendStringVariant(properties, "Address",
                        address == nullptr ? "B8:F7:75:BF:F3:C3" : address);
    appendStringVariant(properties, "Alias", "Test adapter");
    appendBoolVariant(properties, "Powered", powered);
  } else {
    appendStringVariant(properties, "Address",
                        address == nullptr ? "AA:BB:CC:DD:EE:FF" : address);
    if (deviceNameMode == 0) {
      appendStringVariant(properties, "Alias", "Known headset");
    } else if (deviceNameMode == 1) {
      appendStringVariant(properties, "Name", "Reported headset");
    } else if (deviceNameMode == 3) {
      appendStringVariant(properties, "Alias", "Headset, \"One\"");
    }

    appendBoolVariant(properties, "Paired", paired);
    appendBoolVariant(properties, "Connected", connected);
    appendInt16Variant(properties, "RSSI", -47);
  }

  dbus_message_iter_close_container(&interfaceEntry, &properties);
  dbus_message_iter_close_container(&interfaces, &interfaceEntry);
}

auto makeManagedObjectsReply(DBusMessage *request, int deviceNameMode,
                             int topologyMode, bool initialConnected = false,
                             bool devicePresent = true,
                             bool initialPaired = true) -> DBusMessage * {
  DBusMessage *reply = dbus_message_new_method_return(request);
  DBusMessageIter out;
  DBusMessageIter objects;
  dbus_message_iter_init_append(reply, &out);
  dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
                                   &objects);
  struct Object {
    const char *path;
    const char *interface;
    bool adapter;
    const char *address;
    bool powered;
    bool connected;
  };
  std::vector<Object> objectsToAppend;

  if (topologyMode != 1) {
    objectsToAppend.push_back({kAdapterPath, "org.bluez.Adapter1", true,
                               nullptr, topologyMode != 3, false});

    if (devicePresent) {
      objectsToAppend.push_back({kDevicePath, "org.bluez.Device1", false,
                                 nullptr, true, initialConnected});
    }
  }

  if (topologyMode == 2 || topologyMode == 5) {
    objectsToAppend.push_back({"/org/bluez/hci1", "org.bluez.Adapter1", true,
                               "B8:F7:75:BF:F3:C4", true, false});
    objectsToAppend.push_back(
        {"/org/bluez/hci1/dev_11_22_33_44_55_66", "org.bluez.Device1", false,
         topologyMode == 5 ? nullptr : "11:22:33:44:55:66", true, true});
  }

  for (const auto &spec : objectsToAppend) {
    DBusMessageIter object;
    DBusMessageIter interfaces;
    const char *objectPath = spec.path;

    dbus_message_iter_open_container(&objects, DBUS_TYPE_DICT_ENTRY, nullptr,
                                     &object);
    dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH, &objectPath);
    dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                     &interfaces);
    appendInterface(interfaces, spec.interface, spec.adapter, deviceNameMode,
                    spec.address, spec.powered, spec.connected, initialPaired);
    dbus_message_iter_close_container(&object, &interfaces);
    dbus_message_iter_close_container(&objects, &object);
  }

  dbus_message_iter_close_container(&out, &objects);

  return reply;
}

class FakeBluezService {
public:
  explicit FakeBluezService(const std::string &address, int deviceNameMode = 0,
                            int topologyMode = 0, bool initialConnected = false)
      : m_deviceNameMode{deviceNameMode}, m_topologyMode{topologyMode},
        m_initialConnected{initialConnected},
        m_devicePresent{topologyMode != 4} {
    DbusErrorGuard error;
    m_connection.reset(
        dbus_connection_open_private(address.c_str(), error.get()));

    if (m_connection == nullptr ||
        !dbus_bus_register(m_connection.get(), error.get())) {
      throw std::runtime_error("unable to connect fake BlueZ service");
    }

    dbus_connection_set_exit_on_disconnect(m_connection.get(), FALSE);
    if (dbus_bus_request_name(m_connection.get(), kService,
                              DBUS_NAME_FLAG_DO_NOT_QUEUE, error.get()) !=
            DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
        !dbus_connection_add_filter(m_connection.get(), filter, this,
                                    nullptr)) {
      throw std::runtime_error("unable to register fake BlueZ service");
    }

    m_worker = std::thread{[this] { dispatch(); }};
  }

  ~FakeBluezService() {
    m_stopping = true;

    if (m_worker.joinable()) {
      m_worker.join();
    }
  }

  auto discoveryCounts() const -> std::pair<int, int> {
    std::lock_guard lock{m_mutex};

    return {m_startCount, m_stopCount};
  }

  auto forbiddenCount() const -> int {
    std::lock_guard lock{m_mutex};

    return m_forbiddenCount;
  }

  auto deviceActionCounts() const -> std::pair<int, int> {
    std::lock_guard lock{m_mutex};

    return {m_connectCount, m_disconnectCount};
  }

  auto pairCount() const -> int {
    std::lock_guard lock{m_mutex};

    return m_pairCount;
  }

  auto pairPath() const -> std::string {
    std::lock_guard lock{m_mutex};

    return m_pairPath;
  }

  void setInitialPaired(bool paired) {
    std::lock_guard lock{m_mutex};

    m_initialPaired = paired;
  }

  void setPairFailure(bool failure) {
    std::lock_guard lock{m_mutex};

    m_failPair = failure;
  }

  void setPairTransition(bool enabled) {
    std::lock_guard lock{m_mutex};

    m_transitionPaired = enabled;
  }

  auto deviceActionPaths() const -> std::pair<std::string, std::string> {
    std::lock_guard lock{m_mutex};

    return {m_connectPath, m_disconnectPath};
  }

  auto stopBeforeConnect() const -> bool {
    std::lock_guard lock{m_mutex};

    return m_stopSequence != 0 && m_connectSequence > m_stopSequence;
  }

  void setDeviceActionFailures(bool connectFailure, bool disconnectFailure) {
    std::lock_guard lock{m_mutex};

    m_failConnect = connectFailure;
    m_failDisconnect = disconnectFailure;
  }

  void setConnectionTransition(bool enabled) {
    std::lock_guard lock{m_mutex};

    m_transitionConnection = enabled;
  }

  void removeDeviceOnNextAction() {
    std::lock_guard lock{m_mutex};

    m_removeDeviceOnNextAction = true;
  }

  void releaseNameOnNextAction() {
    std::lock_guard lock{m_mutex};

    m_releaseNameOnNextAction = true;
  }

  void restartNameOnNextAction() {
    std::lock_guard lock{m_mutex};

    m_restartNameOnNextAction = true;
  }

  void holdNextDeviceAction() {
    std::lock_guard lock{m_mutex};

    m_holdNextDeviceAction = true;
  }

  auto waitForHeldDeviceAction(std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock{m_mutex};

    return m_snapshotCondition.wait_for(lock, timeout,
                                        [this] { return m_deviceActionHeld; });
  }

  void releaseHeldDeviceAction() {
    {
      std::lock_guard lock{m_mutex};

      m_releaseDeviceAction = true;
    }

    m_snapshotCondition.notify_all();
  }

  auto filterSignature() const -> std::string {
    std::lock_guard lock{m_mutex};

    return m_filterSignature;
  }

  auto filterIsUnrestrictedAuto() const -> bool {
    std::lock_guard lock{m_mutex};

    return m_filterIsUnrestrictedAuto;
  }

  auto discoveryPaths() const -> std::pair<std::string, std::string> {
    std::lock_guard lock{m_mutex};

    return {m_startPath, m_stopPath};
  }

  auto nameReleaseFailed() const -> bool {
    std::lock_guard lock{m_mutex};

    return m_nameOperationFailed;
  }

  void setManagedObjectsFailure(bool fail) {
    std::lock_guard lock{m_mutex};

    m_failManagedObjects = fail;
  }

  void setDiscoveryFailures(bool filterFailure, bool startFailure,
                            bool stopFailure) {
    std::lock_guard lock{m_mutex};

    m_failFilter = filterFailure;
    m_failStart = startFailure;
    m_failStop = stopFailure;
  }

  void releaseNameDuringNextSnapshot() {
    std::lock_guard lock{m_mutex};

    m_releaseNameDuringSnapshot = true;
  }

  void releaseName() {
    std::lock_guard lock{m_mutex};

    m_releaseNameRequested = true;
  }

  void acquireName() {
    std::lock_guard lock{m_mutex};

    m_acquireNameRequested = true;
  }

  void emitDeviceDuringNextSnapshot() {
    std::lock_guard lock{m_mutex};

    m_emitDeviceDuringSnapshot = true;
  }

  void holdNextSnapshotReply() {
    std::lock_guard lock{m_mutex};

    m_holdSnapshotReply = true;
  }

  auto waitForHeldSnapshot(std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lock{m_mutex};

    return m_snapshotCondition.wait_for(lock, timeout,
                                        [this] { return m_snapshotReplyHeld; });
  }

  void releaseHeldSnapshot() {
    {
      std::lock_guard lock{m_mutex};

      m_releaseSnapshotReply = true;
    }

    m_snapshotCondition.notify_all();
  }

  void emitConnected(bool connected, const std::string &path = kDevicePath) {
    emitBooleanProperty(path, "org.bluez.Device1", "Connected", connected);
  }

  void emitPropertyBurst(std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
      emitBooleanProperty(kDevicePath, "org.bluez.Device1", "Connected",
                          index % 2 == 0, false);
    }

    dbus_connection_flush(m_connection.get());
  }

  void emitAdapterPowered(bool powered) {
    emitBooleanProperty(kAdapterPath, "org.bluez.Adapter1", "Powered", powered);
  }

  void emitConnectedInvalidated(const std::string &path = kDevicePath) {
    DbusMessagePtr signal{dbus_message_new_signal(
        path.c_str(), "org.freedesktop.DBus.Properties", "PropertiesChanged")};
    DBusMessageIter out;
    DBusMessageIter changed;
    DBusMessageIter invalidated;
    const char *interface = "org.bluez.Device1";
    const char *property = "Connected";
    dbus_message_iter_init_append(signal.get(), &out);
    dbus_message_iter_append_basic(&out, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &changed);
    dbus_message_iter_close_container(&out, &changed);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING_AS_STRING, &invalidated);
    dbus_message_iter_append_basic(&invalidated, DBUS_TYPE_STRING, &property);
    dbus_message_iter_close_container(&out, &invalidated);
    dbus_connection_send(m_connection.get(), signal.get(), nullptr);
    dbus_connection_flush(m_connection.get());
  }

  void emitDeviceObject(bool added, const std::string &path = kDevicePath) {
    if (path == kDevicePath) {
      std::lock_guard lock{m_mutex};

      m_devicePresent = added;
    }

    emitObject(added, path, "org.bluez.Device1", false);
  }

  void emitAdapterObject(bool added) {
    emitObject(added, kAdapterPath, "org.bluez.Adapter1", true);
  }

private:
  void emitBooleanProperty(const std::string &path, const char *interface,
                           const char *property, bool value,
                           bool flush = true) {
    if (path == kDevicePath &&
        std::string_view{interface} == "org.bluez.Device1") {
      std::lock_guard lock{m_mutex};

      if (std::string_view{property} == "Connected") {
        m_initialConnected = value;
      } else if (std::string_view{property} == "Paired") {
        m_initialPaired = value;
      }
    }

    DbusMessagePtr signal{dbus_message_new_signal(
        path.c_str(), "org.freedesktop.DBus.Properties", "PropertiesChanged")};
    DBusMessageIter out;
    DBusMessageIter changed;
    DBusMessageIter invalidated;
    const dbus_bool_t encoded = value ? TRUE : FALSE;
    dbus_message_iter_init_append(signal.get(), &out);
    dbus_message_iter_append_basic(&out, DBUS_TYPE_STRING, &interface);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sv}", &changed);
    DBusMessageIter entry;
    DBusMessageIter variant;
    dbus_message_iter_open_container(&changed, DBUS_TYPE_DICT_ENTRY, nullptr,
                                     &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &property);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &encoded);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(&changed, &entry);
    dbus_message_iter_close_container(&out, &changed);
    dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                     DBUS_TYPE_STRING_AS_STRING, &invalidated);
    dbus_message_iter_close_container(&out, &invalidated);
    dbus_connection_send(m_connection.get(), signal.get(), nullptr);
    if (flush) {
      dbus_connection_flush(m_connection.get());
    }
  }

  void emitObject(bool added, const std::string &path, const char *interface,
                  bool adapter) {
    DbusMessagePtr signal{dbus_message_new_signal(
        "/", "org.freedesktop.DBus.ObjectManager",
        added ? "InterfacesAdded" : "InterfacesRemoved")};
    DBusMessageIter out;
    const char *objectPath = path.c_str();
    dbus_message_iter_init_append(signal.get(), &out);
    dbus_message_iter_append_basic(&out, DBUS_TYPE_OBJECT_PATH, &objectPath);
    if (added) {
      DBusMessageIter interfaces;
      dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                       &interfaces);
      appendInterface(interfaces, interface, adapter);
      dbus_message_iter_close_container(&out, &interfaces);
    } else {
      DBusMessageIter interfaces;
      dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                       DBUS_TYPE_STRING_AS_STRING, &interfaces);
      dbus_message_iter_append_basic(&interfaces, DBUS_TYPE_STRING, &interface);
      dbus_message_iter_close_container(&out, &interfaces);
    }

    dbus_connection_send(m_connection.get(), signal.get(), nullptr);
    dbus_connection_flush(m_connection.get());
  }

  static auto filter(DBusConnection *connection, DBusMessage *message,
                     void *data) -> DBusHandlerResult {
    auto *self = static_cast<FakeBluezService *>(data);
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    DBusMessage *reply{};
    if (dbus_message_is_method_call(message,
                                    "org.freedesktop.DBus.ObjectManager",
                                    "GetManagedObjects")) {
      bool failManagedObjects{};
      int topologyMode{};
      bool releaseNameDuringSnapshot{};
      bool emitDeviceDuringSnapshot{};
      bool holdSnapshotReply{};
      bool devicePresent{};
      bool initialPaired{};

      {
        std::lock_guard lock{self->m_mutex};

        failManagedObjects = self->m_failManagedObjects;
        topologyMode = self->m_topologyMode;
        devicePresent = self->m_devicePresent;
        initialPaired = self->m_initialPaired;
        releaseNameDuringSnapshot =
            std::exchange(self->m_releaseNameDuringSnapshot, false);
        emitDeviceDuringSnapshot =
            std::exchange(self->m_emitDeviceDuringSnapshot, false);
        holdSnapshotReply = std::exchange(self->m_holdSnapshotReply, false);
      }

      if (releaseNameDuringSnapshot) {
        DbusErrorGuard error;
        if (dbus_bus_release_name(connection, kService, error.get()) !=
                DBUS_RELEASE_NAME_REPLY_RELEASED ||
            dbus_error_is_set(error.get())) {
          reply = dbus_message_new_error(message, DBUS_ERROR_FAILED,
                                         "unable to release fake BlueZ name");
        }
      }

      if (emitDeviceDuringSnapshot) {
        self->emitDeviceObject(true);
      }

      if (holdSnapshotReply) {
        std::unique_lock lock{self->m_mutex};
        self->m_snapshotReplyHeld = true;
        self->m_snapshotCondition.notify_all();
        self->m_snapshotCondition.wait(
            lock, [self] { return self->m_releaseSnapshotReply; });
        self->m_snapshotReplyHeld = false;
        self->m_releaseSnapshotReply = false;
      }

      if (reply == nullptr) {
        reply = failManagedObjects
                    ? dbus_message_new_error(message, DBUS_ERROR_FAILED,
                                             "injected snapshot failure")
                    : makeManagedObjectsReply(message, self->m_deviceNameMode,
                                              topologyMode,
                                              self->m_initialConnected,
                                              devicePresent, initialPaired);
      }
    } else if (dbus_message_is_method_call(message, "org.bluez.Adapter1",
                                           "SetDiscoveryFilter")) {
      bool failFilter{};

      {
        std::lock_guard lock{self->m_mutex};

        self->m_filterSignature = dbus_message_get_signature(message);
        self->m_filterIsUnrestrictedAuto = filterIsUnrestrictedAuto(message);
        failFilter = self->m_failFilter;
      }

      reply = failFilter
                  ? dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED,
                                           "injected filter policy failure")
                  : dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, "org.bluez.Adapter1",
                                           "StartDiscovery")) {
      bool failStart{};

      {
        std::lock_guard lock{self->m_mutex};

        ++self->m_startCount;
        self->m_startPath = dbus_message_get_path(message);
        failStart = self->m_failStart;
      }

      reply = failStart
                  ? dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED,
                                           "injected discovery start failure")
                  : dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, "org.bluez.Adapter1",
                                           "StopDiscovery")) {
      bool failStop{};

      {
        std::lock_guard lock{self->m_mutex};

        ++self->m_stopCount;
        self->m_stopSequence = ++self->m_operationSequence;
        self->m_stopPath = dbus_message_get_path(message);
        failStop = self->m_failStop;
      }

      reply = failStop
                  ? dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED,
                                           "injected discovery stop failure")
                  : dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, "org.bluez.Device1",
                                           "Connect")) {
      bool failConnect{};
      bool transition{};
      bool removeDevice{};
      bool releaseName{};
      bool restartName{};
      bool holdAction{};
      const std::string devicePath{dbus_message_get_path(message)};

      {
        std::lock_guard lock{self->m_mutex};

        ++self->m_connectCount;
        self->m_connectPath = devicePath;
        self->m_connectSequence = ++self->m_operationSequence;
        failConnect = self->m_failConnect;
        transition = self->m_transitionConnection;
        removeDevice = std::exchange(self->m_removeDeviceOnNextAction, false);
        releaseName = std::exchange(self->m_releaseNameOnNextAction, false);
        restartName = std::exchange(self->m_restartNameOnNextAction, false);
        holdAction = std::exchange(self->m_holdNextDeviceAction, false);
      }

      if (holdAction) {
        std::unique_lock lock{self->m_mutex};
        self->m_deviceActionHeld = true;
        self->m_snapshotCondition.notify_all();
        self->m_snapshotCondition.wait(
            lock, [self] { return self->m_releaseDeviceAction; });
        self->m_deviceActionHeld = false;
        self->m_releaseDeviceAction = false;
      }

      if (removeDevice) {
        {
          std::lock_guard lock{self->m_mutex};

          self->m_devicePresent = false;
        }
        self->emitDeviceObject(false, devicePath);
      }

      if (releaseName) {
        DbusErrorGuard error;

        if (dbus_bus_release_name(connection, kService, error.get()) !=
                DBUS_RELEASE_NAME_REPLY_RELEASED ||
            dbus_error_is_set(error.get())) {
          std::lock_guard lock{self->m_mutex};

          self->m_nameOperationFailed = true;
        }
      }

      if (restartName) {
        DbusErrorGuard error;
        const auto released =
            dbus_bus_release_name(connection, kService, error.get());
        const auto acquired = dbus_bus_request_name(
            connection, kService, DBUS_NAME_FLAG_DO_NOT_QUEUE, error.get());
        if (released != DBUS_RELEASE_NAME_REPLY_RELEASED ||
            acquired != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER ||
            dbus_error_is_set(error.get())) {
          std::lock_guard lock{self->m_mutex};

          self->m_nameOperationFailed = true;
        }
      }

      reply = failConnect
                  ? dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED,
                                           "injected connect policy failure")
                  : dbus_message_new_method_return(message);
      if (!failConnect && transition && !removeDevice && !releaseName) {
        self->emitBooleanProperty(devicePath, "org.bluez.Device1", "Connected",
                                  true);
      }
    } else if (dbus_message_is_method_call(message, "org.bluez.Device1",
                                           "Disconnect")) {
      bool failDisconnect{};
      bool transition{};
      bool removeDevice{};
      bool holdAction{};
      const std::string devicePath{dbus_message_get_path(message)};
      {
        std::lock_guard lock{self->m_mutex};

        ++self->m_disconnectCount;
        self->m_disconnectPath = devicePath;
        failDisconnect = self->m_failDisconnect;
        transition = self->m_transitionConnection;
        removeDevice = std::exchange(self->m_removeDeviceOnNextAction, false);
        holdAction = std::exchange(self->m_holdNextDeviceAction, false);
      }

      if (holdAction) {
        std::unique_lock lock{self->m_mutex};
        self->m_deviceActionHeld = true;
        self->m_snapshotCondition.notify_all();
        self->m_snapshotCondition.wait(
            lock, [self] { return self->m_releaseDeviceAction; });
        self->m_deviceActionHeld = false;
        self->m_releaseDeviceAction = false;
      }

      if (removeDevice) {

        {
          std::lock_guard lock{self->m_mutex};

          self->m_devicePresent = false;
        }

        self->emitDeviceObject(false, devicePath);
      }

      reply = failDisconnect
                  ? dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED,
                                           "injected disconnect policy failure")
                  : dbus_message_new_method_return(message);
      if (!failDisconnect && transition && !removeDevice) {
        self->emitBooleanProperty(devicePath, "org.bluez.Device1", "Connected",
                                  false);
      }
    } else if (dbus_message_is_method_call(message, "org.bluez.Device1",
                                           "Pair")) {
      bool failPair{};
      bool transition{};
      bool holdAction{};
      const std::string devicePath{dbus_message_get_path(message)};
      {
        std::lock_guard lock{self->m_mutex};

        ++self->m_pairCount;
        self->m_pairPath = devicePath;
        failPair = self->m_failPair;
        transition = self->m_transitionPaired;
        holdAction = std::exchange(self->m_holdNextDeviceAction, false);
      }

      if (holdAction) {
        std::unique_lock lock{self->m_mutex};
        self->m_deviceActionHeld = true;
        self->m_snapshotCondition.notify_all();
        self->m_snapshotCondition.wait(
            lock, [self] { return self->m_releaseDeviceAction; });
        self->m_deviceActionHeld = false;
        self->m_releaseDeviceAction = false;
      }

      if (!failPair && transition) {
        std::lock_guard lock{self->m_mutex};

        self->m_initialPaired = true;
      }

      reply = failPair
                  ? dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED,
                                           "injected pairing agent failure")
                  : dbus_message_new_method_return(message);
      if (!failPair && transition) {
        self->emitBooleanProperty(devicePath, "org.bluez.Device1", "Paired",
                                  true);
      }
    } else {
      {
        std::lock_guard lock{self->m_mutex};

        ++self->m_forbiddenCount;
      }

      reply = dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_METHOD,
                                     "operation is not allowed in this test");
    }

    if (reply != nullptr) {
      dbus_connection_send(connection, reply, nullptr);
      dbus_message_unref(reply);
      dbus_connection_flush(connection);
    }

    return DBUS_HANDLER_RESULT_HANDLED;
  }

  static auto filterIsUnrestrictedAuto(DBusMessage *message) -> bool {
    DBusMessageIter arguments;
    DBusMessageIter entries;
    if (!dbus_message_iter_init(message, &arguments) ||
        dbus_message_iter_get_arg_type(&arguments) != DBUS_TYPE_ARRAY) {
      return false;
    }

    dbus_message_iter_recurse(&arguments, &entries);
    if (dbus_message_iter_get_arg_type(&entries) != DBUS_TYPE_DICT_ENTRY) {
      return false;
    }

    DBusMessageIter entry;
    dbus_message_iter_recurse(&entries, &entry);
    const char *key{};
    dbus_message_iter_get_basic(&entry, &key);
    if (key == nullptr || std::string{key} != "Transport" ||
        !dbus_message_iter_next(&entry) ||
        dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
      return false;
    }

    DBusMessageIter value;
    dbus_message_iter_recurse(&entry, &value);
    const char *transport{};
    if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_STRING) {
      return false;
    }

    dbus_message_iter_get_basic(&value, &transport);

    return transport != nullptr && std::string{transport} == "auto" &&
           !dbus_message_iter_next(&entries);
  }

  void dispatch() {
    while (!m_stopping) {
      bool releaseNameRequested{};
      bool acquireNameRequested{};

      {
        std::lock_guard lock{m_mutex};

        releaseNameRequested = std::exchange(m_releaseNameRequested, false);
        acquireNameRequested = std::exchange(m_acquireNameRequested, false);
      }

      if (releaseNameRequested || acquireNameRequested) {
        DbusErrorGuard error;
        const auto result = releaseNameRequested
                                ? dbus_bus_release_name(m_connection.get(),
                                                        kService, error.get())
                                : dbus_bus_request_name(
                                      m_connection.get(), kService,
                                      DBUS_NAME_FLAG_DO_NOT_QUEUE, error.get());
        const auto expected = releaseNameRequested
                                  ? DBUS_RELEASE_NAME_REPLY_RELEASED
                                  : DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
        if (result != expected || dbus_error_is_set(error.get())) {
          std::lock_guard lock{m_mutex};

          m_nameOperationFailed = true;
        }
      }

      dbus_connection_read_write_dispatch(m_connection.get(), 20);
    }
  }

  DbusConnectionPtr m_connection;
  std::atomic_bool m_stopping{};
  std::thread m_worker;
  mutable std::mutex m_mutex;
  std::condition_variable m_snapshotCondition;
  int m_startCount{};
  int m_stopCount{};
  int m_forbiddenCount{};
  int m_connectCount{};
  int m_disconnectCount{};
  int m_pairCount{};
  int m_operationSequence{};
  int m_stopSequence{};
  int m_connectSequence{};
  std::string m_filterSignature;
  bool m_filterIsUnrestrictedAuto{};
  int m_deviceNameMode{};
  int m_topologyMode{};
  bool m_initialConnected{};
  bool m_initialPaired{true};
  bool m_devicePresent{true};
  bool m_failConnect{};
  bool m_failDisconnect{};
  bool m_failPair{};
  bool m_transitionConnection{true};
  bool m_transitionPaired{true};
  bool m_failManagedObjects{};
  bool m_failFilter{};
  bool m_failStart{};
  bool m_failStop{};
  bool m_releaseNameDuringSnapshot{};
  bool m_releaseNameRequested{};
  bool m_acquireNameRequested{};
  bool m_nameOperationFailed{};
  bool m_emitDeviceDuringSnapshot{};
  bool m_holdSnapshotReply{};
  bool m_snapshotReplyHeld{};
  bool m_releaseSnapshotReply{};
  bool m_removeDeviceOnNextAction{};
  bool m_releaseNameOnNextAction{};
  bool m_restartNameOnNextAction{};
  bool m_holdNextDeviceAction{};
  bool m_deviceActionHeld{};
  bool m_releaseDeviceAction{};
  std::string m_startPath;
  std::string m_stopPath;
  std::string m_connectPath;
  std::string m_disconnectPath;
  std::string m_pairPath;
};

auto makeClientConfig(const std::string &address)
    -> dmn::Dmn_BluezClientConfig {
  dmn::Dmn_BluezClientConfig config;
  config.test_bus_address = address;

  return config;
}

struct CliProcess {
  pid_t pid{-1};
  int outputFd{-1};
  int status{};
  bool exited{};
  std::string output;

  CliProcess() = default;
  CliProcess(const CliProcess &) = delete;
  auto operator=(const CliProcess &) -> CliProcess & = delete;
  CliProcess(CliProcess &&other) noexcept
      : pid{std::exchange(other.pid, -1)},
        outputFd{std::exchange(other.outputFd, -1)}, status{other.status},
        exited{other.exited}, output{std::move(other.output)} {}
  ~CliProcess() {
    if (pid > 0 && !exited) {
      (void)kill(pid, SIGKILL);
      (void)waitpid(pid, &status, 0);
    }

    if (outputFd >= 0) {
      close(outputFd);
    }
  }
};

auto startCli(const std::string &busAddress,
              const std::vector<std::string> &arguments) -> CliProcess {
  int descriptors[2]{};
  if (pipe(descriptors) != 0) {
    throw std::runtime_error("unable to create CLI output pipe");
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    throw std::runtime_error("unable to initialize CLI spawn actions");
  }

  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, descriptors[0]);
  posix_spawn_file_actions_addclose(&actions, descriptors[1]);

  std::vector<std::string> argumentStorage{DMN_BLUEZ_CLI_EXECUTABLE};
  argumentStorage.insert(argumentStorage.end(), arguments.begin(),
                         arguments.end());
  std::vector<char *> argumentVector;
  for (auto &argument : argumentStorage) {
    argumentVector.push_back(argument.data());
  }

  argumentVector.push_back(nullptr);

  std::vector<std::string> environmentStorage;
  constexpr std::string_view testBusPrefix{"DMN_BLUEZ_TEST_BUS_ADDRESS="};
  for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (std::string_view{*entry}.starts_with(testBusPrefix)) {
      continue;
    }

    environmentStorage.emplace_back(*entry);
  }

  environmentStorage.push_back(std::string{testBusPrefix} + busAddress);
  std::vector<char *> environmentVector;
  for (auto &entry : environmentStorage) {
    environmentVector.push_back(entry.data());
  }

  environmentVector.push_back(nullptr);

  CliProcess process;
  const int spawnResult =
      posix_spawn(&process.pid, DMN_BLUEZ_CLI_EXECUTABLE, &actions, nullptr,
                  argumentVector.data(), environmentVector.data());
  posix_spawn_file_actions_destroy(&actions);
  close(descriptors[1]);
  if (spawnResult != 0) {
    close(descriptors[0]);
    throw std::system_error(spawnResult, std::generic_category(),
                            "unable to start discovery CLI test process");
  }

  process.outputFd = descriptors[0];
  if (fcntl(process.outputFd, F_SETFL,
            fcntl(process.outputFd, F_GETFL) | O_NONBLOCK) < 0) {
    (void)kill(process.pid, SIGKILL);
    (void)waitpid(process.pid, nullptr, 0);
    close(process.outputFd);
    process.pid = -1;
    process.outputFd = -1;
    throw std::runtime_error("unable to configure CLI output pipe");
  }

  return process;
}

void collectCliOutput(CliProcess &process) {
  char buffer[4096];
  while (true) {
    const auto count = read(process.outputFd, buffer, sizeof(buffer));
    if (count > 0) {
      process.output.append(buffer, static_cast<std::size_t>(count));
      continue;
    }

    if (count < 0 && errno == EINTR) {
      continue;
    }

    return;
  }
}

auto waitForCliText(CliProcess &process, std::string_view expected,
                    std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    collectCliOutput(process);
    if (process.output.find(expected) != std::string::npos) {
      return true;
    }

    pollfd descriptor{process.outputFd, POLLIN | POLLHUP, 0};
    (void)poll(&descriptor, 1, 20);
    const auto waited = waitpid(process.pid, &process.status, WNOHANG);
    if (waited == process.pid) {
      process.exited = true;
      collectCliOutput(process);

      return process.output.find(expected) != std::string::npos;
    }
  }

  collectCliOutput(process);

  return process.output.find(expected) != std::string::npos;
}

auto waitForCliExit(CliProcess &process,
                    std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    collectCliOutput(process);
    if (!process.exited) {
      const auto waited = waitpid(process.pid, &process.status, WNOHANG);
      if (waited == process.pid) {
        process.exited = true;
      }
    }

    if (process.exited) {
      collectCliOutput(process);

      return true;
    }

    pollfd descriptor{process.outputFd, POLLIN | POLLHUP, 0};
    (void)poll(&descriptor, 1, 20);
  }

  return false;
}

TEST(DmnBluezDeviceList, IncludesKnownDisconnectedDevices) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};

  const auto adapters = client.snapshot();
  ASSERT_TRUE(adapters) << adapters.error().message;
  ASSERT_EQ(adapters->adapters.size(), 1U);
  EXPECT_EQ(adapters->adapters.front().name, "Test adapter");
  EXPECT_EQ(adapters->adapters.front().address, "B8:F7:75:BF:F3:C3");
  ASSERT_EQ(adapters->devices.size(), 1U);
  const auto device = adapters->devices.front();
  EXPECT_EQ(device.name, "Known headset");
  EXPECT_EQ(device.address, "AA:BB:CC:DD:EE:FF");
  EXPECT_TRUE(device.paired.value());
  EXPECT_FALSE(device.connected.value());
  EXPECT_EQ(device.rssi.value(), -47);
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezDeviceList, EnumeratesEmptyAndMultipleAdapterInventories) {
  {
    PrivateBus bus;
    ASSERT_TRUE(bus.valid());
    FakeBluezService service{bus.address(), 0, 1};
    dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};

    const auto inventory = client.snapshot();
    ASSERT_TRUE(inventory) << inventory.error().message;
    EXPECT_TRUE(inventory->adapters.empty());
    EXPECT_TRUE(inventory->devices.empty());
    EXPECT_EQ(service.forbiddenCount(), 0);
  }

  {
    PrivateBus bus;
    ASSERT_TRUE(bus.valid());
    FakeBluezService service{bus.address(), 0, 2};
    dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};

    const auto inventory = client.snapshot();
    ASSERT_TRUE(inventory) << inventory.error().message;
    ASSERT_EQ(inventory->adapters.size(), 2U);
    ASSERT_EQ(inventory->devices.size(), 2U);
    EXPECT_NE(inventory->adapters[0].object_path,
              inventory->adapters[1].object_path);
    EXPECT_NE(inventory->devices[0].adapter_path,
              inventory->devices[1].adapter_path);
    EXPECT_TRUE(inventory->devices[0].paired.value());
    EXPECT_FALSE(inventory->devices[0].connected.value());
    EXPECT_TRUE(inventory->devices[1].paired.value());
    EXPECT_TRUE(inventory->devices[1].connected.value());
    EXPECT_EQ(service.forbiddenCount(), 0);
  }
}

TEST(DmnBluezDeviceList, FallsBackFromAliasToNameAndAddress) {
  for (const auto &[mode, expected] :
       {std::pair{1, "Reported headset"}, std::pair{2, "AA:BB:CC:DD:EE:FF"}}) {
    PrivateBus bus;
    ASSERT_TRUE(bus.valid());
    FakeBluezService service{bus.address(), mode};
    dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
    const auto inventory = client.snapshot();
    ASSERT_TRUE(inventory) << inventory.error().message;
    ASSERT_EQ(inventory->devices.size(), 1U);
    EXPECT_EQ(inventory->devices.front().name, expected);
  }
}

TEST(DmnBluezCli, SnapshotPrintsPortableCsvAndDoesNotStartDiscovery) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 3};
  auto process = startCli(bus.address(), {});
  ASSERT_TRUE(waitForCliExit(process, 15s));

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_NE(process.output.find(
                "action,adapter_name,adapter_address,adapter_path,"
                "adapter_powered,device_address,name,alias,reported_name,"
                "paired,connected,services_resolved,rssi,device_path,"
                "seen_during_discovery\r\n"),
            std::string::npos);
  EXPECT_NE(process.output.find(
                "inventory,Test adapter,B8:F7:75:BF:F3:C3,/org/bluez/hci0,"
                "true,AA:BB:CC:DD:EE:FF,\"Headset, \"\"One\"\"\","
                "\"Headset, \"\"One\"\"\",,true,false,,-47,"
                "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF,false"),
            std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.pairCount(), 0);
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ConnectKnownDeviceWaitsForObservedConnectedState) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(), {"--connect", "aa:bb:cc:dd:ee:ff"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("changed,"), std::string::npos);
  EXPECT_NE(process.output.find(
                "connected,Test adapter,B8:F7:75:BF:F3:C3,/org/bluez/hci0,"
                "true,AA:BB:CC:DD:EE:FF"),
            std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
  EXPECT_EQ(service.deviceActionPaths(),
            (std::pair{std::string{kDevicePath}, std::string{}}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ConnectAlreadyConnectedIsNoOp) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 0, true};
  auto process = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  ASSERT_EQ(WEXITSTATUS(process.status), 0) << process.output;
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("changed,"), std::string::npos);
  EXPECT_NE(process.output.find("connected,Test adapter"), std::string::npos)
      << process.output;
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, DisconnectAlreadyDisconnectedIsNoOp) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(), {"--disconnect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("changed,"), std::string::npos);
  EXPECT_NE(process.output.find("disconnected,Test adapter"),
            std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
}

TEST(DmnBluezCli, DisconnectConnectedDeviceWaitsForObservedState) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 0, true};
  auto process =
      startCli(bus.address(), {"--disconnect", "/org/bluez/hci0/"
                                               "dev_AA_BB_CC_DD_EE_FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("changed,"), std::string::npos);
  EXPECT_NE(process.output.find(
                "disconnected,Test adapter,B8:F7:75:BF:F3:C3,/org/bluez/hci0,"
                "true,AA:BB:CC:DD:EE:FF"),
            std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 1}));
  EXPECT_EQ(service.deviceActionPaths(),
            (std::pair{std::string{}, std::string{kDevicePath}}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, PairsSelectedDeviceUsingBluezDefaultAgent) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setInitialPaired(false);
  auto process = startCli(bus.address(), {"--pair", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("changed,"), std::string::npos);
  EXPECT_NE(process.output.find("paired,Test adapter"), std::string::npos);
  EXPECT_EQ(service.pairCount(), 1);
  EXPECT_EQ(service.pairPath(), kDevicePath);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, PairAlreadyPairedDeviceIsNoOp) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(), {"--pair", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("changed,"), std::string::npos);
  EXPECT_NE(process.output.find("paired,Test adapter"), std::string::npos);
  EXPECT_EQ(service.pairCount(), 0);
}

TEST(DmnBluezCli, ReportsPairingAgentFailureWithoutSuccessRow) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setInitialPaired(false);
  service.setPairFailure(true);
  auto process = startCli(bus.address(), {"--pair", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 10);
  EXPECT_NE(process.output.find("injected pairing agent failure"),
            std::string::npos);
  EXPECT_EQ(process.output.find("paired,Test adapter"), std::string::npos);
  EXPECT_EQ(service.pairCount(), 1);
}

TEST(DmnBluezCli, PairTimesOutWithoutObservedPairedState) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setInitialPaired(false);
  service.setPairTransition(false);
  auto process = startCli(bus.address(), {"--pair", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 5s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 11);
  EXPECT_NE(process.output.find("did not reach the requested paired state"),
            std::string::npos);
  EXPECT_EQ(process.output.find("paired,Test adapter"), std::string::npos);
  EXPECT_EQ(service.pairCount(), 1);
}

TEST(DmnBluezCli, ConnectCanDiscoverOnlyTheSelectedDevice) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 4};
  auto process = startCli(bus.address(),
                          {"--connect", "AA:BB:CC:DD:EE:FF", "--discovery"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active while waiting", 15s))
      << process.output;
  service.emitDeviceObject(true);
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  ASSERT_EQ(WEXITSTATUS(process.status), 0) << process.output;
  EXPECT_EQ(process.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(process.output.find("added,"), std::string::npos);
  EXPECT_NE(process.output.find("connected,Test adapter"), std::string::npos)
      << process.output;
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
  EXPECT_TRUE(service.stopBeforeConnect());
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, DiscoveryStopFailurePreventsConnect) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 4};
  auto process = startCli(bus.address(),
                          {"--connect", "AA:BB:CC:DD:EE:FF", "--discovery"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active while waiting", 15s))
      << process.output;
  service.setDiscoveryFailures(false, false, true);
  service.emitDeviceObject(true);
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 5);
  EXPECT_NE(process.output.find("Unable to stop discovery"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 2}));
}

TEST(DmnBluezCli, InterruptedTargetDiscoveryStopsWithoutConnecting) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 4};
  auto process = startCli(bus.address(),
                          {"--connect", "AA:BB:CC:DD:EE:FF", "--discovery"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active while waiting", 15s))
      << process.output;
  ASSERT_EQ(kill(process.pid, SIGINT), 0);
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 11);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
}

TEST(DmnBluezCli, DuplicateDeviceAddressRequiresAdapterSelection) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 5};

  auto ambiguous = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(ambiguous, 15s));
  ASSERT_TRUE(WIFEXITED(ambiguous.status));
  EXPECT_EQ(WEXITSTATUS(ambiguous.status), 9);
  EXPECT_NE(ambiguous.output.find("Device selection is ambiguous"),
            std::string::npos);

  auto selected = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF",
                                           "--adapter", "B8:F7:75:BF:F3:C4"});
  ASSERT_TRUE(waitForCliExit(selected, 15s)) << selected.output;
  ASSERT_TRUE(WIFEXITED(selected.status));
  EXPECT_EQ(WEXITSTATUS(selected.status), 0);
  EXPECT_EQ(selected.output.find("inventory,"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ReportsConnectPolicyFailureWithoutSuccessRow) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setDeviceActionFailures(true, false);
  auto process = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 10);
  EXPECT_NE(process.output.find("injected connect policy failure"),
            std::string::npos);
  EXPECT_EQ(process.output.find("connected,Test adapter"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ReportsDisconnectPolicyFailureWithoutSuccessRow) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 0, true};
  service.setDeviceActionFailures(false, true);
  auto process = startCli(bus.address(), {"--disconnect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 10);
  EXPECT_NE(process.output.find("injected disconnect policy failure"),
            std::string::npos);
  EXPECT_EQ(process.output.find("disconnected,Test adapter"),
            std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 1}));
}

TEST(DmnBluezCli, ReportsTargetRemovalAfterConnectWithoutWaitingForTimeout) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.removeDeviceOnNextAction();
  auto process = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 9);
  EXPECT_NE(process.output.find("Selected device disappeared"),
            std::string::npos);
  EXPECT_EQ(process.output.find("connected,Test adapter"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
}

TEST(DmnBluezCli, ReportsTargetRemovalAfterDisconnectWithoutWaitingForTimeout) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 0, true};
  service.removeDeviceOnNextAction();
  auto process = startCli(bus.address(), {"--disconnect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 9);
  EXPECT_NE(process.output.find("Selected device disappeared"),
            std::string::npos);
  EXPECT_EQ(process.output.find("disconnected,Test adapter"),
            std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 1}));
}

TEST(DmnBluezCli, ReportsBluezLossAfterConnectWithoutSuccessRow) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.releaseNameOnNextAction();
  auto process = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 4);
  EXPECT_EQ(process.output.find("connected,Test adapter"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
  EXPECT_FALSE(service.nameReleaseFailed());
}

TEST(DmnBluezCli, ReportsBluezRestartAfterConnectWithoutSuccessRow) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.restartNameOnNextAction();
  auto process = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 4);
  EXPECT_EQ(process.output.find("connected,Test adapter"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
  EXPECT_FALSE(service.nameReleaseFailed());
}

TEST(DmnBluezCli, ConnectTimesOutWithoutObservedConnectedState) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setConnectionTransition(false);
  auto process = startCli(bus.address(), {"--connect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 5s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 11);
  EXPECT_NE(process.output.find("did not reach the requested connected state"),
            std::string::npos);
  EXPECT_EQ(process.output.find("connected,Test adapter"), std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
}

TEST(DmnBluezCli, DisconnectTimesOutWithoutObservedDisconnectedState) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 0, true};
  service.setConnectionTransition(false);
  auto process = startCli(bus.address(), {"--disconnect", "AA:BB:CC:DD:EE:FF"});
  ASSERT_TRUE(waitForCliExit(process, 5s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 11);
  EXPECT_NE(
      process.output.find("did not reach the requested disconnected state"),
      std::string::npos);
  EXPECT_EQ(process.output.find("disconnected,Test adapter"),
            std::string::npos);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 1}));
}

TEST(DmnBluezCli, RejectsInvalidConnectionOptionCombinations) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};

  for (const auto &arguments :
       {std::vector<std::string>{"--connect"},
        std::vector<std::string>{"--pair"},
        std::vector<std::string>{"--connect", "AA:BB:CC:DD:EE:FF", "--connect",
                                 "AA:BB:CC:DD:EE:FF"},
        std::vector<std::string>{"--pair", "AA:BB:CC:DD:EE:FF", "--pair",
                                 "AA:BB:CC:DD:EE:FF"},
        std::vector<std::string>{"--connect", "AA:BB:CC:DD:EE:FF",
                                 "--disconnect", "AA:BB:CC:DD:EE:FF"},
        std::vector<std::string>{"--disconnect", "AA:BB:CC:DD:EE:FF",
                                 "--discovery"},
        std::vector<std::string>{"--pair", "AA:BB:CC:DD:EE:FF",
                                 "--discovery"}}) {
    auto process = startCli(bus.address(), arguments);
    ASSERT_TRUE(waitForCliExit(process, 15s));
    ASSERT_TRUE(WIFEXITED(process.status));
    EXPECT_EQ(WEXITSTATUS(process.status), 2);
    EXPECT_NE(process.output.find("Usage: dmn-bluez-discovery"),
              std::string::npos);
  }

  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.pairCount(), 0);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{0, 0}));
}

TEST(DmnBluezCli, RequiresExplicitAdapterWhenMultipleArePowered) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 2};
  auto process = startCli(bus.address(), {"--discovery"});
  ASSERT_TRUE(waitForCliExit(process, 15s));

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 6);
  EXPECT_NE(process.output.find("Multiple powered adapters"),
            std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ExplicitAdapterSelectsOnlyMatchingPoweredAdapter) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 2};

  for (const auto &selector :
       {std::string{"B8:F7:75:BF:F3:C4"}, std::string{"/org/bluez/hci1"}}) {
    auto process =
        startCli(bus.address(), {"--discovery", "--adapter", selector});
    ASSERT_TRUE(waitForCliText(process, "Discovery active", 15s))
        << process.output;
    ASSERT_EQ(kill(process.pid, SIGINT), 0);
    ASSERT_TRUE(waitForCliExit(process, 15s));

    ASSERT_TRUE(WIFEXITED(process.status));
    EXPECT_EQ(WEXITSTATUS(process.status), 0);
  }

  EXPECT_EQ(service.discoveryCounts(), (std::pair{2, 2}));
  EXPECT_EQ(service.discoveryPaths(),
            (std::pair{std::string{"/org/bluez/hci1"},
                       std::string{"/org/bluez/hci1"}}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, NoUsableOrUnpoweredAdapterDoesNotStartDiscovery) {
  for (const auto &[topologyMode, arguments, exitCode, expected] :
       {std::tuple{1, std::vector<std::string>{"--discovery"}, 3,
                   std::string{"No powered Bluetooth adapter"}},
        std::tuple{3,
                   std::vector<std::string>{"--discovery", "--adapter",
                                            std::string{kAdapterPath}},
                   7, std::string{"No powered adapter matches"}}}) {
    PrivateBus bus;
    ASSERT_TRUE(bus.valid());
    FakeBluezService service{bus.address(), 0, topologyMode};
    auto process = startCli(bus.address(), arguments);
    ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

    ASSERT_TRUE(WIFEXITED(process.status));
    EXPECT_EQ(WEXITSTATUS(process.status), exitCode);
    EXPECT_NE(process.output.find(expected), std::string::npos);
    EXPECT_EQ(service.discoveryCounts(), (std::pair{0, 0}));
    EXPECT_EQ(service.forbiddenCount(), 0);
  }
}

TEST(DmnBluezCli, ScanStopsOwnedSessionOnInterrupt) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(), {"--discovery"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active", 15s))
      << process.output;
  service.emitConnected(true);
  ASSERT_TRUE(
      waitForCliText(process,
                     "changed,Test adapter,B8:F7:75:BF:F3:C3,/org/bluez/hci0,"
                     "true,AA:BB:CC:DD:EE:FF,Known headset,",
                     15s))
      << process.output;
  ASSERT_EQ(kill(process.pid, SIGINT), 0);
  ASSERT_TRUE(waitForCliExit(process, 15s));

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_NE(process.output.find("action,adapter_name"), std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ScanStopsOwnedSessionOnSigterm) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(), {"--discovery"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active", 15s))
      << process.output;
  ASSERT_EQ(kill(process.pid, SIGTERM), 0);
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
}

TEST(DmnBluezCli, ReportsBluezLossDuringScanWithoutStaleSuccess) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process =
      startCli(bus.address(), {"--discovery", "--adapter", kAdapterPath});
  ASSERT_TRUE(waitForCliText(process, "Discovery active", 15s))
      << process.output;
  service.releaseName();
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 4);
  EXPECT_NE(process.output.find("BlueZ service owner disappeared"),
            std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 0}));
  EXPECT_FALSE(service.nameReleaseFailed());
}

TEST(DmnBluezCli, ReportsDiscoveryStartFailure) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setDiscoveryFailures(false, true, false);
  auto process = startCli(bus.address(),
                          {"--discovery", "--adapter", "B8:F7:75:BF:F3:C3"});
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 5);
  EXPECT_NE(process.output.find("Unable to start discovery"),
            std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezCli, ReportsDiscoveryStopFailureOnTermination) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(),
                          {"--discovery", "--adapter", "B8:F7:75:BF:F3:C3"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active", 15s))
      << process.output;
  service.setDiscoveryFailures(false, false, true);
  ASSERT_EQ(kill(process.pid, SIGINT), 0);
  ASSERT_TRUE(waitForCliExit(process, 15s)) << process.output;

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 5);
  EXPECT_NE(process.output.find("Unable to stop discovery"), std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 2}));
}

TEST(DmnBluezCli, RejectsInvalidArgumentsWithUsage) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());

  for (const auto &arguments :
       {std::vector<std::string>{"--unknown"},
        std::vector<std::string>{"--adapter"},
        std::vector<std::string>{"--discovery", "--discovery"},
        std::vector<std::string>{"--adapter", "B8:F7:75:BF:F3:C3", "--adapter",
                                 "B8:F7:75:BF:F3:C3"}}) {
    auto process = startCli(bus.address(), arguments);
    ASSERT_TRUE(waitForCliExit(process, 15s));

    ASSERT_TRUE(WIFEXITED(process.status));
    EXPECT_EQ(WEXITSTATUS(process.status), 2);
    EXPECT_NE(process.output.find("Usage: dmn-bluez-discovery"),
              std::string::npos);
  }
}

TEST(DmnBluezCli, HelpPrintsUsageWithoutOpeningBluezConnection) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(), {"--help"});
  ASSERT_TRUE(waitForCliExit(process, 15s));

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_NE(process.output.find("Usage: dmn-bluez-discovery"),
            std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezDiscovery, StartsAndStopsOnlyItsOwnedSession) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  ASSERT_TRUE(client.startDiscovery(kAdapterPath));
  ASSERT_TRUE(client.stopDiscovery());
  ASSERT_TRUE(client.stopDiscovery());

  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
  EXPECT_EQ(service.filterSignature(), "a{sv}");
  EXPECT_TRUE(service.filterIsUnrestrictedAuto());
  EXPECT_EQ(service.discoveryPaths(),
            (std::pair{std::string{kAdapterPath}, std::string{kAdapterPath}}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezDiscovery, RejectsUnpoweredAdapterWithoutCallingBluez) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 3};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  const auto started = client.startDiscovery(kAdapterPath);
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().kind, dmn::Dmn_DbusErrorKind::InvalidArgument);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{0, 0}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezDiscovery, StartFailuresDoNotAcquireOwnedSession) {
  for (const auto &[filterFailure, startFailure] :
       {std::pair{true, false}, std::pair{false, true}}) {
    PrivateBus bus;
    ASSERT_TRUE(bus.valid());
    FakeBluezService service{bus.address()};
    service.setDiscoveryFailures(filterFailure, startFailure, false);
    dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
    ASSERT_TRUE(client.snapshot());

    const auto started = client.startDiscovery(kAdapterPath);
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().kind, dmn::Dmn_DbusErrorKind::Remote);
    EXPECT_EQ(started.error().remote_name, DBUS_ERROR_ACCESS_DENIED);
    EXPECT_NE(started.error().message.find(
                  filterFailure ? "injected filter policy failure"
                                : "injected discovery start failure"),
              std::string::npos);
    ASSERT_TRUE(client.stopDiscovery());
    EXPECT_EQ(service.discoveryCounts(), (std::pair{startFailure ? 1 : 0, 0}));
  }
}

TEST(DmnBluezDiscovery, StopFailureRetainsOwnershipForRetry) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());
  ASSERT_TRUE(client.startDiscovery(kAdapterPath));

  service.setDiscoveryFailures(false, false, true);
  const auto firstStop = client.stopDiscovery();
  ASSERT_FALSE(firstStop);
  EXPECT_EQ(firstStop.error().kind, dmn::Dmn_DbusErrorKind::Remote);
  EXPECT_EQ(firstStop.error().remote_name, DBUS_ERROR_ACCESS_DENIED);
  EXPECT_NE(firstStop.error().message.find("injected discovery stop failure"),
            std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));

  service.setDiscoveryFailures(false, false, false);
  ASSERT_TRUE(client.stopDiscovery());
  ASSERT_TRUE(client.stopDiscovery());
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 2}));
}

TEST(DmnBluezDiscovery, SnapshotFailureDoesNotForgetOwnedSession) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());
  ASSERT_TRUE(client.startDiscovery(kAdapterPath));

  service.setManagedObjectsFailure(true);
  EXPECT_FALSE(client.snapshot());
  service.setManagedObjectsFailure(false);
  ASSERT_TRUE(client.stopDiscovery());

  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
}

TEST(DmnBluezDeviceList, OwnerLossDuringSnapshotKeepsInventoryUnavailable) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  service.releaseNameDuringNextSnapshot();

  const auto inventory = client.snapshot();
  ASSERT_FALSE(inventory);
  EXPECT_EQ(inventory.error().kind, dmn::Dmn_DbusErrorKind::NotConnected);
  EXPECT_TRUE(client.devices().empty());
}

TEST(DmnBluezDeviceList, ReloadsInventoryAfterBluezOwnerRestart) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  bool ownerLost{};
  bool inventoryReadyAfterRestart{};

  client.setEventHandler([&](const auto &event) {
    std::lock_guard lock{mutex};

    ownerLost |= event.kind == dmn::Dmn_BluezEventKind::ServiceUnavailable;
    inventoryReadyAfterRestart |=
        ownerLost && event.kind == dmn::Dmn_BluezEventKind::ServiceAvailable;
    condition.notify_all();
  });

  service.releaseName();

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return ownerLost; }));
  }

  EXPECT_TRUE(client.devices().empty());

  service.acquireName();

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 5s,
                                   [&] { return inventoryReadyAfterRestart; }));
  }

  const auto inventory = client.snapshot();
  ASSERT_TRUE(inventory) << inventory.error().message;
  ASSERT_EQ(inventory->adapters.size(), 1U);
  ASSERT_EQ(inventory->devices.size(), 1U);
  EXPECT_FALSE(service.nameReleaseFailed());
}

TEST(DmnBluezDeviceList, SnapshotReplayIsNotDeliveredAsLiveDuplicate) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  std::mutex mutex;
  std::condition_variable condition;
  int addedEvents{};
  int changedEvents{};

  client.setEventHandler([&](const auto &event) {
    std::lock_guard lock{mutex};

    addedEvents += event.kind == dmn::Dmn_BluezEventKind::DeviceAdded;
    changedEvents += event.kind == dmn::Dmn_BluezEventKind::DeviceChanged;
    condition.notify_all();
  });

  service.emitDeviceDuringNextSnapshot();

  const auto inventory = client.snapshot();
  ASSERT_TRUE(inventory) << inventory.error().message;
  ASSERT_EQ(inventory->devices.size(), 1U);
  service.emitConnected(true);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(
        condition.wait_for(lock, 2s, [&] { return changedEvents >= 1; }));
    EXPECT_EQ(addedEvents, 0);
    EXPECT_EQ(changedEvents, 1);
  }
}

TEST(DmnBluezDiscovery, SnapshotRefreshPreservesSeenDeviceMarkers) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());
  ASSERT_TRUE(client.startDiscovery(kAdapterPath));

  std::mutex mutex;
  std::condition_variable condition;
  bool seenEvent{};

  client.setEventHandler([&](const auto &event) {
    if (event.kind == dmn::Dmn_BluezEventKind::DeviceChanged && event.device &&
        event.device->seen_during_discovery) {
      std::lock_guard lock{mutex};

      seenEvent = true;
      condition.notify_all();
    }
  });

  service.emitDeviceObject(true);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return seenEvent; }));
  }

  const auto inventory = client.snapshot();
  ASSERT_TRUE(inventory);
  ASSERT_EQ(inventory->devices.size(), 1U);
  EXPECT_TRUE(inventory->devices.front().seen_during_discovery);
  ASSERT_TRUE(client.stopDiscovery());
}

TEST(DmnBluezDeviceList, TracksPropertyUpdatesWithSourcePath) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  bool connectedEvent{};

  client.setEventHandler([&](const auto &event) {
    if (event.kind == dmn::Dmn_BluezEventKind::DeviceChanged && event.device &&
        event.device->connected.value_or(false)) {
      std::lock_guard lock{mutex};

      connectedEvent = true;
      condition.notify_all();
    }
  });

  service.emitConnected(true);
  std::unique_lock lock{mutex};
  EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return connectedEvent; }));
  const auto devices = client.devices();
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_TRUE(devices.front().connected.value());
}

TEST(DmnBluezDeviceList, SignalQueueOverflowInvalidatesAndResynchronizes) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto config = makeClientConfig(bus.address());
  config.limits.max_signal_messages = 1;
  config.limits.max_signal_bytes = 1024;
  dmn::Dmn_BluezClient client{config};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  bool overflowReported{};
  bool inventoryRestoredAfterOverflow{};

  client.setEventHandler([&](const auto &event) {
    std::lock_guard lock{mutex};

    overflowReported |=
        event.kind == dmn::Dmn_BluezEventKind::Error &&
        event.message.find("signal queue dropped") != std::string::npos;
    inventoryRestoredAfterOverflow |=
        overflowReported &&
        event.kind == dmn::Dmn_BluezEventKind::ServiceAvailable;
    condition.notify_all();
  });

  service.emitPropertyBurst(128);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 5s, [&] { return overflowReported; }));
    ASSERT_TRUE(condition.wait_for(
        lock, 5s, [&] { return inventoryRestoredAfterOverflow; }));
  }

  const auto inventory = client.snapshot();
  ASSERT_TRUE(inventory) << inventory.error().message;
  const auto &devices = inventory->devices;
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_FALSE(devices.front().connected.value());
}

TEST(DmnBluezDeviceList, StagingOverflowRejectsSnapshotAndAllowsRetry) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto config = makeClientConfig(bus.address());
  config.limits.max_signal_messages = 4096;
  config.limits.max_signal_bytes = 4U * 1024U * 1024U;
  dmn::Dmn_BluezClient client{config};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  bool overflowEvent{};

  client.setEventHandler([&](const auto &event) {
    if (event.kind == dmn::Dmn_BluezEventKind::Error &&
        event.message.find("staging queue overflow") != std::string::npos) {
      std::lock_guard lock{mutex};

      overflowEvent = true;
      condition.notify_all();
    }
  });

  service.holdNextSnapshotReply();
  auto pendingSnapshot =
      std::async(std::launch::async, [&client] { return client.snapshot(); });

  if (!service.waitForHeldSnapshot(5s)) {
    service.releaseHeldSnapshot();
    FAIL() << "fake BlueZ did not hold the ObjectManager snapshot reply";
  }

  // Keep the snapshot open until more signals than the staging limit arrive.
  service.emitPropertyBurst(1100);
  service.releaseHeldSnapshot();

  ASSERT_EQ(pendingSnapshot.wait_for(15s), std::future_status::ready);
  const auto failedSnapshot = pendingSnapshot.get();
  ASSERT_FALSE(failedSnapshot);
  EXPECT_EQ(failedSnapshot.error().kind, dmn::Dmn_DbusErrorKind::ResourceLimit);
  EXPECT_NE(failedSnapshot.error().message.find("staging queue overflow"),
            std::string::npos);
  EXPECT_TRUE(client.devices().empty());

  {
    std::unique_lock lock{mutex};

    EXPECT_TRUE(condition.wait_for(lock, 5s, [&] { return overflowEvent; }));
  }

  const auto retriedSnapshot = client.snapshot();
  ASSERT_TRUE(retriedSnapshot) << retriedSnapshot.error().message;
  ASSERT_EQ(retriedSnapshot->devices.size(), 1U);
  EXPECT_FALSE(retriedSnapshot->devices.front().connected.value());
}

TEST(DmnBluezDeviceList, TracksAdapterPowerChangesAndRemoval) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  bool poweredOffEvent{};
  bool removedEvent{};

  client.setEventHandler([&](const auto &event) {
    std::lock_guard lock{mutex};

    poweredOffEvent |= event.kind == dmn::Dmn_BluezEventKind::AdapterChanged &&
                       event.adapter && !event.adapter->powered.value_or(false);
    removedEvent |= event.kind == dmn::Dmn_BluezEventKind::AdapterRemoved;
    condition.notify_all();
  });

  service.emitAdapterPowered(false);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return poweredOffEvent; }));
  }

  service.emitAdapterObject(false);

  {
    std::unique_lock lock{mutex};

    EXPECT_TRUE(condition.wait_for(lock, 2s, [&] { return removedEvent; }));
  }
}

TEST(DmnBluezDeviceList, InvalidatedConnectionPropertyBecomesUnknown) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  bool invalidatedEvent{};

  client.setEventHandler([&](const auto &event) {
    if (event.kind == dmn::Dmn_BluezEventKind::DeviceChanged && event.device &&
        !event.device->connected) {
      std::lock_guard lock{mutex};

      invalidatedEvent = true;
      condition.notify_all();
    }
  });

  service.emitConnectedInvalidated();

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return invalidatedEvent; }));
  }

  const auto devices = client.devices();
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_FALSE(devices.front().connected.has_value());
}

TEST(DmnBluezDeviceList, TracksObjectManagerRemovalAndAddition) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  std::mutex mutex;
  std::condition_variable condition;
  int added{};
  int removed{};

  client.setEventHandler([&](const auto &event) {
    std::lock_guard lock{mutex};

    added += event.kind == dmn::Dmn_BluezEventKind::DeviceAdded;
    removed += event.kind == dmn::Dmn_BluezEventKind::DeviceRemoved;
    condition.notify_all();
  });

  service.emitDeviceObject(false);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return removed == 1; }));
  }

  EXPECT_TRUE(client.devices().empty());

  service.emitDeviceObject(true);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return added == 1; }));
  }

  EXPECT_EQ(client.devices().size(), 1U);
}

TEST(DmnBluezDeviceList, UpdatesByStableDeviceIdentityAcrossPathChanges) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  constexpr char alternatePath[] =
      "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF_alias";
  std::mutex mutex;
  std::condition_variable condition;
  bool alternatePathEvent{};
  bool connectedEvent{};
  int removedEvents{};

  client.setEventHandler([&](const auto &event) {
    std::lock_guard lock{mutex};

    if (event.kind == dmn::Dmn_BluezEventKind::DeviceChanged && event.device) {
      alternatePathEvent |= event.device->object_path == alternatePath;
      connectedEvent |= event.device->object_path == alternatePath &&
                        event.device->connected.value_or(false);
    }

    removedEvents += event.kind == dmn::Dmn_BluezEventKind::DeviceRemoved;
    condition.notify_all();
  });

  service.emitDeviceObject(true, alternatePath);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(
        condition.wait_for(lock, 2s, [&] { return alternatePathEvent; }));
  }

  auto devices = client.devices();
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices.front().object_path, alternatePath);

  service.emitDeviceObject(false);
  service.emitConnected(true, alternatePath);

  {
    std::unique_lock lock{mutex};

    ASSERT_TRUE(condition.wait_for(lock, 2s, [&] { return connectedEvent; }));
  }

  devices = client.devices();
  ASSERT_EQ(devices.size(), 1U);
  EXPECT_EQ(devices.front().object_path, alternatePath);
  EXPECT_TRUE(devices.front().connected.value());
  EXPECT_EQ(removedEvents, 0);
}

TEST(DmnBluezClient, DeviceActionResolvesCurrentObjectPath) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  constexpr char alternatePath[] =
      "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF_current";
  service.emitDeviceObject(true, alternatePath);
  const auto deadline = std::chrono::steady_clock::now() + 2s;

  while (std::chrono::steady_clock::now() < deadline) {
    const auto devices = client.devices();
    if (devices.size() == 1 && devices.front().object_path == alternatePath) {
      break;
    }

    std::this_thread::sleep_for(10ms);
  }

  const auto devices = client.devices();
  ASSERT_EQ(devices.size(), 1U);
  ASSERT_EQ(devices.front().object_path, alternatePath);
  const auto connected =
      client.connectDevice("aa:bb:cc:dd:ee:ff", "B8:F7:75:BF:F3:C3");
  ASSERT_TRUE(connected) << connected.error().message;
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
  EXPECT_EQ(service.deviceActionPaths(),
            (std::pair{std::string{alternatePath}, std::string{}}));
  EXPECT_EQ(service.forbiddenCount(), 0);
}

TEST(DmnBluezClient, PairUsesCurrentObjectPathAndDefaultAgent) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setInitialPaired(false);
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  const auto paired =
      client.pairDevice("aa:bb:cc:dd:ee:ff", "B8:F7:75:BF:F3:C3");
  ASSERT_TRUE(paired) << paired.error().message;
  EXPECT_EQ(service.pairCount(), 1);
  EXPECT_EQ(service.pairPath(), kDevicePath);

  const auto inventory = client.snapshot();
  ASSERT_TRUE(inventory);
  ASSERT_EQ(inventory->devices.size(), 1);
  EXPECT_TRUE(inventory->devices.front().paired.value());
}

TEST(DmnBluezClient, PairAlreadyPairedIsNoOp) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  const auto paired = client.pairDevice("AA:BB:CC:DD:EE:FF");
  ASSERT_TRUE(paired) << paired.error().message;
  EXPECT_EQ(service.pairCount(), 0);
}

TEST(DmnBluezClient, PairReturnsDefaultAgentFailure) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setInitialPaired(false);
  service.setPairFailure(true);
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  const auto paired = client.pairDevice("AA:BB:CC:DD:EE:FF");
  ASSERT_FALSE(paired);
  EXPECT_EQ(paired.error().kind, dmn::Dmn_DbusErrorKind::Remote);
  EXPECT_EQ(paired.error().message, "injected pairing agent failure");
  EXPECT_EQ(paired.error().remote_name, DBUS_ERROR_ACCESS_DENIED);
  EXPECT_EQ(service.pairCount(), 1);
}

TEST(DmnBluezClient, PairHonorsFiniteMethodTimeout) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setInitialPaired(false);
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  service.holdNextDeviceAction();
  const auto pairResult =
      client.pairDevice("AA:BB:CC:DD:EE:FF", std::nullopt, 50ms);
  const auto pairHeld = service.waitForHeldDeviceAction(1s);
  service.releaseHeldDeviceAction();
  ASSERT_FALSE(pairResult);
  EXPECT_EQ(pairResult.error().kind, dmn::Dmn_DbusErrorKind::Timeout);
  ASSERT_TRUE(pairHeld) << pairResult.error().message;
  EXPECT_EQ(service.pairCount(), 1);
}

TEST(DmnBluezClient, ConnectHonorsFiniteMethodTimeout) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  service.setConnectionTransition(false);
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  service.holdNextDeviceAction();
  const auto connectResult =
      client.connectDevice("AA:BB:CC:DD:EE:FF", std::nullopt, 50ms);
  const auto connectHeld = service.waitForHeldDeviceAction(1s);
  service.releaseHeldDeviceAction();
  ASSERT_FALSE(connectResult);
  EXPECT_EQ(connectResult.error().kind, dmn::Dmn_DbusErrorKind::Timeout);
  ASSERT_TRUE(connectHeld) << connectResult.error().message;
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{1, 0}));
}

TEST(DmnBluezClient, DisconnectHonorsFiniteMethodTimeout) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address(), 0, 0, true};
  service.setConnectionTransition(false);
  dmn::Dmn_BluezClient client{makeClientConfig(bus.address())};
  ASSERT_TRUE(client.snapshot());

  service.holdNextDeviceAction();
  const auto disconnectResult =
      client.disconnectDevice("AA:BB:CC:DD:EE:FF", std::nullopt, 50ms);
  const auto disconnectHeld = service.waitForHeldDeviceAction(1s);
  service.releaseHeldDeviceAction();
  ASSERT_TRUE(disconnectHeld);
  ASSERT_FALSE(disconnectResult);
  EXPECT_EQ(disconnectResult.error().kind, dmn::Dmn_DbusErrorKind::Timeout);
  EXPECT_EQ(service.deviceActionCounts(), (std::pair{0, 1}));
}

} // namespace
