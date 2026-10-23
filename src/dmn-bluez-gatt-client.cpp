/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-gatt-client.cpp
 * @brief Remote GATT operations over BlueZ's D-Bus API.
 *
 * The implementation translates the backend-neutral GATT contract into
 * bounded BlueZ property/method calls. Discovery returns owned snapshots;
 * notification subscriptions retain their D-Bus client and invalidate on
 * device, characteristic, or BlueZ-owner changes.
 */

#include "dmn-bluez-gatt-client.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace dmn {
namespace {

constexpr std::string_view kBluez{"org.bluez"};
constexpr std::string_view kObjectManager{"org.freedesktop.DBus.ObjectManager"};
constexpr std::string_view kProperties{"org.freedesktop.DBus.Properties"};
constexpr std::string_view kDevice{"org.bluez.Device1"};
constexpr std::string_view kGattService{"org.bluez.GattService1"};
constexpr std::string_view kGattCharacteristic{"org.bluez.GattCharacteristic1"};

auto error(Dmn_GattErrorKind kind, std::string message,
           std::string remoteName = {}) -> Dmn_GattError {
  return {kind, std::move(message), std::move(remoteName)};
}

auto mapError(const Dmn_DbusError &source) -> Dmn_GattError {
  const auto kind =
      source.kind == Dmn_DbusErrorKind::InvalidArgument
          ? Dmn_GattErrorKind::InvalidArgument
      : source.kind == Dmn_DbusErrorKind::Timeout ? Dmn_GattErrorKind::Timeout
      : source.kind == Dmn_DbusErrorKind::Remote  ? Dmn_GattErrorKind::Remote
      : source.kind == Dmn_DbusErrorKind::Codec   ? Dmn_GattErrorKind::Decode
                                                  : Dmn_GattErrorKind::Backend;

  return error(kind, source.message, source.remote_name);
}

auto timeoutError(std::string message) -> Dmn_GattError {
  return error(Dmn_GattErrorKind::Timeout, std::move(message));
}

auto remainingTimeout(std::chrono::steady_clock::time_point deadline)
    -> std::chrono::milliseconds {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds::zero();
  }

  return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

auto makeDbusConfig(const Dmn_BluezClientConfig &config)
    -> Dmn_DbusClientConfig {
  Dmn_DbusClientConfig result;
  result.limits = config.limits;
  result.bus_kind = config.test_bus_address.empty() ? Dmn_DbusBusKind::System
                                                    : Dmn_DbusBusKind::Address;
  result.bus_address = config.test_bus_address;

  return result;
}

auto property(const Dmn_DbusValue &properties,
              std::string_view name) -> const Dmn_DbusValue * {
  if (properties.signature() != "a{sv}") {
    return nullptr;
  }

  for (const auto &[key, value] : properties.asDictionary().entries) {
    if (key.signature() == "s" && key.asString() == name) {
      return &value.asVariant();
    }
  }

  return nullptr;
}

auto interface(const Dmn_DbusValue &interfaces,
               std::string_view name) -> const Dmn_DbusValue * {
  if (interfaces.signature() != "a{sa{sv}}") {
    return nullptr;
  }

  for (const auto &[key, value] : interfaces.asDictionary().entries) {
    if (key.signature() == "s" && key.asString() == name) {
      return &value;
    }
  }

  return nullptr;
}

auto stringProperty(const Dmn_DbusValue &props,
                    std::string_view name) -> std::optional<std::string> {
  const auto *value = property(props, name);
  if (value == nullptr || value->signature() != "s") {
    return std::nullopt;
  }

  return value->asString();
}

auto boolProperty(const Dmn_DbusValue &props,
                  std::string_view name) -> std::optional<bool> {
  const auto *value = property(props, name);
  if (value == nullptr || value->signature() != "b") {
    return std::nullopt;
  }

  return value->asBool();
}

auto pathWithinDevice(std::string_view device,
                      std::string_view object) -> bool {
  return !device.empty() && object.size() > device.size() &&
         object.starts_with(device) && object[device.size()] == '/';
}

auto hasFlag(const Dmn_GattCharacteristic &characteristic,
             std::string_view flag) -> bool {
  return std::find(characteristic.flags.begin(), characteristic.flags.end(),
                   flag) != characteristic.flags.end();
}

auto canonicalUuid(std::string uuid) -> std::string {
  std::transform(uuid.begin(), uuid.end(), uuid.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });

  return uuid;
}

} // namespace

class Dmn_BluezGattClient::Impl
    : public std::enable_shared_from_this<Dmn_BluezGattClient::Impl> {
public:
  struct NotificationState {
    std::mutex mutex;
    NotificationHandler handler;
    Dmn_DbusSubscription property_subscription;
    Dmn_DbusSubscription owner_subscription;
    Dmn_DbusSubscription object_subscription;
    bool invalidated{};
    bool started{};
    std::string path;
  };

  explicit Impl(const Dmn_BluezClientConfig &config)
      : m_dbus{makeDbusConfig(config)} {}

  auto validateDeviceId(std::string_view deviceId) const -> Dmn_GattResult {
    if (deviceId.empty() || deviceId.front() != '/') {
      return std::unexpected(
          error(Dmn_GattErrorKind::InvalidArgument,
                "device identifier must be a BlueZ object path"));
    }

    return {};
  }

  auto getDeviceState(const std::string &deviceId,
                      std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<std::pair<bool, bool>> {
    if (timeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "GATT operation deadline expired while reading device state"));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] { return remainingTimeout(deadline); };
    const auto servicesTimeout = remaining();
    if (servicesTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "GATT operation deadline expired while reading device state"));
    }

    auto result = m_dbus.call(std::string{kBluez}, deviceId,
                              std::string{kProperties}, "Get",
                              {Dmn_DbusValue{std::string{kDevice}},
                               Dmn_DbusValue{"ServicesResolved"}},
                              servicesTimeout);
    if (!result) {
      return std::unexpected(mapError(result.error()));
    }

    if (result->size() != 1 || result->front().signature() != "v" ||
        result->front().asVariant().signature() != "b") {
      return std::unexpected(
          error(Dmn_GattErrorKind::Decode,
                "BlueZ returned invalid ServicesResolved property"));
    }

    const auto resolved = result->front().asVariant().asBool();
    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "GATT operation deadline expired while reading device state"));
    }

    const auto connectedTimeout = remaining();
    if (connectedTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "GATT operation deadline expired while reading device state"));
    }

    auto connectedResult = m_dbus.call(
        std::string{kBluez}, deviceId, std::string{kProperties}, "Get",
        {Dmn_DbusValue{std::string{kDevice}}, Dmn_DbusValue{"Connected"}},
        connectedTimeout);
    if (!connectedResult) {
      return std::unexpected(mapError(connectedResult.error()));
    }

    if (connectedResult->size() != 1 ||
        connectedResult->front().signature() != "v" ||
        connectedResult->front().asVariant().signature() != "b") {
      return std::unexpected(
          error(Dmn_GattErrorKind::Decode,
                "BlueZ returned invalid Connected property"));
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "GATT operation deadline expired while reading device state"));
    }

    return std::pair{connectedResult->front().asVariant().asBool(), resolved};
  }

  auto
  waitForServicesResolved(const std::string &deviceId,
                          std::chrono::milliseconds timeout) -> Dmn_GattResult {
    auto valid = validateDeviceId(deviceId);
    if (!valid) {
      return valid;
    }

    if (timeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(error(Dmn_GattErrorKind::InvalidArgument,
                                   "GATT wait timeout must be positive"));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] { return remainingTimeout(deadline); };
    struct WaitState {
      std::mutex mutex;
      std::condition_variable condition;
      std::optional<bool> connected;
      std::optional<bool> resolved;
      std::optional<Dmn_GattError> failure;
      bool observedConnectionChange{};
    };
    auto state = std::make_shared<WaitState>();
    const auto propertiesTimeout = remaining();
    if (propertiesTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "service-resolution wait deadline expired before subscribing"));
    }

    auto properties = m_dbus.subscribeWithPath(
        {"org.bluez", std::string{kProperties}, "PropertiesChanged", {}, {}},
        [state, deviceId](std::string path,
                          Dmn_DbusClient::Arguments arguments) {
          if (path != deviceId || arguments.size() != 3 ||
              arguments[0].signature() != "s" ||
              arguments[0].asString() != kDevice ||
              arguments[1].signature() != "a{sv}") {
            return;
          }

          std::lock_guard lock{state->mutex};

          if (const auto value = boolProperty(arguments[1], "Connected")) {
            state->connected = value;
            state->observedConnectionChange = true;
          }

          if (const auto value =
                  boolProperty(arguments[1], "ServicesResolved")) {
            state->resolved = value;
          }

          state->condition.notify_all();
        },
        propertiesTimeout);
    if (!properties) {
      return std::unexpected(mapError(properties.error()));
    }

    const auto ownerTimeout = remaining();
    if (ownerTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "service-resolution wait deadline expired while subscribing"));
    }

    auto owner = m_dbus.subscribe(
        {"org.freedesktop.DBus",
         "org.freedesktop.DBus",
         "NameOwnerChanged",
         "/org/freedesktop/DBus",
         {}},
        [state](Dmn_DbusClient::Arguments arguments) {
          if (arguments.size() != 3 || arguments[0].signature() != "s" ||
              arguments[0].asString() != kBluez ||
              arguments[1].signature() != "s" ||
              arguments[2].signature() != "s" ||
              arguments[1].asString() == arguments[2].asString() ||
              (arguments[1].asString().empty() &&
               arguments[2].asString().empty())) {
            return;
          }

          std::lock_guard lock{state->mutex};

          state->failure =
              error(Dmn_GattErrorKind::Backend,
                    "BlueZ service owner changed during service wait");
          state->condition.notify_all();
        },
        ownerTimeout);
    if (!owner) {
      return std::unexpected(mapError(owner.error()));
    }

    const auto objectsTimeout = remaining();
    if (objectsTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "service-resolution wait deadline expired while subscribing"));
    }

    auto objects = m_dbus.subscribe(
        {"org.bluez",
         std::string{kObjectManager},
         "InterfacesRemoved",
         "/",
         {}},
        [state, deviceId](Dmn_DbusClient::Arguments arguments) {
          if (arguments.size() != 2 || arguments[0].signature() != "o" ||
              arguments[0].asObjectPath() != deviceId ||
              arguments[1].signature() != "as") {
            return;
          }

          const auto removedDevice = std::any_of(
              arguments[1].asArray().values.begin(),
              arguments[1].asArray().values.end(), [](const auto &value) {
                return value.signature() == "s" && value.asString() == kDevice;
              });
          if (!removedDevice) {
            return;
          }

          std::lock_guard lock{state->mutex};

          state->failure =
              error(Dmn_GattErrorKind::NotReady,
                    "selected BlueZ device was removed during service wait");
          state->condition.notify_all();
        },
        objectsTimeout);
    if (!objects) {
      return std::unexpected(mapError(objects.error()));
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "service-resolution wait deadline expired while subscribing"));
    }

    auto initial = getDeviceState(deviceId, remaining());
    if (!initial) {
      return std::unexpected(initial.error());
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(
          timeoutError("GATT service-resolution wait deadline expired"));
    }

    std::unique_lock lock{state->mutex};
    if (!state->connected) {
      state->connected = initial->first;
    }

    if (!state->resolved) {
      state->resolved = initial->second;
    }

    while (true) {
      if (state->failure) {
        return std::unexpected(*state->failure);
      }

      if (state->connected == true && state->resolved == true) {
        return {};
      }

      if (state->connected == false && state->observedConnectionChange) {
        return std::unexpected(error(
            Dmn_GattErrorKind::NotReady,
            "selected BlueZ device disconnected before service resolution"));
      }

      if (state->condition.wait_until(lock, deadline) ==
          std::cv_status::timeout) {
        return std::unexpected(
            error(Dmn_GattErrorKind::Timeout,
                  "BlueZ services were not resolved before the deadline"));
      }
    }
  }

  auto discover(const std::string &deviceId, std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<std::vector<Dmn_GattService>> {
    auto valid = validateDeviceId(deviceId);
    if (!valid) {
      return std::unexpected(valid.error());
    }

    if (timeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(error(Dmn_GattErrorKind::InvalidArgument,
                                   "GATT discovery timeout must be positive"));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] { return remainingTimeout(deadline); };
    auto state = getDeviceState(deviceId, remaining());
    if (!state) {
      return std::unexpected(state.error());
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "GATT discovery deadline expired before reading the object tree"));
    }

    if (!state->first || !state->second) {
      return std::unexpected(error(Dmn_GattErrorKind::NotReady,
                                   "device must be connected with services "
                                   "resolved before GATT discovery"));
    }

    const auto objectTreeTimeout = remaining();
    if (objectTreeTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(
          timeoutError("GATT discovery deadline expired before reading the "
                       "object tree"));
    }

    auto objects =
        m_dbus.call(std::string{kBluez}, "/", std::string{kObjectManager},
                    "GetManagedObjects", {}, objectTreeTimeout);
    if (!objects) {
      return std::unexpected(mapError(objects.error()));
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(
          timeoutError("GATT discovery deadline expired while reading the "
                       "object tree"));
    }

    if (objects->size() != 1 ||
        objects->front().signature() != "a{oa{sa{sv}}}") {
      return std::unexpected(
          error(Dmn_GattErrorKind::Decode,
                "BlueZ returned an invalid GATT object tree"));
    }

    try {
      std::map<std::string, Dmn_GattService> services;
      std::vector<std::pair<std::string, Dmn_GattCharacteristic>> chars;
      for (const auto &[pathValue, interfaces] :
           objects->front().asDictionary().entries) {
        if (pathValue.signature() != "o") {
          return std::unexpected(
              error(Dmn_GattErrorKind::Decode,
                    "BlueZ object tree contains a non-path key"));
        }

        const auto &path = pathValue.asObjectPath();
        if (const auto *props = interface(interfaces, kGattService)) {
          const auto parent = property(*props, "Device");
          if (parent == nullptr || parent->signature() != "o") {
            return std::unexpected(
                error(Dmn_GattErrorKind::Decode,
                      "BlueZ returned an invalid GattService1 Device"));
          }

          if (parent->asObjectPath() != deviceId) {
            continue;
          }

          const auto uuid = stringProperty(*props, "UUID");
          const auto primary = boolProperty(*props, "Primary");
          if (!uuid || !primary) {
            return std::unexpected(
                error(Dmn_GattErrorKind::Decode,
                      "BlueZ returned incomplete GattService1 properties"));
          }

          services.emplace(
              path, Dmn_GattService{path, canonicalUuid(*uuid), *primary, {}});
        }

        if (const auto *props = interface(interfaces, kGattCharacteristic)) {
          const auto parent = property(*props, "Service");
          if (parent == nullptr || parent->signature() != "o") {
            return std::unexpected(
                error(Dmn_GattErrorKind::Decode,
                      "BlueZ returned an invalid GattCharacteristic1 Service"));
          }

          if (!pathWithinDevice(deviceId, parent->asObjectPath())) {
            continue;
          }

          const auto uuid = stringProperty(*props, "UUID");
          const auto *flagsValue = property(*props, "Flags");
          if (!uuid || flagsValue == nullptr ||
              flagsValue->signature() != "as") {
            return std::unexpected(error(
                Dmn_GattErrorKind::Decode,
                "BlueZ returned incomplete GattCharacteristic1 properties"));
          }

          Dmn_GattCharacteristic characteristic{path, canonicalUuid(*uuid), {}};
          for (const auto &flag : flagsValue->asArray().values) {
            if (flag.signature() != "s") {
              return std::unexpected(error(
                  Dmn_GattErrorKind::Decode,
                  "BlueZ characteristic Flags contains a non-string value"));
            }

            characteristic.flags.push_back(flag.asString());
          }

          chars.emplace_back(parent->asObjectPath(), std::move(characteristic));
        }
      }

      for (auto &[servicePath, characteristic] : chars) {
        const auto parent = services.find(servicePath);
        if (parent != services.end()) {
          parent->second.characteristics.push_back(std::move(characteristic));
        }
      }

      std::vector<Dmn_GattService> result;
      result.reserve(services.size());
      for (auto &entry : services) {
        auto &service = entry.second;
        std::sort(service.characteristics.begin(),
                  service.characteristics.end(),
                  [](const auto &left, const auto &right) {
                    return left.id < right.id;
                  });
        result.push_back(std::move(service));
      }

      if (remaining() <= std::chrono::milliseconds::zero()) {
        return std::unexpected(
            timeoutError("GATT discovery deadline expired while decoding the "
                         "object tree"));
      }

      return result;
    } catch (const std::bad_variant_access &) {
      return std::unexpected(
          error(Dmn_GattErrorKind::Decode,
                "BlueZ returned a malformed GATT property value"));
    }
  }

  auto
  startNotify(const std::string &deviceId, const std::string &characteristicId,
              NotificationHandler handler, std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<Dmn_GattNotification> {
    if (!pathWithinDevice(deviceId, characteristicId) || !handler) {
      return std::unexpected(error(Dmn_GattErrorKind::InvalidArgument,
                                   "notification requires a characteristic in "
                                   "the selected device and a callback"));
    }

    if (timeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(error(Dmn_GattErrorKind::InvalidArgument,
                                   "StartNotify timeout must be positive"));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] { return remainingTimeout(deadline); };
    const auto discoveryTimeout = remaining();
    if (discoveryTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "StartNotify deadline expired before discovering the GATT tree"));
    }

    auto services = discover(deviceId, discoveryTimeout);
    if (!services) {
      return std::unexpected(services.error());
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "StartNotify deadline expired before enabling notifications"));
    }

    const auto characteristic = std::find_if(
        services->begin(), services->end(),
        [&characteristicId](const auto &service) {
          return std::any_of(service.characteristics.begin(),
                             service.characteristics.end(),
                             [&characteristicId](const auto &candidate) {
                               return candidate.id == characteristicId;
                             });
        });
    if (characteristic == services->end()) {
      return std::unexpected(error(
          Dmn_GattErrorKind::InvalidArgument,
          "characteristic is not in the selected device's current GATT tree"));
    }

    const auto capability =
        std::find_if(characteristic->characteristics.begin(),
                     characteristic->characteristics.end(),
                     [&characteristicId](const auto &candidate) {
                       return candidate.id == characteristicId;
                     });
    if (!hasFlag(*capability, "notify") && !hasFlag(*capability, "indicate")) {
      return std::unexpected(
          error(Dmn_GattErrorKind::InvalidArgument,
                "characteristic does not support notifications"));
    }

    auto state = std::make_shared<NotificationState>();
    state->handler = std::move(handler);
    state->path = characteristicId;
    std::weak_ptr<NotificationState> weakState = state;

    // Install every observer before StartNotify can produce an immediate value.
    const auto propertyTimeout = remaining();
    if (propertyTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "StartNotify deadline expired before installing signal observers"));
    }

    auto propertySubscription = m_dbus.subscribeWithPath(
        {"org.bluez", std::string{kProperties}, "PropertiesChanged", {}, {}},
        [weakState, deviceId, characteristicId](
            std::string path, Dmn_DbusClient::Arguments arguments) {
          const auto state = weakState.lock();
          if (state == nullptr) {
            return;
          }

          if (arguments.size() != 3 || arguments[0].signature() != "s" ||
              arguments[1].signature() != "a{sv}") {
            return;
          }

          Dmn_GattEvent event;
          bool deliver{};

          {
            std::lock_guard lock{state->mutex};

            if (state->invalidated) {
              return;
            }

            if (path == characteristicId &&
                arguments[0].asString() == kGattCharacteristic) {
              const auto *value = property(arguments[1], "Value");
              if (value != nullptr && value->signature() == "ay") {
                event.kind = Dmn_GattEvent::Kind::ValueChanged;
                event.value = value->asByteArray();
                deliver = true;
              }
            } else if (path == deviceId && arguments[0].asString() == kDevice) {
              const auto connected = boolProperty(arguments[1], "Connected");
              const auto resolved =
                  boolProperty(arguments[1], "ServicesResolved");
              if ((connected && !*connected) || (resolved && !*resolved)) {
                state->invalidated = true;
                event.kind = Dmn_GattEvent::Kind::Invalidated;
                event.message =
                    "BlueZ device disconnected or GATT services changed";
                deliver = true;
              }
            }
          }

          if (deliver) {
            state->handler(event);
          }
        },
        propertyTimeout);
    if (!propertySubscription) {
      return std::unexpected(mapError(propertySubscription.error()));
    }

    state->property_subscription = std::move(*propertySubscription);

    const auto ownerTimeout = remaining();
    if (ownerTimeout <= std::chrono::milliseconds::zero()) {
      state->property_subscription.cancel();

      return std::unexpected(timeoutError(
          "StartNotify deadline expired before installing signal observers"));
    }

    auto ownerSubscription = m_dbus.subscribe(
        {"org.freedesktop.DBus",
         "org.freedesktop.DBus",
         "NameOwnerChanged",
         "/org/freedesktop/DBus",
         {}},
        [weakState](Dmn_DbusClient::Arguments arguments) {
          const auto state = weakState.lock();
          if (state == nullptr) {
            return;
          }

          if (arguments.size() == 3 && arguments[0].signature() == "s" &&
              arguments[0].asString() == kBluez &&
              arguments[1].signature() == "s" &&
              arguments[2].signature() == "s" &&
              arguments[1].asString() != arguments[2].asString() &&
              (!arguments[1].asString().empty() ||
               !arguments[2].asString().empty())) {
            bool deliver{};

            {
              std::lock_guard lock{state->mutex};

              deliver = !std::exchange(state->invalidated, true);
            }

            if (deliver) {
              state->handler({Dmn_GattEvent::Kind::Invalidated,
                              {},
                              "BlueZ service owner changed"});
            }
          }
        },
        ownerTimeout);
    if (!ownerSubscription) {
      state->property_subscription.cancel();

      return std::unexpected(mapError(ownerSubscription.error()));
    }

    state->owner_subscription = std::move(*ownerSubscription);

    const auto objectTimeout = remaining();
    if (objectTimeout <= std::chrono::milliseconds::zero()) {
      state->property_subscription.cancel();
      state->owner_subscription.cancel();

      return std::unexpected(timeoutError(
          "StartNotify deadline expired before installing signal observers"));
    }

    auto objectSubscription = m_dbus.subscribe(
        {"org.bluez",
         std::string{kObjectManager},
         "InterfacesRemoved",
         "/",
         {}},
        [weakState, deviceId,
         characteristicId](Dmn_DbusClient::Arguments arguments) {
          const auto state = weakState.lock();
          if (state == nullptr) {
            return;
          }

          if (arguments.size() != 2 || arguments[0].signature() != "o" ||
              arguments[1].signature() != "as") {
            return;
          }

          const auto &removedPath = arguments[0].asObjectPath();
          if (removedPath != characteristicId && removedPath != deviceId &&
              !pathWithinDevice(deviceId, removedPath)) {
            return;
          }

          bool deliver{};

          {
            std::lock_guard lock{state->mutex};

            deliver = !std::exchange(state->invalidated, true);
          }

          if (deliver) {
            state->handler({Dmn_GattEvent::Kind::Invalidated,
                            {},
                            "BlueZ removed the selected GATT object"});
          }
        },
        objectTimeout);
    if (!objectSubscription) {
      state->property_subscription.cancel();
      state->owner_subscription.cancel();

      return std::unexpected(mapError(objectSubscription.error()));
    }

    state->object_subscription = std::move(*objectSubscription);

    if (remaining() <= std::chrono::milliseconds::zero()) {
      state->property_subscription.cancel();
      state->owner_subscription.cancel();
      state->object_subscription.cancel();

      return std::unexpected(timeoutError(
          "StartNotify deadline expired before enabling notifications"));
    }

    const auto startTimeout = remaining();
    if (startTimeout <= std::chrono::milliseconds::zero()) {
      state->property_subscription.cancel();
      state->owner_subscription.cancel();
      state->object_subscription.cancel();

      return std::unexpected(timeoutError(
          "StartNotify deadline expired before enabling notifications"));
    }

    auto called = m_dbus.call(std::string{kBluez}, characteristicId,
                              std::string{kGattCharacteristic}, "StartNotify",
                              {}, startTimeout);
    if (!called) {
      state->property_subscription.cancel();
      state->owner_subscription.cancel();
      state->object_subscription.cancel();

      return std::unexpected(mapError(called.error()));
    }

    {
      std::lock_guard lock{state->mutex};

      state->started = !state->invalidated;
    }

    const auto rollback = [this, &state](Dmn_GattError failure)
        -> Dmn_GattValueResult<Dmn_GattNotification> {
      auto stopped = stopNotify(state);
      if (!stopped) {
        failure.message +=
            "; notification cleanup failed: " + stopped.error().message;
      }

      return std::unexpected(std::move(failure));
    };

    bool invalidated{};
    {
      std::lock_guard lock{state->mutex};

      invalidated = state->invalidated;
    }

    if (invalidated) {
      return rollback(error(Dmn_GattErrorKind::NotReady,
                            "GATT notification was invalidated during "
                            "StartNotify"));
    }

    const auto notifyingTimeout = remaining();
    if (notifyingTimeout <= std::chrono::milliseconds::zero()) {
      return rollback(timeoutError(
          "StartNotify deadline expired while verifying notification state"));
    }

    auto notifying = m_dbus.call(
        std::string{kBluez}, characteristicId, std::string{kProperties}, "Get",
        {Dmn_DbusValue{std::string{kGattCharacteristic}},
         Dmn_DbusValue{"Notifying"}},
        notifyingTimeout);
    if (!notifying) {
      return rollback(mapError(notifying.error()));
    }

    if (notifying->size() != 1 || notifying->front().signature() != "v" ||
        notifying->front().asVariant().signature() != "b") {
      return rollback(
          error(Dmn_GattErrorKind::Decode,
                "BlueZ returned an invalid GATT Notifying property"));
    }

    if (!notifying->front().asVariant().asBool()) {
      return rollback(error(
          Dmn_GattErrorKind::NotReady,
          "BlueZ accepted StartNotify but reports notification delivery is "
          "not active; the connected bearer may not provide live GATT"));
    }

    {
      std::lock_guard lock{state->mutex};

      invalidated = state->invalidated;
    }

    if (invalidated) {
      return rollback(error(Dmn_GattErrorKind::NotReady,
                            "GATT notification was invalidated during "
                            "StartNotify"));
    }

    auto self = shared_from_this();

    return Dmn_GattNotification{
        [self = std::move(self), state] { return self->stopNotify(state); }};
  }

  auto stopNotify(const std::shared_ptr<NotificationState> &state)
      -> Dmn_GattResult {
    bool started{};
    bool invalidated{};

    {
      std::lock_guard lock{state->mutex};

      started = state->started;
      invalidated = state->invalidated;
    }

    if (started && !invalidated) {
      auto result =
          m_dbus.call(std::string{kBluez}, state->path,
                      std::string{kGattCharacteristic}, "StopNotify", {});
      if (!result) {
        return std::unexpected(mapError(result.error()));
      }
    }

    {
      std::lock_guard lock{state->mutex};

      state->started = false;
    }

    state->property_subscription.cancel();
    state->owner_subscription.cancel();
    state->object_subscription.cancel();

    return {};
  }

  auto writeWithResponse(const std::string &deviceId,
                         const std::string &characteristicId,
                         const std::vector<std::uint8_t> &value,
                         std::chrono::milliseconds timeout) -> Dmn_GattResult {
    if (!pathWithinDevice(deviceId, characteristicId)) {
      return std::unexpected(
          error(Dmn_GattErrorKind::InvalidArgument,
                "characteristic does not belong to selected device"));
    }

    if (timeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(error(Dmn_GattErrorKind::InvalidArgument,
                                   "WriteValue timeout must be positive"));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto remaining = [&] { return remainingTimeout(deadline); };

    const auto discoveryTimeout = remaining();
    if (discoveryTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "WriteValue deadline expired before discovering the GATT tree"));
    }

    auto services = discover(deviceId, discoveryTimeout);
    if (!services) {
      return std::unexpected(services.error());
    }

    if (remaining() <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "WriteValue deadline expired before writing the characteristic"));
    }

    const auto present = std::any_of(
        services->begin(), services->end(),
        [&characteristicId](const auto &service) {
          return std::any_of(service.characteristics.begin(),
                             service.characteristics.end(),
                             [&characteristicId](const auto &candidate) {
                               return candidate.id == characteristicId &&
                                      hasFlag(candidate, "write");
                             });
        });
    if (!present) {
      return std::unexpected(error(
          Dmn_GattErrorKind::InvalidArgument,
          "characteristic is absent or does not support acknowledged writes"));
    }

    std::vector<std::pair<Dmn_DbusValue, Dmn_DbusValue>> options;
    options.emplace_back(Dmn_DbusValue{"type"},
                         Dmn_DbusValue::variant(Dmn_DbusValue{"request"}));

    const auto writeTimeout = remaining();
    if (writeTimeout <= std::chrono::milliseconds::zero()) {
      return std::unexpected(timeoutError(
          "WriteValue deadline expired before writing the characteristic"));
    }

    const auto result =
        m_dbus.call(std::string{kBluez}, characteristicId,
                    std::string{kGattCharacteristic}, "WriteValue",
                    {Dmn_DbusValue::byteArray(value),
                     Dmn_DbusValue::dictionary("s", "v", std::move(options))},
                    writeTimeout);
    if (!result) {
      return std::unexpected(mapError(result.error()));
    }

    return {};
  }

private:
  Dmn_DbusClient m_dbus;
};

Dmn_BluezGattClient::Dmn_BluezGattClient(const Dmn_BluezClientConfig &config)
    : m_impl{std::make_shared<Impl>(config)} {}

Dmn_BluezGattClient::~Dmn_BluezGattClient() = default;

auto Dmn_BluezGattClient::waitForServicesResolved(
    const std::string &deviceId,
    std::chrono::milliseconds timeout) -> Dmn_GattResult {
  return m_impl->waitForServicesResolved(deviceId, timeout);
}

auto Dmn_BluezGattClient::discover(const std::string &deviceId,
                                   std::chrono::milliseconds timeout)
    -> Dmn_GattValueResult<std::vector<Dmn_GattService>> {
  return m_impl->discover(deviceId, timeout);
}

auto Dmn_BluezGattClient::startNotify(const std::string &deviceId,
                                      const std::string &characteristicId,
                                      NotificationHandler handler,
                                      std::chrono::milliseconds timeout)
    -> Dmn_GattValueResult<Dmn_GattNotification> {
  return m_impl->startNotify(deviceId, characteristicId, std::move(handler),
                             timeout);
}

auto Dmn_BluezGattClient::writeWithResponse(
    const std::string &deviceId, const std::string &characteristicId,
    const std::vector<std::uint8_t> &value,
    std::chrono::milliseconds timeout) -> Dmn_GattResult {
  return m_impl->writeWithResponse(deviceId, characteristicId, value, timeout);
}

} // namespace dmn
