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
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
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
                     bool adapter, int deviceNameMode = 0) {
  DBusMessageIter interfaceEntry;
  DBusMessageIter properties;
  dbus_message_iter_open_container(&interfaces, DBUS_TYPE_DICT_ENTRY, nullptr,
                                   &interfaceEntry);
  dbus_message_iter_append_basic(&interfaceEntry, DBUS_TYPE_STRING, &name);
  dbus_message_iter_open_container(&interfaceEntry, DBUS_TYPE_ARRAY, "{sv}",
                                   &properties);
  if (adapter) {
    appendStringVariant(properties, "Address", "B8:F7:75:BF:F3:C3");
    appendStringVariant(properties, "Alias", "Test adapter");
    appendBoolVariant(properties, "Powered", true);
  } else {
    appendStringVariant(properties, "Address", "AA:BB:CC:DD:EE:FF");
    if (deviceNameMode == 0) {
      appendStringVariant(properties, "Alias", "Known headset");
    } else if (deviceNameMode == 1) {
      appendStringVariant(properties, "Name", "Reported headset");
    } else if (deviceNameMode == 3) {
      appendStringVariant(properties, "Alias", "Headset, \"One\"");
    }

    appendBoolVariant(properties, "Paired", true);
    appendBoolVariant(properties, "Connected", false);
    appendInt16Variant(properties, "RSSI", -47);
  }

  dbus_message_iter_close_container(&interfaceEntry, &properties);
  dbus_message_iter_close_container(&interfaces, &interfaceEntry);
}

auto makeManagedObjectsReply(DBusMessage *request,
                             int deviceNameMode) -> DBusMessage * {
  DBusMessage *reply = dbus_message_new_method_return(request);
  DBusMessageIter out;
  DBusMessageIter objects;
  dbus_message_iter_init_append(reply, &out);
  dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}",
                                   &objects);
  for (const auto &[path, interface, isAdapter] :
       {std::tuple{kAdapterPath, "org.bluez.Adapter1", true},
        std::tuple{kDevicePath, "org.bluez.Device1", false}}) {
    DBusMessageIter object;
    DBusMessageIter interfaces;
    const char *objectPath = path;
    dbus_message_iter_open_container(&objects, DBUS_TYPE_DICT_ENTRY, nullptr,
                                     &object);
    dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH, &objectPath);
    dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sa{sv}}",
                                     &interfaces);
    appendInterface(interfaces, interface, isAdapter, deviceNameMode);
    dbus_message_iter_close_container(&object, &interfaces);
    dbus_message_iter_close_container(&objects, &object);
  }

  dbus_message_iter_close_container(&out, &objects);
  return reply;
}

class FakeBluezService {
public:
  explicit FakeBluezService(const std::string &address, int deviceNameMode = 0)
      : m_deviceNameMode{deviceNameMode} {
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

  auto filterSignature() const -> std::string {
    std::lock_guard lock{m_mutex};
    return m_filterSignature;
  }

  auto filterIsUnrestrictedAuto() const -> bool {
    std::lock_guard lock{m_mutex};
    return m_filterIsUnrestrictedAuto;
  }

  void setManagedObjectsFailure(bool fail) {
    std::lock_guard lock{m_mutex};
    m_failManagedObjects = fail;
  }

  void releaseNameDuringNextSnapshot() {
    std::lock_guard lock{m_mutex};
    m_releaseNameDuringSnapshot = true;
  }

  void emitDeviceDuringNextSnapshot() {
    std::lock_guard lock{m_mutex};
    m_emitDeviceDuringSnapshot = true;
  }

  void emitConnected(bool connected, const std::string &path = kDevicePath) {
    DbusMessagePtr signal{dbus_message_new_signal(
        path.c_str(), "org.freedesktop.DBus.Properties", "PropertiesChanged")};
    DBusMessageIter out;
    DBusMessageIter changed;
    DBusMessageIter invalidated;
    const char *interface = "org.bluez.Device1";
    const char *property = "Connected";
    const dbus_bool_t encoded = connected ? TRUE : FALSE;
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
    dbus_connection_flush(m_connection.get());
  }

  void emitDeviceObject(bool added, const std::string &path = kDevicePath) {
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
      appendInterface(interfaces, "org.bluez.Device1", false);
      dbus_message_iter_close_container(&out, &interfaces);
    } else {
      DBusMessageIter interfaces;
      const char *interface = "org.bluez.Device1";
      dbus_message_iter_open_container(&out, DBUS_TYPE_ARRAY,
                                       DBUS_TYPE_STRING_AS_STRING, &interfaces);
      dbus_message_iter_append_basic(&interfaces, DBUS_TYPE_STRING, &interface);
      dbus_message_iter_close_container(&out, &interfaces);
    }

    dbus_connection_send(m_connection.get(), signal.get(), nullptr);
    dbus_connection_flush(m_connection.get());
  }

private:
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
      bool releaseNameDuringSnapshot{};
      bool emitDeviceDuringSnapshot{};
      {
        std::lock_guard lock{self->m_mutex};
        failManagedObjects = self->m_failManagedObjects;
        releaseNameDuringSnapshot =
            std::exchange(self->m_releaseNameDuringSnapshot, false);
        emitDeviceDuringSnapshot =
            std::exchange(self->m_emitDeviceDuringSnapshot, false);
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

      if (reply == nullptr) {
        reply = failManagedObjects
                    ? dbus_message_new_error(message, DBUS_ERROR_FAILED,
                                             "injected snapshot failure")
                    : makeManagedObjectsReply(message, self->m_deviceNameMode);
      }
    } else if (dbus_message_is_method_call(message, "org.bluez.Adapter1",
                                           "SetDiscoveryFilter")) {
      {
        std::lock_guard lock{self->m_mutex};
        self->m_filterSignature = dbus_message_get_signature(message);
        self->m_filterIsUnrestrictedAuto = filterIsUnrestrictedAuto(message);
      }

      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, "org.bluez.Adapter1",
                                           "StartDiscovery")) {
      {
        std::lock_guard lock{self->m_mutex};
        ++self->m_startCount;
      }

      reply = dbus_message_new_method_return(message);
    } else if (dbus_message_is_method_call(message, "org.bluez.Adapter1",
                                           "StopDiscovery")) {
      {
        std::lock_guard lock{self->m_mutex};
        ++self->m_stopCount;
      }

      reply = dbus_message_new_method_return(message);
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
      dbus_connection_read_write_dispatch(m_connection.get(), 20);
    }
  }

  DbusConnectionPtr m_connection;
  std::atomic_bool m_stopping{};
  std::thread m_worker;
  mutable std::mutex m_mutex;
  int m_startCount{};
  int m_stopCount{};
  int m_forbiddenCount{};
  std::string m_filterSignature;
  bool m_filterIsUnrestrictedAuto{};
  int m_deviceNameMode{};
  bool m_failManagedObjects{};
  bool m_releaseNameDuringSnapshot{};
  bool m_emitDeviceDuringSnapshot{};
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
  ASSERT_TRUE(waitForCliExit(process, 5s));

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
}

TEST(DmnBluezCli, ScanStopsOwnedSessionOnInterrupt) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  FakeBluezService service{bus.address()};
  auto process = startCli(bus.address(),
                          {"--discovery", "--adapter", "B8:F7:75:BF:F3:C3"});
  ASSERT_TRUE(waitForCliText(process, "Discovery active", 5s))
      << process.output;
  service.emitConnected(true);
  ASSERT_TRUE(
      waitForCliText(process,
                     "changed,Test adapter,B8:F7:75:BF:F3:C3,/org/bluez/hci0,"
                     "true,AA:BB:CC:DD:EE:FF,Known headset,",
                     5s))
      << process.output;
  ASSERT_EQ(kill(process.pid, SIGINT), 0);
  ASSERT_TRUE(waitForCliExit(process, 5s));

  ASSERT_TRUE(WIFEXITED(process.status));
  EXPECT_EQ(WEXITSTATUS(process.status), 0);
  EXPECT_NE(process.output.find("action,adapter_name"), std::string::npos);
  EXPECT_EQ(service.discoveryCounts(), (std::pair{1, 1}));
}

TEST(DmnBluezCli, RejectsInvalidArgumentsWithUsage) {
  PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  for (const auto &arguments :
       {std::vector<std::string>{"--unknown"},
        std::vector<std::string>{"--adapter"},
        std::vector<std::string>{"--discovery", "--discovery"}}) {
    auto process = startCli(bus.address(), arguments);
    ASSERT_TRUE(waitForCliExit(process, 5s));

    ASSERT_TRUE(WIFEXITED(process.status));
    EXPECT_EQ(WEXITSTATUS(process.status), 2);
    EXPECT_NE(process.output.find("Usage: dmn-bluez-discovery"),
              std::string::npos);
  }
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
  EXPECT_EQ(service.forbiddenCount(), 0);
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

} // namespace
