/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-demo-backend.cpp
 * @brief Deterministic synthetic backend for hardware-free UI runs.
 *
 * Selected by `--demo`. Simulates one adapter and three phones with short
 * operation delays and a scripted stream of notifications, so frontends and
 * tests exercise the full engine workflow without BlueZ or hardware.
 */

#include "app/engine/dmn-engine-backend.hpp"

#include "dmn-proc.hpp"
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace dmn::gui {
namespace {

constexpr std::string_view kAdapterAddress{"00:11:22:33:44:55"};
constexpr std::chrono::milliseconds kOperationDelay{150};

struct DemoPhone {
  std::string m_address;
  std::string m_name;
  bool m_paired{};
  bool m_connected{};
};

struct DemoMessage {
  const char *m_app;
  const char *m_title;
  const char *m_message;
};

constexpr DemoMessage kMessages[] = {
    {"com.apple.MobileSMS", "Alex", "Are we still on for lunch?"},
    {"com.apple.mobilecal", "Calendar", "Design review starts in 10 minutes"},
    {"com.apple.mobilemail", "Weekly digest", "Your summary is ready"},
    {"com.tinyspeck.chatlyio", "#build", "Pipeline finished successfully"},
};

auto lower(std::string text) -> std::string {
  std::transform(text.begin(), text.end(), text.begin(), [](const auto ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  });

  return text;
}

class Dmn_DemoBackend final : public Dmn_EngineBackend {
public:
  explicit Dmn_DemoBackend(const std::chrono::milliseconds interval)
      : m_interval{interval},
        m_phones{{"AA:BB:CC:00:00:01", "Demo iPhone", true, false},
                 {"AA:BB:CC:00:00:02", "Demo iPhone Old", true, false},
                 {"AA:BB:CC:00:00:03", "Unpaired Phone", false, false}} {}

  ~Dmn_DemoBackend() override {
    std::map<std::string, std::unique_ptr<Feed>> feeds;
    {
      std::lock_guard lock{m_mutex};
      feeds.swap(m_feeds);
    }

    for (auto &[address, feed] : feeds) {
      stopFeed(*feed);
    }
  }

  [[nodiscard]] auto name() const -> std::string override { return "demo"; }

  void setHandlers(InventoryChangedHandler inventoryChanged,
                   MonitorEventHandler monitorEvent) override {
    std::lock_guard lock{m_mutex};
    m_inventory_changed = std::move(inventoryChanged);
    m_monitor_event = std::move(monitorEvent);
  }

  auto
  snapshot() -> std::expected<Dmn_BackendInventory, Dmn_BackendError> override {
    std::lock_guard lock{m_mutex};
    Dmn_BackendInventory inventory;
    inventory.m_adapters.push_back({"/org/bluez/hci0",
                                    std::string{kAdapterAddress},
                                    "Demo Adapter", true});
    for (const auto &phone : m_phones) {
      Dmn_DeviceEntry entry;
      entry.m_identity = {std::string{kAdapterAddress}, phone.m_address};
      entry.m_display_name = phone.m_name;
      entry.m_paired = phone.m_paired;
      entry.m_connected = phone.m_connected;
      entry.m_present = true;
      inventory.m_devices.push_back(std::move(entry));
    }

    return inventory;
  }

  auto setScanning(bool, const std::string &) -> Dmn_BackendResult override {
    std::this_thread::sleep_for(kOperationDelay);

    return {};
  }

  auto
  connect(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult override {
    return change(identity, true);
  }

  auto
  disconnect(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult override {
    return change(identity, false);
  }

  void cancelConnect(const Dmn_DeviceIdentity &) override {
    m_cancel_connect.store(true, std::memory_order_release);
  }

  auto
  startMonitor(const Dmn_DeviceIdentity &identity,
               const std::atomic<bool> &cancel,
               const ProgressHandler &progress) -> Dmn_BackendResult override {
    if (progress) {
      progress("Subscribing to ANCS");
    }

    std::this_thread::sleep_for(kOperationDelay);
    if (cancel.load(std::memory_order_acquire)) {
      return std::unexpected(Dmn_BackendError{"Canceled", true});
    }

    auto feed{std::make_unique<Feed>()};
    auto *raw = feed.get();
    {
      std::lock_guard lock{m_mutex};
      const auto *phone = find(identity.m_device_address);
      if (phone == nullptr || !phone->m_connected) {
        return std::unexpected(Dmn_BackendError{"device is not connected"});
      }

      m_feeds[lower(identity.m_device_address)] = std::move(feed);
    }

    emit(identity, state("ready"));
    raw->m_started = true;
    raw->m_thread.exec([this, identity, raw] { feedLoop(identity, *raw); });

    return {};
  }

  auto stopMonitor(const Dmn_DeviceIdentity &identity)
      -> Dmn_BackendResult override {
    std::unique_ptr<Feed> feed;
    {
      std::lock_guard lock{m_mutex};
      const auto found{m_feeds.find(lower(identity.m_device_address))};
      if (found == m_feeds.end()) {
        return {};
      }

      feed = std::move(found->second);
      m_feeds.erase(found);
    }

    stopFeed(*feed);

    return {};
  }

private:
  // The feed thread ends through m_stop and is wait()ed, never cancelled.
  struct Feed {
    Feed() : m_thread{"dmn-demo-feed"} {}

    Dmn_Proc m_thread;
    bool m_started{};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop{};
  };

  static auto state(std::string text) -> Dmn_BackendMonitorEvent {
    Dmn_BackendMonitorEvent event;
    event.m_kind = Dmn_BackendMonitorEvent::Kind::kState;
    event.m_state = std::move(text);

    return event;
  }

  static void stopFeed(Feed &feed) {
    {
      std::lock_guard lock{feed.m_mutex};
      feed.m_stop = true;
    }

    feed.m_cv.notify_all();
    if (feed.m_started) {
      feed.m_thread.wait();
      feed.m_started = false;
    }
  }

  auto find(const std::string &address) -> DemoPhone * {
    for (auto &phone : m_phones) {
      if (lower(phone.m_address) == lower(address)) {
        return &phone;
      }
    }

    return nullptr;
  }

  void emit(const Dmn_DeviceIdentity &identity,
            const Dmn_BackendMonitorEvent &event) {
    MonitorEventHandler handler;
    {
      std::lock_guard lock{m_mutex};
      handler = m_monitor_event;
    }

    if (handler) {
      handler(identity, event);
    }
  }

  void feedLoop(const Dmn_DeviceIdentity &identity, Feed &feed) {
    std::uint32_t uid{1};
    for (;;) {
      Dmn_BackendMonitorEvent event;
      event.m_kind = Dmn_BackendMonitorEvent::Kind::kNotification;
      const auto &message = kMessages[(uid - 1) % std::size(kMessages)];
      Dmn_BackendNotification notification;
      notification.m_kind = "added";
      notification.m_uid = uid;
      notification.m_category = 1;
      notification.m_app_identifier = message.m_app;
      notification.m_title = message.m_title;
      notification.m_message = message.m_message;
      event.m_notification = std::move(notification);
      emit(identity, event);
      ++uid;

      std::unique_lock lock{feed.m_mutex};
      if (feed.m_cv.wait_for(lock, m_interval,
                             [&feed] { return feed.m_stop; })) {
        return;
      }
    }
  }

  auto change(const Dmn_DeviceIdentity &identity,
              const bool connected) -> Dmn_BackendResult {
    if (connected) {
      m_cancel_connect.store(false, std::memory_order_release);
    }

    std::this_thread::sleep_for(kOperationDelay);
    if (connected && m_cancel_connect.load(std::memory_order_acquire)) {
      return std::unexpected(Dmn_BackendError{"Canceled", true});
    }

    InventoryChangedHandler handler;
    {
      std::lock_guard lock{m_mutex};
      auto *phone = find(identity.m_device_address);
      if (phone == nullptr) {
        return std::unexpected(Dmn_BackendError{"unknown demo device"});
      }

      if (connected && !phone->m_paired) {
        return std::unexpected(Dmn_BackendError{"device is not paired"});
      }

      phone->m_connected = connected;
      handler = m_inventory_changed;
    }

    if (handler) {
      handler();
    }

    return {};
  }

  const std::chrono::milliseconds m_interval;
  std::mutex m_mutex;
  std::vector<DemoPhone> m_phones;
  std::map<std::string, std::unique_ptr<Feed>> m_feeds;
  std::atomic<bool> m_cancel_connect{false};
  InventoryChangedHandler m_inventory_changed;
  MonitorEventHandler m_monitor_event;
};

} // namespace

auto makeDemoBackend(const std::chrono::milliseconds notificationInterval)
    -> std::unique_ptr<Dmn_EngineBackend> {
  return std::make_unique<Dmn_DemoBackend>(notificationInterval);
}

} // namespace dmn::gui
