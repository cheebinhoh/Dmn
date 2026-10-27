/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-backend.cpp
 * @brief Engine backend over the existing BlueZ client and ANCS monitor.
 *
 * All Bluetooth behavior comes from Dmn_BluezClient (inventory, discovery,
 * connect) and Dmn_AncsBluezMonitor (pairing-aware ANCS startup over the GATT
 * and D-Bus layers). This file only adapts them to Dmn_EngineBackend.
 */

#include "app/gui/engine/dmn-engine-backend.hpp"

#include "dmn-ancs-bluez-monitor.hpp"
#include "dmn-bluez-client.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace dmn::gui {
namespace {

constexpr std::chrono::milliseconds kDeviceStateTimeout{30000};
constexpr std::chrono::milliseconds kDeviceSnapshotTimeout{2000};
constexpr std::chrono::milliseconds kDeviceStatePollInterval{50};

auto lower(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(), [](const auto ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });

  return text;
}

auto stateName(const Dmn_AncsState state) -> const char * {
  switch (state) {
  case Dmn_AncsState::Stopped:
    return "stopped";
  case Dmn_AncsState::Starting:
    return "starting";
  case Dmn_AncsState::ResolvingServices:
    return "resolving_services";
  case Dmn_AncsState::DiscoveringANCS:
    return "discovering_ancs";
  case Dmn_AncsState::SubscribingDataSource:
    return "subscribing_data_source";
  case Dmn_AncsState::SubscribingNotificationSource:
    return "subscribing_notification_source";
  case Dmn_AncsState::Ready:
    return "ready";
  case Dmn_AncsState::Stopping:
    return "stopping";
  case Dmn_AncsState::Failed:
    return "failed";
  }

  return "unknown";
}

auto progressText(const Dmn_AncsMonitorProgress progress) -> const char * {
  switch (progress) {
  case Dmn_AncsMonitorProgress::Inventory:
    return "Resolving device";
  case Dmn_AncsMonitorProgress::Discovery:
    return "Scanning for device";
  case Dmn_AncsMonitorProgress::Pairing:
    return "Pairing";
  case Dmn_AncsMonitorProgress::Connection:
    return "Connecting profiles";
  case Dmn_AncsMonitorProgress::AncsStartup:
    return "Subscribing to ANCS";
  }

  return "Starting";
}

auto stageName(const Dmn_AncsMonitorStage stage) -> const char * {
  switch (stage) {
  case Dmn_AncsMonitorStage::AncsStartup:
    return "ANCS startup";
  case Dmn_AncsMonitorStage::Cancelled:
    return "Canceled";
  case Dmn_AncsMonitorStage::Cleanup:
    return "Cleanup";
  case Dmn_AncsMonitorStage::Connection:
    return "Connection";
  case Dmn_AncsMonitorStage::DeviceNotFound:
    return "Device not found";
  case Dmn_AncsMonitorStage::Discovery:
    return "Discovery";
  case Dmn_AncsMonitorStage::InvalidArgument:
    return "Invalid argument";
  case Dmn_AncsMonitorStage::Inventory:
    return "Inventory";
  case Dmn_AncsMonitorStage::NotReady:
    return "Not ready";
  case Dmn_AncsMonitorStage::Pairing:
    return "Pairing";
  }

  return "Monitor";
}

auto notificationFrom(const Dmn_AncsNotification &source)
    -> Dmn_BackendNotification {
  Dmn_BackendNotification notification;
  switch (source.kind) {
  case Dmn_AncsNotificationKind::Added:
    notification.m_kind = "added";
    break;
  case Dmn_AncsNotificationKind::AttributesUnavailable:
    notification.m_kind = "attributes_unavailable";
    break;
  case Dmn_AncsNotificationKind::Removed:
    notification.m_kind = "removed";
    break;
  case Dmn_AncsNotificationKind::Updated:
    notification.m_kind = "updated";
    break;
  }

  notification.m_uid = source.uid;
  notification.m_category = source.category;
  notification.m_flags = source.flags;
  notification.m_app_identifier = source.app_identifier;
  notification.m_title = source.title;
  notification.m_subtitle = source.subtitle;
  notification.m_message = source.message;
  notification.m_date = source.date;

  return notification;
}

auto toEvent(const Dmn_AncsEvent &event) -> Dmn_BackendMonitorEvent {
  Dmn_BackendMonitorEvent result;
  switch (event.kind) {
  case Dmn_AncsEvent::Kind::StateChanged:
    result.m_kind = Dmn_BackendMonitorEvent::Kind::kState;
    if (event.state) {
      result.m_state = stateName(*event.state);
      result.m_terminal = *event.state == Dmn_AncsState::Failed ||
                          *event.state == Dmn_AncsState::Stopped;
    }

    break;
  case Dmn_AncsEvent::Kind::Error:
    result.m_kind = Dmn_BackendMonitorEvent::Kind::kError;
    if (event.error) {
      result.m_error = event.error->message;
      result.m_terminal = event.error->fatal;
    }

    break;
  default:
    result.m_kind = Dmn_BackendMonitorEvent::Kind::kNotification;
    if (event.notification) {
      result.m_notification = notificationFrom(*event.notification);
    }

    if (event.kind == Dmn_AncsEvent::Kind::AttributesUnavailable &&
        !result.m_notification) {
      result.m_notification = Dmn_BackendNotification{};
      result.m_notification->m_kind = "attributes_unavailable";
    }

    break;
  }

  return result;
}

class Dmn_BluezEngineBackend final : public Dmn_EngineBackend {
public:
  Dmn_BluezEngineBackend()
      : m_notification_cutoff{std::chrono::system_clock::now()} {}

  ~Dmn_BluezEngineBackend() override {
    std::map<std::string, std::unique_ptr<Dmn_AncsBluezMonitor>> monitors;
    {
      std::lock_guard lock{m_monitor_mutex};
      monitors.swap(m_monitors);
    }

    for (auto &[key, monitor] : monitors) {
      (void)monitor->stop();
    }
  }

  [[nodiscard]] auto name() const -> std::string override { return "bluez"; }

  void setHandlers(InventoryChangedHandler inventoryChanged,
                   MonitorEventHandler monitorEvent) override {
    std::lock_guard lock{m_handler_mutex};
    m_inventory_changed = std::move(inventoryChanged);
    m_monitor_event = std::move(monitorEvent);
  }

  auto
  snapshot() -> std::expected<Dmn_BackendInventory, Dmn_BackendError> override {
    std::lock_guard lock{m_mutex};

    return snapshotLocked(std::chrono::milliseconds::zero());
  }

  auto setScanning(const bool enabled, const std::string &adapterPath)
      -> Dmn_BackendResult override {
    std::lock_guard lock{m_mutex};
    try {
      auto result = enabled ? client().startDiscovery(adapterPath)
                            : client().stopDiscovery();
      if (!result) {
        return std::unexpected(Dmn_BackendError{result.error().message});
      }
    } catch (const std::exception &exception) {
      return std::unexpected(Dmn_BackendError{exception.what()});
    }

    return {};
  }

  auto
  connect(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult override {
    return changeConnection(identity, true);
  }

  auto
  disconnect(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult override {
    return changeConnection(identity, false);
  }

  void cancelConnect(const Dmn_DeviceIdentity &identity) override {
    m_cancel_connect.store(true, std::memory_order_release);
    try {
      // Intentionally not serialized: it must interrupt a blocked connect().
      (void)client().cancelConnectionAttempt(identity.m_device_address,
                                             identity.m_adapter_address,
                                             kDeviceStateTimeout);
    } catch (const std::exception &) {
      // The pending connect() reports its own outcome.
    }
  }

  auto
  startMonitor(const Dmn_DeviceIdentity &identity,
               const std::atomic<bool> &cancel,
               const ProgressHandler &progress) -> Dmn_BackendResult override {
    try {
      auto monitor = std::make_unique<Dmn_AncsBluezMonitor>();
      const auto handlerResult = monitor->setEventHandler(
          [this, identity](const Dmn_AncsEvent &event) {
            MonitorEventHandler handler;
            {
              std::lock_guard lock{m_handler_mutex};
              handler = m_monitor_event;
            }

            if (handler) {
              handler(identity, toEvent(event));
            }
          });
      if (!handlerResult) {
        return std::unexpected(Dmn_BackendError{handlerResult.error().message});
      }

      Dmn_AncsMonitorConfig config;
      config.adapter_selector = identity.m_adapter_address;
      config.require_existing_connection = true;
      config.request_remaining_profiles_when_connected = true;
      config.ancs.notification_date_cutoff = m_notification_cutoff;
      config.cancellation_requested = [&cancel] {
        return cancel.load(std::memory_order_acquire);
      };
      config.progress_handler =
          [&progress](const Dmn_AncsMonitorProgress step) {
            if (progress) {
              progress(progressText(step));
            }
          };

      auto started = monitor->start(identity.m_device_address, config);
      if (!started) {
        const auto cancelled =
            started.error().stage == Dmn_AncsMonitorStage::Cancelled;

        return std::unexpected(
            Dmn_BackendError{std::string{stageName(started.error().stage)} +
                                 ": " + started.error().message,
                             cancelled});
      }

      if (cancel.load(std::memory_order_acquire)) {
        (void)monitor->stop();

        return std::unexpected(Dmn_BackendError{"Canceled", true});
      }

      std::lock_guard lock{m_monitor_mutex};
      m_monitors[key(identity)] = std::move(monitor);
    } catch (const std::exception &exception) {
      return std::unexpected(Dmn_BackendError{exception.what()});
    }

    return {};
  }

  auto stopMonitor(const Dmn_DeviceIdentity &identity)
      -> Dmn_BackendResult override {
    std::unique_ptr<Dmn_AncsBluezMonitor> monitor;
    {
      std::lock_guard lock{m_monitor_mutex};
      const auto found = m_monitors.find(key(identity));
      if (found == m_monitors.end()) {
        return {};
      }

      monitor = std::move(found->second);
      m_monitors.erase(found);
    }

    try {
      auto stopped = monitor->stop();
      if (!stopped) {
        return std::unexpected(Dmn_BackendError{stopped.error().message});
      }
    } catch (const std::exception &exception) {
      return std::unexpected(Dmn_BackendError{exception.what()});
    }

    return {};
  }

private:
  static auto key(const Dmn_DeviceIdentity &identity) -> std::string {
    return lower(identity.m_adapter_address) + "/" +
           lower(identity.m_device_address);
  }

  auto client() -> Dmn_BluezClient & {
    std::lock_guard lock{m_client_mutex};
    if (!m_client) {
      m_client = std::make_unique<Dmn_BluezClient>();
      m_client->setEventHandler([this](const Dmn_BluezEvent &event) {
        if (event.kind == Dmn_BluezEventKind::Error) {
          return;
        }

        InventoryChangedHandler handler;
        {
          std::lock_guard handlerLock{m_handler_mutex};
          handler = m_inventory_changed;
        }

        if (handler) {
          handler();
        }
      });
    }

    return *m_client;
  }

  auto snapshotLocked(const std::chrono::milliseconds timeout)
      -> std::expected<Dmn_BackendInventory, Dmn_BackendError> {
    try {
      auto inventory = client().snapshot(timeout);
      if (!inventory) {
        return std::unexpected(Dmn_BackendError{inventory.error().message});
      }

      Dmn_BackendInventory result;
      std::map<std::string, std::string> adapterAddress;
      for (const auto &adapter : inventory->adapters) {
        adapterAddress[adapter.object_path] = adapter.address;
        result.m_adapters.push_back({adapter.object_path, adapter.address,
                                     adapter.alias.value_or(adapter.name),
                                     adapter.powered.value_or(false)});
      }

      for (const auto &device : inventory->devices) {
        const auto adapter = adapterAddress.find(device.adapter_path);
        if (adapter == adapterAddress.end()) {
          continue;
        }

        Dmn_DeviceEntry entry;
        entry.m_identity = {adapter->second, device.address};
        entry.m_display_name =
            device.alias.value_or(device.reported_name.value_or(device.name));
        entry.m_paired = device.paired;
        entry.m_connected = device.connected;
        entry.m_present = true;
        entry.m_seen_during_discovery = device.seen_during_discovery;
        result.m_devices.push_back(std::move(entry));
      }

      return result;
    } catch (const std::exception &exception) {
      return std::unexpected(Dmn_BackendError{exception.what()});
    }
  }

  auto changeConnection(const Dmn_DeviceIdentity &identity,
                        const bool wantConnected) -> Dmn_BackendResult {
    std::lock_guard lock{m_mutex};
    if (wantConnected) {
      m_cancel_connect.store(false, std::memory_order_release);
    }

    try {
      const auto deadline =
          std::chrono::steady_clock::now() + kDeviceStateTimeout;
      const auto result =
          wantConnected ? client().connectDevice(identity.m_device_address,
                                                 identity.m_adapter_address,
                                                 kDeviceStateTimeout)
                        : client().disconnectDevice(identity.m_device_address,
                                                    identity.m_adapter_address,
                                                    kDeviceStateTimeout);
      const auto cancelled = [&] {
        return wantConnected &&
               m_cancel_connect.load(std::memory_order_acquire);
      };

      if (cancelled()) {
        return std::unexpected(Dmn_BackendError{"Canceled", true});
      }

      if (!result) {
        return std::unexpected(Dmn_BackendError{result.error().message});
      }

      // BlueZ publishes the new state asynchronously; wait until observed.
      for (;;) {
        if (cancelled()) {
          return std::unexpected(Dmn_BackendError{"Canceled", true});
        }

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
          return std::unexpected(Dmn_BackendError{
              std::string{"device did not reach the requested "} +
              (wantConnected ? "connected" : "disconnected") +
              " state before the deadline"});
        }

        auto current =
            snapshotLocked(std::min(remaining, kDeviceSnapshotTimeout));
        if (!current) {
          return std::unexpected(current.error());
        }

        const auto device =
            std::find_if(current->m_devices.begin(), current->m_devices.end(),
                         [&identity](const auto &entry) {
                           return entry.m_identity.m_adapter_address ==
                                      identity.m_adapter_address &&
                                  lower(entry.m_identity.m_device_address) ==
                                      lower(identity.m_device_address);
                         });
        if (device == current->m_devices.end()) {
          return std::unexpected(Dmn_BackendError{
              "the device disappeared before its connection state was "
              "observed"});
        }

        if (device->m_connected.value_or(false) == wantConnected) {
          return {};
        }

        std::this_thread::sleep_for(kDeviceStatePollInterval);
      }
    } catch (const std::exception &exception) {
      return std::unexpected(Dmn_BackendError{exception.what()});
    }
  }

  const std::chrono::system_clock::time_point m_notification_cutoff;
  std::mutex m_mutex;
  std::mutex m_client_mutex;
  std::unique_ptr<Dmn_BluezClient> m_client;
  std::atomic<bool> m_cancel_connect{false};
  std::mutex m_handler_mutex;
  InventoryChangedHandler m_inventory_changed;
  MonitorEventHandler m_monitor_event;
  std::mutex m_monitor_mutex;
  std::map<std::string, std::unique_ptr<Dmn_AncsBluezMonitor>> m_monitors;
};

} // namespace

auto makeBluezBackend() -> std::unique_ptr<Dmn_EngineBackend> {
  return std::make_unique<Dmn_BluezEngineBackend>();
}

} // namespace dmn::gui
