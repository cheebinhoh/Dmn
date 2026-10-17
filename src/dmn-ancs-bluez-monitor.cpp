/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-ancs-bluez-monitor.cpp
 * @brief Reusable selected-device setup and ANCS orchestration.
 */

#include "dmn-ancs-bluez-monitor.hpp"

#include "dmn-bluez-gatt-client.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

namespace dmn {
namespace {

using namespace std::chrono_literals;

auto isBluetoothAddress(std::string_view value) -> bool {
  if (value.size() != 17) {
    return false;
  }

  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index % 3 == 2) {
      if (value[index] != ':') {
        return false;
      }
    } else {
      const auto character = value[index];
      const bool isHex = (character >= '0' && character <= '9') ||
                         (character >= 'A' && character <= 'F') ||
                         (character >= 'a' && character <= 'f');
      if (!isHex) {
        return false;
      }
    }
  }

  return true;
}

auto normalizedAddress(std::string_view value) -> std::string {
  std::string normalized{value};
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(character >= 'a' && character <= 'z'
                                                ? character - ('a' - 'A')
                                                : character);
                 });

  return normalized;
}

auto asciiCaseEqual(std::string_view left, std::string_view right) -> bool {
  if (left.size() != right.size()) {
    return false;
  }

  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto leftByte = static_cast<unsigned char>(left[index]);
    const auto rightByte = static_cast<unsigned char>(right[index]);
    const auto foldedLeft =
        leftByte >= 'A' && leftByte <= 'Z' ? leftByte + ('a' - 'A') : leftByte;
    const auto foldedRight = rightByte >= 'A' && rightByte <= 'Z'
                                 ? rightByte + ('a' - 'A')
                                 : rightByte;
    if (foldedLeft != foldedRight) {
      return false;
    }
  }

  return true;
}

auto matchesAdapter(const Dmn_BluezAdapter &adapter,
                    std::string_view selector) -> bool {
  return adapter.object_path == selector ||
         normalizedAddress(adapter.address) == normalizedAddress(selector);
}

auto selectDevice(const std::vector<Dmn_BluezDevice> &devices,
                  std::string_view selector, std::string_view adapterPath)
    -> std::vector<Dmn_BluezDevice> {
  const bool addressSelector = isBluetoothAddress(selector);
  std::vector<Dmn_BluezDevice> matches;
  for (const auto &device : devices) {
    if (!adapterPath.empty() && device.adapter_path != adapterPath) {
      continue;
    }

    const bool matched =
        addressSelector
            ? normalizedAddress(device.address) == normalizedAddress(selector)
            : asciiCaseEqual(device.name, selector);
    if (matched) {
      matches.push_back(device);
    }
  }

  return matches;
}

auto findSelectedAdapter(const Dmn_BluezInventory &inventory,
                         const std::optional<std::string> &selector)
    -> std::optional<Dmn_BluezAdapter> {
  std::vector<Dmn_BluezAdapter> powered;
  std::copy_if(inventory.adapters.begin(), inventory.adapters.end(),
               std::back_inserter(powered), [](const auto &adapter) {
                 return adapter.powered.value_or(false);
               });

  if (selector) {
    std::vector<Dmn_BluezAdapter> matches;
    std::copy_if(powered.begin(), powered.end(), std::back_inserter(matches),
                 [&selector](const auto &adapter) {
                   return matchesAdapter(adapter, *selector);
                 });
    if (matches.size() == 1) {
      return matches.front();
    }

    return std::nullopt;
  }

  if (powered.size() == 1) {
    return powered.front();
  }

  return std::nullopt;
}

auto findDeviceAdapter(const Dmn_BluezInventory &inventory,
                       const Dmn_BluezDevice &device)
    -> std::optional<Dmn_BluezAdapter> {
  const auto found =
      std::find_if(inventory.adapters.begin(), inventory.adapters.end(),
                   [&device](const auto &adapter) {
                     return adapter.object_path == device.adapter_path;
                   });
  if (found == inventory.adapters.end()) {
    return std::nullopt;
  }

  return *found;
}

auto makeError(Dmn_AncsMonitorStage stage,
               const Dmn_DbusError &error) -> Dmn_AncsMonitorError {
  return {stage, error.message, error.remote_name, std::nullopt};
}

auto validTimeout(std::chrono::milliseconds timeout) -> bool {
  return timeout > 0ms && timeout <= 600s;
}

auto remainingTimeout(std::chrono::steady_clock::time_point deadline)
    -> std::chrono::milliseconds {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return std::chrono::milliseconds::zero();
  }

  return std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
}

auto validConfig(const Dmn_AncsMonitorConfig &config) -> bool {
  return validTimeout(config.discovery_timeout) &&
         validTimeout(config.pairing_timeout) &&
         validTimeout(config.connection_timeout) &&
         validTimeout(config.ancs.service_resolution_timeout) &&
         validTimeout(config.ancs.gatt_operation_timeout) &&
         validTimeout(config.ancs.attribute_response_timeout) &&
         (!config.adapter_selector || !config.adapter_selector->empty());
}

} // namespace

class Dmn_AncsBluezMonitor::Impl {
public:
  explicit Impl(const Dmn_BluezClientConfig &config)
      : m_bluez{config}, m_gatt{config}, m_ancs{m_gatt} {}

  auto setEventHandler(Dmn_AncsClient::EventHandler handler) -> Dmn_AncsResult {
    return m_ancs.setEventHandler(std::move(handler));
  }

  auto start(const std::string &deviceSelector,
             const Dmn_AncsMonitorConfig &config) -> Dmn_AncsMonitorResult {
    std::lock_guard apiLock{m_api_mutex};
    if (deviceSelector.empty() || !validConfig(config)) {
      return std::unexpected(
          Dmn_AncsMonitorError{Dmn_AncsMonitorStage::InvalidArgument,
                               "device selector and setup deadlines must be "
                               "valid",
                               {},
                               std::nullopt});
    }

    if (m_ancs.state() != Dmn_AncsState::Stopped) {
      return std::unexpected(
          Dmn_AncsMonitorError{Dmn_AncsMonitorStage::NotReady,
                               "ANCS monitor is already active",
                               {},
                               std::nullopt});
    }

    reportProgress(config, Dmn_AncsMonitorProgress::Inventory);
    auto inventory = m_bluez.snapshot();
    if (!inventory) {
      return std::unexpected(
          makeError(Dmn_AncsMonitorStage::Inventory, inventory.error()));
    }

    std::optional<Dmn_BluezAdapter> requestedAdapter;
    if (config.adapter_selector) {
      std::vector<Dmn_BluezAdapter> adapterMatches;
      std::copy_if(inventory->adapters.begin(), inventory->adapters.end(),
                   std::back_inserter(adapterMatches),
                   [&config](const auto &candidate) {
                     return matchesAdapter(candidate, *config.adapter_selector);
                   });
      if (adapterMatches.size() != 1) {
        return std::unexpected(Dmn_AncsMonitorError{
            Dmn_AncsMonitorStage::Inventory,
            "adapter selector does not match exactly one adapter",
            {},
            std::nullopt});
      }

      requestedAdapter = std::move(adapterMatches.front());
    }

    const auto selectedAdapterPath =
        requestedAdapter ? requestedAdapter->object_path : std::string{};
    auto matches =
        selectDevice(inventory->devices, deviceSelector, selectedAdapterPath);
    std::optional<Dmn_BluezAdapter> adapter = requestedAdapter;
    if (matches.size() == 1) {
      if (!adapter) {
        adapter = findDeviceAdapter(*inventory, matches.front());
      }
    } else if (matches.size() > 1) {
      return std::unexpected(Dmn_AncsMonitorError{
          Dmn_AncsMonitorStage::DeviceNotFound,
          "device selector is ambiguous; specify an adapter",
          {},
          std::nullopt});
    }

    if (matches.empty()) {
      adapter = findSelectedAdapter(inventory.value(), config.adapter_selector);
      if (!adapter) {
        return std::unexpected(Dmn_AncsMonitorError{
            Dmn_AncsMonitorStage::Discovery,
            "no unique powered adapter is available; specify an adapter",
            {},
            std::nullopt});
      }

      reportProgress(config, Dmn_AncsMonitorProgress::Discovery);
      auto discovered =
          discoverDevice(deviceSelector, *adapter, config.discovery_timeout);
      if (!discovered) {
        return std::unexpected(discovered.error());
      }

      matches.push_back(std::move(discovered->first));
      adapter = std::move(discovered->second);

      inventory = m_bluez.snapshot();
      if (!inventory) {
        return std::unexpected(
            makeError(Dmn_AncsMonitorStage::Inventory, inventory.error()));
      }

      matches = selectDevice(inventory->devices, deviceSelector,
                             adapter->object_path);
    }

    if (matches.empty()) {
      return std::unexpected(Dmn_AncsMonitorError{
          Dmn_AncsMonitorStage::DeviceNotFound,
          "device was not found in the current BlueZ inventory",
          {},
          std::nullopt});
    }

    if (matches.size() != 1) {
      return std::unexpected(Dmn_AncsMonitorError{
          Dmn_AncsMonitorStage::DeviceNotFound,
          "device selector is ambiguous; specify an adapter",
          {},
          std::nullopt});
    }

    auto target = matches.front();
    if (!adapter) {
      adapter = findDeviceAdapter(inventory.value(), target);
    }

    if (!adapter) {
      return std::unexpected(
          Dmn_AncsMonitorError{Dmn_AncsMonitorStage::Inventory,
                               "selected device has no current adapter",
                               {},
                               std::nullopt});
    }

    if (!adapter->powered.value_or(false)) {
      return std::unexpected(
          Dmn_AncsMonitorError{Dmn_AncsMonitorStage::Inventory,
                               "selected Bluetooth adapter is not powered",
                               {},
                               std::nullopt});
    }

    const auto actionSelector =
        target.address.empty() ? target.object_path : target.address;
    if (!target.paired.value_or(false)) {
      reportProgress(config, Dmn_AncsMonitorProgress::Pairing);
      const auto pairingDeadline =
          std::chrono::steady_clock::now() + config.pairing_timeout;
      const auto pairingCallTimeout = remainingTimeout(pairingDeadline);
      if (pairingCallTimeout <= 0ms) {
        return std::unexpected(Dmn_AncsMonitorError{
            Dmn_AncsMonitorStage::Pairing,
            "pairing deadline expired before the BlueZ request",
            {},
            std::nullopt});
      }

      auto paired = m_bluez.pairDevice(actionSelector, adapter->object_path,
                                       pairingCallTimeout);
      if (!paired) {
        return std::unexpected(
            makeError(Dmn_AncsMonitorStage::Pairing, paired.error()));
      }

      auto confirmed =
          waitForDeviceState(target, *adapter, true, std::nullopt,
                             pairingDeadline, Dmn_AncsMonitorStage::Pairing);
      if (!confirmed) {
        return std::unexpected(confirmed.error());
      }

      target = std::move(confirmed->device);
    }

    const bool alreadyConnected = target.connected.value_or(false);
    reportProgress(config, Dmn_AncsMonitorProgress::Connection);
    if (!alreadyConnected) {
      const auto connectionDeadline =
          std::chrono::steady_clock::now() + config.connection_timeout;
      const auto connectionCallTimeout = remainingTimeout(connectionDeadline);
      if (connectionCallTimeout <= 0ms) {
        return std::unexpected(Dmn_AncsMonitorError{
            Dmn_AncsMonitorStage::Connection,
            "connection deadline expired before the BlueZ request",
            {},
            std::nullopt});
      }

      auto connected = m_bluez.connectDevice(
          actionSelector, adapter->object_path, connectionCallTimeout);
      if (!connected) {
        return std::unexpected(
            makeError(Dmn_AncsMonitorStage::Connection, connected.error()));
      }

      auto confirmed = waitForDeviceState(
          target, *adapter, target.paired.value_or(false), true,
          connectionDeadline, Dmn_AncsMonitorStage::Connection);
      if (!confirmed) {
        return std::unexpected(confirmed.error());
      }

      target = std::move(confirmed->device);
    } else {
      auto connected = m_bluez.connectAvailableProfiles(
          actionSelector, adapter->object_path, config.connection_timeout);
      if (!connected) {
        return std::unexpected(
            makeError(Dmn_AncsMonitorStage::Connection, connected.error()));
      }
    }

    reportProgress(config, Dmn_AncsMonitorProgress::AncsStartup);
    auto ancsStarted = m_ancs.start(target.object_path, config.ancs);
    if (!ancsStarted) {
      const auto cleanup = m_ancs.stop();
      if (!cleanup) {
        return std::unexpected(Dmn_AncsMonitorError{
            Dmn_AncsMonitorStage::Cleanup, cleanup.error().message,
            cleanup.error().remote_name, cleanup.error().kind});
      }

      return std::unexpected(Dmn_AncsMonitorError{
          Dmn_AncsMonitorStage::AncsStartup, ancsStarted.error().message,
          ancsStarted.error().remote_name, ancsStarted.error().kind});
    }

    return Dmn_AncsMonitorSession{std::move(*adapter), std::move(target),
                                  alreadyConnected};
  }

  auto stop() -> Dmn_AncsResult {
    std::lock_guard apiLock{m_api_mutex};

    return m_ancs.stop();
  }

  auto state() const -> Dmn_AncsState { return m_ancs.state(); }

private:
  void reportProgress(const Dmn_AncsMonitorConfig &config,
                      Dmn_AncsMonitorProgress progress) {
    if (config.progress_handler) {
      config.progress_handler(progress);
    }
  }

  auto discoverDevice(const std::string &selector,
                      const Dmn_BluezAdapter &adapter,
                      std::chrono::milliseconds timeout)
      -> std::expected<std::pair<Dmn_BluezDevice, Dmn_BluezAdapter>,
                       Dmn_AncsMonitorError> {
    auto discovery = m_bluez.startDiscovery(adapter.object_path);
    if (!discovery) {
      return std::unexpected(
          makeError(Dmn_AncsMonitorStage::Discovery, discovery.error()));
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<Dmn_BluezDevice> matches;
    do {
      matches = selectDevice(m_bluez.devices(), selector, adapter.object_path);
      if (isBluetoothAddress(selector) && !matches.empty()) {
        break;
      }

      std::this_thread::sleep_for(100ms);
    } while (std::chrono::steady_clock::now() < deadline);

    auto stopped = m_bluez.stopDiscovery();
    if (!stopped) {
      return std::unexpected(
          makeError(Dmn_AncsMonitorStage::Discovery, stopped.error()));
    }

    if (matches.empty()) {
      return std::unexpected(Dmn_AncsMonitorError{
          Dmn_AncsMonitorStage::DeviceNotFound,
          "device was not found before the discovery deadline",
          {},
          std::nullopt});
    }

    if (matches.size() != 1) {
      return std::unexpected(Dmn_AncsMonitorError{
          Dmn_AncsMonitorStage::DeviceNotFound,
          "device selector is ambiguous; specify an adapter",
          {},
          std::nullopt});
    }

    return std::pair{std::move(matches.front()), adapter};
  }

  auto waitForDeviceState(const Dmn_BluezDevice &selected,
                          const Dmn_BluezAdapter &adapter, bool paired,
                          std::optional<bool> connected,
                          std::chrono::steady_clock::time_point deadline,
                          Dmn_AncsMonitorStage stage)
      -> std::expected<Dmn_AncsMonitorSession, Dmn_AncsMonitorError> {
    while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshotTimeout = std::min(2000ms, remainingTimeout(deadline));
      auto inventory = m_bluez.snapshot(snapshotTimeout);
      if (!inventory) {
        return std::unexpected(makeError(stage, inventory.error()));
      }

      const auto selector =
          selected.address.empty() ? selected.object_path : selected.address;
      auto matches =
          selectDevice(inventory->devices, selector, adapter.object_path);
      if (matches.size() == 1 &&
          matches.front().paired.value_or(false) == paired &&
          (!connected ||
           matches.front().connected.value_or(false) == *connected)) {
        return Dmn_AncsMonitorSession{adapter, std::move(matches.front()),
                                      false};
      }

      std::this_thread::sleep_for(std::min(100ms, remainingTimeout(deadline)));
    }

    return std::unexpected(Dmn_AncsMonitorError{
        stage,
        connected ? "device connection state was not confirmed"
                  : "device pairing state was not confirmed",
        {},
        std::nullopt});
  }

  Dmn_BluezClient m_bluez;
  Dmn_BluezGattClient m_gatt;
  Dmn_AncsClient m_ancs;
  std::mutex m_api_mutex;
};

Dmn_AncsBluezMonitor::Dmn_AncsBluezMonitor(
    const Dmn_BluezClientConfig &bluezConfig)
    : m_impl{std::make_unique<Impl>(bluezConfig)} {}

Dmn_AncsBluezMonitor::~Dmn_AncsBluezMonitor() noexcept = default;

auto Dmn_AncsBluezMonitor::setEventHandler(Dmn_AncsClient::EventHandler handler)
    -> Dmn_AncsResult {
  return m_impl->setEventHandler(std::move(handler));
}

auto Dmn_AncsBluezMonitor::start(const std::string &deviceSelector,
                                 const Dmn_AncsMonitorConfig &config)
    -> Dmn_AncsMonitorResult {
  return m_impl->start(deviceSelector, config);
}

auto Dmn_AncsBluezMonitor::stop() -> Dmn_AncsResult { return m_impl->stop(); }

auto Dmn_AncsBluezMonitor::state() const -> Dmn_AncsState {
  return m_impl->state();
}

} // namespace dmn
