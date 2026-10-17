/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-client.cpp
 * @brief BlueZ inventory and discovery implementation over Dmn_DbusClient.
 */

#include "dmn-bluez-client.hpp"
#include "dmn-proc.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace dmn {
namespace {

constexpr std::string_view kBluez{"org.bluez"};
constexpr std::string_view kObjectManager{"org.freedesktop.DBus.ObjectManager"};
constexpr std::string_view kProperties{"org.freedesktop.DBus.Properties"};
constexpr std::string_view kAdapter{"org.bluez.Adapter1"};
constexpr std::string_view kDevice{"org.bluez.Device1"};

auto property(const Dmn_DbusValue &properties,
              std::string_view name) -> const Dmn_DbusValue * {
  for (const auto &[key, value] : properties.asDictionary().entries) {
    if (key.asString() == name) {
      return &value.asVariant();
    }
  }

  return nullptr;
}

auto stringProperty(const Dmn_DbusValue &properties,
                    std::string_view name) -> std::optional<std::string> {
  const auto *value = property(properties, name);
  if (value == nullptr || value->signature() != "s") {
    return std::nullopt;
  }

  return value->asString();
}

auto boolProperty(const Dmn_DbusValue &properties,
                  std::string_view name) -> std::optional<bool> {
  const auto *value = property(properties, name);
  if (value == nullptr || value->signature() != "b") {
    return std::nullopt;
  }

  return value->asBool();
}

auto int16Property(const Dmn_DbusValue &properties,
                   std::string_view name) -> std::optional<std::int16_t> {
  const auto *value = property(properties, name);
  if (value == nullptr || value->signature() != "n") {
    return std::nullopt;
  }

  return value->asInt16();
}

auto findInterface(const Dmn_DbusValue &interfaces,
                   std::string_view name) -> const Dmn_DbusValue * {
  for (const auto &[key, value] : interfaces.asDictionary().entries) {
    if (key.asString() == name) {
      return &value;
    }
  }

  return nullptr;
}

auto adapterForDevicePath(const std::string &path) -> std::string {
  const auto separator = path.rfind('/');

  return separator == std::string::npos ? std::string{}
                                        : path.substr(0, separator);
}

auto makeError(Dmn_DbusErrorKind kind, std::string message) -> Dmn_DbusError {
  return Dmn_DbusError{kind, std::move(message), {}};
}

auto displayName(const std::optional<std::string> &alias,
                 const std::optional<std::string> &name,
                 const std::string &address) -> std::string {
  if (alias && !alias->empty()) {
    return *alias;
  }

  if (name && !name->empty()) {
    return *name;
  }

  return address;
}

auto normalizedAddress(std::string_view address) -> std::string {
  std::string normalized{address};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });

  return normalized;
}

} // namespace

class Dmn_BluezClient::Impl {
public:
  enum class DeviceAction { Connect, Disconnect, Pair };

  explicit Impl(const Dmn_BluezClientConfig &config)
      : m_dbus(makeDbusConfig(config)) {
    m_loading = true;

    subscribe(
        {"org.bluez", std::string{kObjectManager}, "InterfacesAdded", "/", {}});

    subscribe({"org.bluez",
               std::string{kObjectManager},
               "InterfacesRemoved",
               "/",
               {}});

    subscribe({"org.bluez",
               std::string{kProperties},
               "PropertiesChanged",
               {},
               "/org/bluez"});

    subscribe({"org.freedesktop.DBus",
               "org.freedesktop.DBus",
               "NameOwnerChanged",
               "/org/freedesktop/DBus",
               {}});

    if (!m_event_worker.exec([this] { eventLoop(); })) {
      throw std::runtime_error("unable to start BlueZ event worker");
    }

    if (!m_recovery.exec([this] { recoveryLoop(); })) {
      {
        std::lock_guard lock{m_event_mutex};

        m_event_stopping = true;
      }

      m_event_condition.notify_all();
      (void)m_event_worker.wait();

      throw std::runtime_error("unable to start BlueZ recovery worker");
    }
  }

  ~Impl() {
    {
      std::lock_guard lock{m_recovery_mutex};

      m_recovery_stopping = true;
    }

    m_recovery_condition.notify_all();
    (void)m_recovery.wait();

    (void)stopDiscovery();

    for (auto &subscription : m_subscriptions) {
      subscription.cancel();
    }

    m_dbus.shutdown();
    {
      std::lock_guard lock{m_event_mutex};

      m_event_stopping = true;
    }

    m_event_condition.notify_all();
    (void)m_event_worker.wait();
  }

  static auto
  makeDbusConfig(const Dmn_BluezClientConfig &config) -> Dmn_DbusClientConfig {
    Dmn_DbusClientConfig dbusConfig;
    dbusConfig.limits = config.limits;

    if (config.test_bus_address.empty()) {
      dbusConfig.bus_kind = Dmn_DbusBusKind::System;
    } else {
      dbusConfig.bus_kind = Dmn_DbusBusKind::Address;
      dbusConfig.bus_address = config.test_bus_address;
    }

    return dbusConfig;
  }

  void subscribe(const Dmn_DbusSignalMatch &match) {
    auto result = m_dbus.subscribeWithPath(
        match, [this, member = match.member](std::string path,
                                             Dmn_DbusClient::Arguments args) {
          onSignal(member, std::move(path), std::move(args));
        });
    if (!result) {
      throw std::runtime_error("unable to subscribe to BlueZ " + match.member +
                               ": " + result.error().message);
    }

    m_subscriptions.push_back(std::move(*result));
  }

  auto snapshot(
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_BluezValueResult<Dmn_BluezInventory> {
    std::lock_guard snapshotLock{m_snapshot_mutex};
    {
      std::lock_guard lock{m_mutex};

      m_loading = true;
      m_stage_overflow = false;
    }

    const auto droppedBefore = m_dbus.status().dropped_signal_messages;
    auto result =
        m_dbus.call(std::string{kBluez}, "/", std::string{kObjectManager},
                    "GetManagedObjects", {}, timeout);
    auto callbacksDrained = m_dbus.drainSignalCallbacks();
    if (!callbacksDrained) {
      std::lock_guard eventOrderLock{m_event_order_mutex};
      std::lock_guard lock{m_mutex};

      m_loading = false;
      m_stage_overflow = false;
      m_staged_signals.clear();
      m_available = false;
      m_adapters.clear();
      m_devices.clear();
      m_device_paths.clear();

      return std::unexpected(callbacksDrained.error());
    }

    if (!result) {
      std::lock_guard eventOrderLock{m_event_order_mutex};
      {
        std::lock_guard lock{m_mutex};

        m_loading = false;
        m_stage_overflow = false;
        m_staged_signals.clear();
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();
      }

      return std::unexpected(result.error());
    }

    if (result->empty() || result->front().signature() != "a{oa{sa{sv}}}") {
      std::lock_guard eventOrderLock{m_event_order_mutex};
      {
        std::lock_guard lock{m_mutex};

        m_loading = false;
        m_stage_overflow = false;
        m_staged_signals.clear();
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();
      }

      return std::unexpected(
          makeError(Dmn_DbusErrorKind::Codec,
                    "BlueZ returned an invalid object snapshot"));
    }

    std::lock_guard eventOrderLock{m_event_order_mutex};

    const auto droppedAfter = m_dbus.status().dropped_signal_messages;
    std::vector<Dmn_BluezEvent> events;
    std::vector<Dmn_BluezAdapter> adapters;
    std::vector<Dmn_BluezDevice> devices;
    bool ownerLostDuringSnapshot{};

    {
      std::lock_guard lock{m_mutex};

      if (droppedAfter > droppedBefore) {
        m_loading = false;
        m_stage_overflow = false;
        m_staged_signals.clear();
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();

        return std::unexpected(makeError(
            Dmn_DbusErrorKind::ResourceLimit,
            "D-Bus signal queue overflow during BlueZ snapshot; retry"));
      }

      if (m_stage_overflow) {
        m_loading = false;
        m_stage_overflow = false;
        m_staged_signals.clear();
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();

        return std::unexpected(makeError(
            Dmn_DbusErrorKind::ResourceLimit,
            "BlueZ inventory staging queue overflow; retry snapshot"));
      }

      auto previousDevices = std::move(m_devices);
      m_adapters.clear();
      m_devices.clear();
      m_device_paths.clear();

      try {
        const auto &objects = result->front().asDictionary().entries;

        for (const auto &[pathValue, interfaces] : objects) {
          const auto &path = pathValue.asObjectPath();

          if (const auto *adapter = findInterface(interfaces, kAdapter)) {
            m_adapters[path] = parseAdapter(path, *adapter);
          }
        }

        for (const auto &[pathValue, interfaces] : objects) {
          const auto &path = pathValue.asObjectPath();

          if (const auto *device = findInterface(interfaces, kDevice)) {
            auto parsedDevice = parseDevice(path, *device);
            const auto prior =
                previousDevices.find(deviceIdentity(parsedDevice));
            if (prior != previousDevices.end()) {
              parsedDevice.seen_during_discovery =
                  prior->second.seen_during_discovery;
            }

            storeDevice(std::move(parsedDevice));
          }
        }

        for (const auto &signal : m_staged_signals) {
          if (signal.member == "NameOwnerChanged" &&
              signal.arguments.size() == 3 &&
              signal.arguments[0].asString() == kBluez &&
              !signal.arguments[1].asString().empty()) {
            ownerLostDuringSnapshot = true;
          }

          applySignal(signal.member, signal.path, signal.arguments, events);
        }
      } catch (const std::exception &error) {
        m_loading = false;
        m_staged_signals.clear();
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();

        return std::unexpected(
            makeError(Dmn_DbusErrorKind::Codec, error.what()));
      }

      m_staged_signals.clear();
      m_loading = false;

      if (ownerLostDuringSnapshot) {
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();
      } else {
        m_available = true;

        for (const auto &[path, adapter] : m_adapters) {
          (void)path;
          adapters.push_back(adapter);
        }

        for (const auto &[identity, device] : m_devices) {
          (void)identity;
          devices.push_back(device);
        }
      }
    }

    if (ownerLostDuringSnapshot) {
      return std::unexpected(
          makeError(Dmn_DbusErrorKind::NotConnected,
                    "BlueZ service owner changed during inventory snapshot"));
    }

    // Staged changes are reflected in the returned snapshot. Publishing them
    // as live events would make snapshot consumers process them twice.
    std::sort(adapters.begin(), adapters.end(),
              [](const auto &left, const auto &right) {
                return left.address == right.address
                           ? left.object_path < right.object_path
                           : left.address < right.address;
              });

    std::sort(devices.begin(), devices.end(),
              [](const auto &left, const auto &right) {
                return left.adapter_path == right.adapter_path
                           ? left.address < right.address
                           : left.adapter_path < right.adapter_path;
              });

    return Dmn_BluezInventory{std::move(adapters), std::move(devices)};
  }

  auto devices() const -> std::vector<Dmn_BluezDevice> {
    std::lock_guard lock{m_mutex};

    std::vector<Dmn_BluezDevice> result;
    result.reserve(m_devices.size());

    for (const auto &[identity, device] : m_devices) {
      (void)identity;
      result.push_back(device);
    }

    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                return left.adapter_path == right.adapter_path
                           ? left.address < right.address
                           : left.adapter_path < right.adapter_path;
              });

    return result;
  }

  auto deviceAction(const std::string &deviceSelector,
                    const std::optional<std::string> &adapterSelector,
                    std::chrono::milliseconds timeout,
                    DeviceAction action) -> Dmn_BluezResult {
    std::lock_guard actionLock{m_device_action_mutex};
    std::string devicePath;
    bool alreadyInRequestedState{};

    {
      std::lock_guard lock{m_mutex};

      if (!m_available) {
        return std::unexpected(makeError(Dmn_DbusErrorKind::NotConnected,
                                         "BlueZ inventory is not available"));
      }

      const bool isObjectPath = deviceSelector.starts_with('/');
      const auto targetAddress = normalizedAddress(deviceSelector);
      std::vector<const Dmn_BluezDevice *> matches;

      for (const auto &[identity, device] : m_devices) {
        (void)identity;
        const bool matchesSelector =
            isObjectPath ? device.object_path == deviceSelector
                         : normalizedAddress(device.address) == targetAddress;

        if (!matchesSelector) {
          continue;
        }

        const auto adapter = m_adapters.find(device.adapter_path);

        if (adapterSelector && (adapter == m_adapters.end() ||
                                (normalizedAddress(adapter->second.address) !=
                                     normalizedAddress(*adapterSelector) &&
                                 device.adapter_path != *adapterSelector))) {
          continue;
        }

        matches.push_back(&device);
      }

      if (matches.empty()) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::InvalidArgument,
                      "device is not present in the current BlueZ inventory: " +
                          deviceSelector));
      }

      if (matches.size() != 1) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::InvalidArgument,
                      "device address is ambiguous; specify its adapter: " +
                          deviceSelector));
      }

      const auto &device = *matches.front();
      devicePath = device.object_path;

      if (action == DeviceAction::Pair) {
        alreadyInRequestedState = device.paired && *device.paired;
      } else {
        const bool requestedConnected = action == DeviceAction::Connect;
        alreadyInRequestedState =
            device.connected && *device.connected == requestedConnected;
      }
    }

    if (alreadyInRequestedState) {
      return {};
    }

    const auto result =
        m_dbus.call(std::string{kBluez}, devicePath, std::string{kDevice},
                    action == DeviceAction::Connect      ? "Connect"
                    : action == DeviceAction::Disconnect ? "Disconnect"
                                                         : "Pair",
                    {}, timeout);
    if (!result) {
      return std::unexpected(result.error());
    }

    return {};
  }

  auto startDiscovery(const std::string &adapterPath) -> Dmn_BluezResult {
    std::lock_guard discoveryLock{m_discovery_mutex};
    {
      std::lock_guard lock{m_mutex};
      const auto adapter = m_adapters.find(adapterPath);

      if (adapter == m_adapters.end()) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::InvalidArgument,
                      "unknown BlueZ adapter: " + adapterPath));
      }

      if (!m_available) {
        return std::unexpected(makeError(Dmn_DbusErrorKind::NotConnected,
                                         "BlueZ inventory is not available"));
      }

      if (!adapter->second.powered.value_or(false)) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::InvalidArgument,
                      "selected BlueZ adapter is not powered"));
      }

      if (m_discovery_active) {
        return std::unexpected(
            makeError(Dmn_DbusErrorKind::InvalidArgument,
                      "this client already owns a discovery session"));
      }
    }

    std::vector<std::pair<Dmn_DbusValue, Dmn_DbusValue>> entries;
    entries.emplace_back(Dmn_DbusValue{"Transport"},
                         Dmn_DbusValue::variant(Dmn_DbusValue{"auto"}));
    auto filter = Dmn_DbusValue::dictionary("s", "v", std::move(entries));
    auto filterResult =
        m_dbus.call(std::string{kBluez}, adapterPath, std::string{kAdapter},
                    "SetDiscoveryFilter", {std::move(filter)});
    if (!filterResult) {
      return std::unexpected(filterResult.error());
    }

    {
      std::lock_guard lock{m_mutex};

      m_discovery_starting = true;
      m_discovery_adapter = adapterPath;

      for (auto &[identity, device] : m_devices) {
        (void)identity;

        if (device.adapter_path == adapterPath) {
          device.seen_during_discovery = false;
        }
      }
    }

    auto startResult = m_dbus.call(std::string{kBluez}, adapterPath,
                                   std::string{kAdapter}, "StartDiscovery");
    if (!startResult) {
      std::lock_guard lock{m_mutex};

      m_discovery_starting = false;
      m_discovery_adapter.clear();
      for (auto &[identity, device] : m_devices) {
        (void)identity;

        if (device.adapter_path == adapterPath) {
          device.seen_during_discovery = false;
        }
      }

      return std::unexpected(startResult.error());
    }

    std::lock_guard lock{m_mutex};

    m_discovery_active = true;
    m_discovery_starting = false;

    return {};
  }

  auto stopDiscovery() -> Dmn_BluezResult {
    std::lock_guard discoveryLock{m_discovery_mutex};

    std::string adapterPath;
    {
      std::lock_guard lock{m_mutex};

      if (!m_discovery_active) {
        return {};
      }

      adapterPath = m_discovery_adapter;
    }

    auto result = m_dbus.call(std::string{kBluez}, adapterPath,
                              std::string{kAdapter}, "StopDiscovery");
    if (!result) {
      return std::unexpected(result.error());
    }

    std::lock_guard lock{m_mutex};

    m_discovery_active = false;
    m_discovery_starting = false;
    m_discovery_adapter.clear();

    return {};
  }

  void setEventHandler(EventHandler handler) {
    const bool available = static_cast<bool>(handler);
    {
      std::lock_guard lock{m_mutex};

      m_event_handler = std::move(handler);
    }

    {
      std::lock_guard lock{m_event_mutex};

      m_event_handlerAvailable = available;
    }

    m_event_condition.notify_all();
  }

private:
  struct Signal {
    std::string member;
    std::string path;
    Dmn_DbusClient::Arguments arguments;
  };

  static auto
  parseAdapter(const std::string &path,
               const Dmn_DbusValue &properties) -> Dmn_BluezAdapter {
    Dmn_BluezAdapter adapter;
    adapter.object_path = path;
    adapter.address = stringProperty(properties, "Address").value_or("");
    adapter.alias = stringProperty(properties, "Alias");
    adapter.reported_name = stringProperty(properties, "Name");
    adapter.name =
        displayName(adapter.alias, adapter.reported_name, adapter.address);
    adapter.powered = boolProperty(properties, "Powered");

    return adapter;
  }

  static auto parseDevice(const std::string &path,
                          const Dmn_DbusValue &properties) -> Dmn_BluezDevice {
    Dmn_BluezDevice device;
    device.object_path = path;
    device.adapter_path = adapterForDevicePath(path);
    device.address = stringProperty(properties, "Address").value_or("");
    device.alias = stringProperty(properties, "Alias");
    device.reported_name = stringProperty(properties, "Name");
    device.name =
        displayName(device.alias, device.reported_name, device.address);
    device.rssi = int16Property(properties, "RSSI");
    device.paired = boolProperty(properties, "Paired");
    device.connected = boolProperty(properties, "Connected");
    device.services_resolved = boolProperty(properties, "ServicesResolved");

    return device;
  }

  static auto changedProperty(const Dmn_DbusValue &properties,
                              std::string_view name) -> const Dmn_DbusValue * {
    for (const auto &[key, value] : properties.asDictionary().entries) {
      if (key.asString() == name) {
        return &value.asVariant();
      }
    }

    return nullptr;
  }

  static auto invalidatedProperty(const std::vector<Dmn_DbusValue> &invalidated,
                                  std::string_view name) -> bool {
    return std::any_of(
        invalidated.begin(), invalidated.end(),
        [name](const auto &value) { return value.asString() == name; });
  }

  static auto stringChange(
      const Dmn_DbusValue &changed,
      const std::vector<Dmn_DbusValue> &invalidated, std::string_view name,
      std::optional<std::string> current) -> std::optional<std::string> {
    if (const auto *value = changedProperty(changed, name);
        value != nullptr && value->signature() == "s") {
      return value->asString();
    }

    if (invalidatedProperty(invalidated, name)) {
      return std::nullopt;
    }

    return current;
  }

  static auto boolChange(const Dmn_DbusValue &changed,
                         const std::vector<Dmn_DbusValue> &invalidated,
                         std::string_view name,
                         std::optional<bool> current) -> std::optional<bool> {
    if (const auto *value = changedProperty(changed, name);
        value != nullptr && value->signature() == "b") {
      return value->asBool();
    }

    if (invalidatedProperty(invalidated, name)) {
      return std::nullopt;
    }

    return current;
  }

  static auto rssiChange(const Dmn_DbusValue &changed,
                         const std::vector<Dmn_DbusValue> &invalidated,
                         std::optional<std::int16_t> current)
      -> std::optional<std::int16_t> {
    if (const auto *value = changedProperty(changed, "RSSI");
        value != nullptr && value->signature() == "n") {
      return value->asInt16();
    }

    if (invalidatedProperty(invalidated, "RSSI")) {
      return std::nullopt;
    }

    return current;
  }

  static void updateAdapter(Dmn_BluezAdapter &adapter,
                            const Dmn_DbusValue &changed,
                            const std::vector<Dmn_DbusValue> &invalidated) {
    adapter.address =
        stringChange(changed, invalidated, "Address", adapter.address)
            .value_or("");
    adapter.alias = stringChange(changed, invalidated, "Alias", adapter.alias);
    adapter.reported_name =
        stringChange(changed, invalidated, "Name", adapter.reported_name);
    adapter.powered =
        boolChange(changed, invalidated, "Powered", adapter.powered);
    adapter.name =
        displayName(adapter.alias, adapter.reported_name, adapter.address);
  }

  static void updateDevice(Dmn_BluezDevice &device,
                           const Dmn_DbusValue &changed,
                           const std::vector<Dmn_DbusValue> &invalidated) {
    device.address =
        stringChange(changed, invalidated, "Address", device.address)
            .value_or("");
    device.alias = stringChange(changed, invalidated, "Alias", device.alias);
    device.reported_name =
        stringChange(changed, invalidated, "Name", device.reported_name);
    device.rssi = rssiChange(changed, invalidated, device.rssi);
    device.paired = boolChange(changed, invalidated, "Paired", device.paired);
    device.connected =
        boolChange(changed, invalidated, "Connected", device.connected);
    device.services_resolved = boolChange(
        changed, invalidated, "ServicesResolved", device.services_resolved);
    device.name =
        displayName(device.alias, device.reported_name, device.address);
  }

  void onSignal(const std::string &member, std::string path,
                Dmn_DbusClient::Arguments arguments) {
    const auto droppedSignals = m_dbus.status().dropped_signal_messages;
    std::lock_guard eventOrderLock{m_event_order_mutex};
    std::vector<Dmn_BluezEvent> events;
    bool requestRecovery{};

    {
      std::lock_guard lock{m_mutex};

      if (droppedSignals != m_last_dropped_signal_messages) {
        m_last_dropped_signal_messages = droppedSignals;
        m_available = false;
        m_adapters.clear();
        m_devices.clear();
        m_device_paths.clear();
        if (m_loading) {
          m_staged_signals.clear();
          m_stage_overflow = true;
        }

        requestRecovery = true;
        events.push_back({Dmn_BluezEventKind::Error,
                          {},
                          {},
                          "D-Bus signal queue dropped BlueZ events; "
                          "resynchronizing inventory"});
      } else if (m_loading) {
        if (m_staged_signals.size() >= kMaxStagedSignals) {
          m_staged_signals.clear();
          m_stage_overflow = true;
          m_available = false;
          m_adapters.clear();
          m_devices.clear();
          m_device_paths.clear();
          events.push_back(
              {Dmn_BluezEventKind::Error,
               {},
               {},
               "BlueZ inventory staging queue overflow; resnapshot required"});
        } else {
          m_staged_signals.push_back(
              {member, std::move(path), std::move(arguments)});
        }
      } else {
        applySignal(member, path, arguments, events);
      }
    }

    if (requestRecovery) {
      {
        std::lock_guard recoveryLock{m_recovery_mutex};

        m_recovery_requested = true;
      }

      m_recovery_condition.notify_one();
    }

    for (const auto &event : events) {
      emit(event);
    }
  }

  void applySignal(const std::string &member, const std::string &path,
                   const Dmn_DbusClient::Arguments &arguments,
                   std::vector<Dmn_BluezEvent> &events) {
    try {
      if (member == "NameOwnerChanged") {
        if (arguments.size() != 3 || arguments[0].asString() != kBluez) {
          return;
        }

        const auto oldOwner = arguments[1].asString();
        const auto newOwner = arguments[2].asString();
        if (!oldOwner.empty()) {
          m_available = false;
          m_adapters.clear();
          m_devices.clear();
          m_device_paths.clear();
          m_discovery_active = false;
          m_discovery_starting = false;
          m_discovery_adapter.clear();
          events.push_back({Dmn_BluezEventKind::ServiceUnavailable,
                            {},
                            {},
                            "BlueZ service owner disappeared"});
        }

        if (!newOwner.empty()) {
          {
            std::lock_guard recoveryLock{m_recovery_mutex};

            m_recovery_requested = true;
          }

          m_recovery_condition.notify_one();
        }

        return;
      }

      if (member == "InterfacesAdded" && arguments.size() == 2) {
        const auto path = arguments[0].asObjectPath();
        const auto &interfaces = arguments[1];

        if (const auto *props = findInterface(interfaces, kAdapter)) {
          auto adapter = parseAdapter(path, *props);
          m_adapters[path] = adapter;
          events.push_back({Dmn_BluezEventKind::AdapterAdded, adapter, {}, {}});
        }

        if (const auto *props = findInterface(interfaces, kDevice)) {
          auto device = parseDevice(path, *props);
          device.seen_during_discovery =
              (m_discovery_active || m_discovery_starting) &&
              device.adapter_path == m_discovery_adapter;
          const auto inserted = storeDevice(std::move(device));
          const auto stored = findDeviceByPath(path);

          if (stored != m_devices.end()) {
            events.push_back({inserted ? Dmn_BluezEventKind::DeviceAdded
                                       : Dmn_BluezEventKind::DeviceChanged,
                              {},
                              stored->second,
                              {}});
          }
        }

        return;
      }

      if (member == "InterfacesRemoved" && arguments.size() == 2) {
        const auto path = arguments[0].asObjectPath();
        const auto &interfaces = arguments[1].asArray().values;

        for (const auto &interface : interfaces) {
          if (interface.asString() == kAdapter) {
            const auto found = m_adapters.find(path);
            if (found != m_adapters.end()) {
              events.push_back(
                  {Dmn_BluezEventKind::AdapterRemoved, found->second, {}, {}});
              m_adapters.erase(found);
            }
          } else if (interface.asString() == kDevice) {
            const auto pathEntry = m_device_paths.find(path);
            if (pathEntry != m_device_paths.end()) {
              const auto found = m_devices.find(pathEntry->second);
              if (found == m_devices.end()) {
                m_device_paths.erase(pathEntry);

                continue;
              }

              events.push_back(
                  {Dmn_BluezEventKind::DeviceRemoved, {}, found->second, {}});
              m_devices.erase(found);
              m_device_paths.erase(pathEntry);
            }
          }
        }

        return;
      }

      if (member == "PropertiesChanged" && arguments.size() == 3) {
        const auto interface = arguments[0].asString();
        const auto &changed = arguments[1];
        const auto &invalidated = arguments[2].asArray().values;

        if (interface == kAdapter) {
          const auto found = m_adapters.find(path);
          if (found == m_adapters.end()) {
            return;
          }

          updateAdapter(found->second, changed, invalidated);
          events.push_back(
              {Dmn_BluezEventKind::AdapterChanged, found->second, {}, {}});
        } else if (interface == kDevice) {
          const auto found = findDeviceByPath(path);
          if (found == m_devices.end()) {
            return;
          }

          const auto previousIdentity = found->first;
          updateDevice(found->second, changed, invalidated);

          if (deviceIdentity(found->second) != previousIdentity) {
            auto updatedDevice = std::move(found->second);
            m_devices.erase(found);
            storeDevice(std::move(updatedDevice));
          }

          const auto updated = findDeviceByPath(path);
          if (updated == m_devices.end()) {
            return;
          }

          if ((m_discovery_active || m_discovery_starting) &&
              updated->second.adapter_path == m_discovery_adapter &&
              changedProperty(changed, "RSSI") != nullptr) {
            updated->second.seen_during_discovery = true;
          }

          events.push_back(
              {Dmn_BluezEventKind::DeviceChanged, {}, updated->second, {}});
        }
      }
    } catch (const std::exception &error) {
      events.push_back({Dmn_BluezEventKind::Error,
                        {},
                        {},
                        std::string{"invalid BlueZ signal: "} + error.what()});
    }
  }

  void emit(const Dmn_BluezEvent &event) {
    bool overflow{};

    {
      std::lock_guard lock{m_event_mutex};

      if (m_event_stopping) {
        return;
      }

      if (m_event_queue.size() >= kMaxQueuedEvents) {
        overflow = true;
        m_event_queue.clear();
        m_event_queue.push_back({Dmn_BluezEventKind::Error,
                                 {},
                                 {},
                                 "BlueZ event queue overflow; inventory "
                                 "resynchronization required"});
      } else {
        m_event_queue.push_back(event);
      }
    }

    if (overflow) {
      std::lock_guard lock{m_mutex};

      m_available = false;
      m_adapters.clear();
      m_devices.clear();
      m_device_paths.clear();
      m_loading = false;
      m_staged_signals.clear();
      m_stage_overflow = false;
    }

    if (overflow) {
      {
        std::lock_guard lock{m_recovery_mutex};

        m_recovery_requested = true;
      }

      m_recovery_condition.notify_one();
    }

    m_event_condition.notify_one();
  }

  void eventLoop() {
    while (true) {
      Dmn_BluezEvent event;
      EventHandler handler;

      {
        std::unique_lock lock{m_event_mutex};

        m_event_condition.wait(lock, [this] {
          return m_event_stopping ||
                 (m_event_handlerAvailable && !m_event_queue.empty());
        });

        if (m_event_stopping) {
          return;
        }

        event = std::move(m_event_queue.front());
        m_event_queue.pop_front();
      }

      {
        std::lock_guard lock{m_mutex};

        handler = m_event_handler;
      }

      if (handler) {
        handler(event);
      }
    }
  }

  void recoveryLoop() {
    while (true) {
      {
        std::unique_lock lock{m_recovery_mutex};

        m_recovery_condition.wait(lock, [this] {
          return m_recovery_stopping || m_recovery_requested;
        });

        if (m_recovery_stopping) {
          return;
        }

        m_recovery_requested = false;
      }

      const auto result = snapshot();
      if (result) {
        emit({Dmn_BluezEventKind::ServiceAvailable,
              {},
              {},
              "BlueZ inventory is ready"});
      } else {
        emit({Dmn_BluezEventKind::Error,
              {},
              {},
              "unable to reload BlueZ inventory after service restart: " +
                  result.error().message});
      }
    }
  }

  using DeviceIdentity = std::pair<std::string, std::string>;

  auto deviceIdentity(const Dmn_BluezDevice &device) const -> DeviceIdentity {
    const auto adapter = m_adapters.find(device.adapter_path);
    const auto adapterIdentity =
        adapter == m_adapters.end() || adapter->second.address.empty()
            ? device.adapter_path
            : adapter->second.address;
    const auto deviceAddress =
        device.address.empty() ? device.object_path : device.address;

    return {adapterIdentity, deviceAddress};
  }

  auto findDeviceByPath(const std::string &path)
      -> std::map<DeviceIdentity, Dmn_BluezDevice>::iterator {
    const auto pathEntry = m_device_paths.find(path);

    return pathEntry == m_device_paths.end()
               ? m_devices.end()
               : m_devices.find(pathEntry->second);
  }

  auto storeDevice(Dmn_BluezDevice device) -> bool {
    const auto identity = deviceIdentity(device);
    const auto existing = m_devices.find(identity);
    const bool inserted = existing == m_devices.end();

    if (existing != m_devices.end()) {
      if (existing->second.object_path != device.object_path) {
        m_device_paths.erase(existing->second.object_path);
      }

      device.seen_during_discovery = device.seen_during_discovery ||
                                     existing->second.seen_during_discovery;
    }

    if (const auto priorPath = m_device_paths.find(device.object_path);
        priorPath != m_device_paths.end() && priorPath->second != identity) {
      m_devices.erase(priorPath->second);
    }

    m_device_paths[device.object_path] = identity;
    m_devices[identity] = std::move(device);

    return inserted;
  }

  static constexpr std::size_t kMaxStagedSignals{1024};
  static constexpr std::size_t kMaxQueuedEvents{4096};
  Dmn_DbusClient m_dbus;
  std::vector<Dmn_DbusSubscription> m_subscriptions;
  std::mutex m_event_order_mutex;
  std::mutex m_snapshot_mutex;
  std::mutex m_discovery_mutex;
  std::mutex m_device_action_mutex;
  std::mutex m_recovery_mutex;
  std::condition_variable m_recovery_condition;
  Dmn_Proc m_recovery{"dmn-bluez-recovery"};
  std::mutex m_event_mutex;
  std::condition_variable m_event_condition;
  std::deque<Dmn_BluezEvent> m_event_queue;
  Dmn_Proc m_event_worker{"dmn-bluez-event"};
  bool m_recovery_requested{};
  bool m_recovery_stopping{};
  bool m_event_handlerAvailable{};
  bool m_event_stopping{};
  mutable std::mutex m_mutex;
  std::map<std::string, Dmn_BluezAdapter> m_adapters;
  std::map<DeviceIdentity, Dmn_BluezDevice> m_devices;
  std::map<std::string, DeviceIdentity> m_device_paths;
  std::vector<Signal> m_staged_signals;
  EventHandler m_event_handler;
  bool m_loading{};
  bool m_stage_overflow{};
  bool m_available{};
  std::size_t m_last_dropped_signal_messages{};
  bool m_discovery_active{};
  bool m_discovery_starting{};
  std::string m_discovery_adapter;
};

Dmn_BluezClient::Dmn_BluezClient(const Dmn_BluezClientConfig &config)
    : m_impl{std::make_unique<Impl>(config)} {}

Dmn_BluezClient::~Dmn_BluezClient() = default;

void Dmn_BluezClient::setEventHandler(EventHandler handler) {
  m_impl->setEventHandler(std::move(handler));
}

auto Dmn_BluezClient::snapshot(std::chrono::milliseconds timeout)
    -> Dmn_BluezValueResult<Dmn_BluezInventory> {
  return m_impl->snapshot(timeout);
}

auto Dmn_BluezClient::devices() const -> std::vector<Dmn_BluezDevice> {
  return m_impl->devices();
}

auto Dmn_BluezClient::connectDevice(
    const std::string &deviceSelector,
    const std::optional<std::string> &adapterSelector,
    std::chrono::milliseconds timeout) -> Dmn_BluezResult {
  return m_impl->deviceAction(deviceSelector, adapterSelector, timeout,
                              Impl::DeviceAction::Connect);
}

auto Dmn_BluezClient::pairDevice(
    const std::string &deviceSelector,
    const std::optional<std::string> &adapterSelector,
    std::chrono::milliseconds timeout) -> Dmn_BluezResult {
  return m_impl->deviceAction(deviceSelector, adapterSelector, timeout,
                              Impl::DeviceAction::Pair);
}

auto Dmn_BluezClient::disconnectDevice(
    const std::string &deviceSelector,
    const std::optional<std::string> &adapterSelector,
    std::chrono::milliseconds timeout) -> Dmn_BluezResult {
  return m_impl->deviceAction(deviceSelector, adapterSelector, timeout,
                              Impl::DeviceAction::Disconnect);
}

auto Dmn_BluezClient::startDiscovery(const std::string &adapterPath)
    -> Dmn_BluezResult {
  return m_impl->startDiscovery(adapterPath);
}

auto Dmn_BluezClient::stopDiscovery() -> Dmn_BluezResult {
  return m_impl->stopDiscovery();
}

} // namespace dmn
