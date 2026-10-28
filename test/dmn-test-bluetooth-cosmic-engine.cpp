/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluetooth-cosmic-engine.cpp
 * @brief Tests for the JSON protocol, engine and C ABI used by the COSMIC app.
 */

#include "app/engine/dmn-engine-backend.hpp"
#include "app/engine/dmn-engine.hpp"
#include "app/engine/dmn-json.hpp"
#include "app/gui/cabi/dmn-bluetooth-cabi.h"

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace {

using dmn::gui::Dmn_BackendError;
using dmn::gui::Dmn_BackendInventory;
using dmn::gui::Dmn_BackendResult;
using dmn::gui::Dmn_DeviceEntry;
using dmn::gui::Dmn_DeviceIdentity;
using dmn::gui::Dmn_Engine;
using dmn::gui::Dmn_EngineBackend;
using dmn::gui::Dmn_JsonValue;

constexpr auto kWait = std::chrono::seconds{10};
constexpr auto kDevice = "AA:BB:CC:00:00:01";
constexpr auto kAdapter = "00:11:22:33:44:55";

class Pump {
public:
  explicit Pump(Dmn_Engine &engine) : m_engine{engine} {}

  void send(const std::string &json) { ASSERT_TRUE(m_engine.send(json)); }

  auto waitEvent() -> bool {
    return waitMessage([this](const Dmn_JsonValue &message) {
      return message.getString("type") == "event" && !m_events.empty();
    });
  }

  // Reads messages until a `state` satisfies the predicate; records the rest.
  auto waitState(const std::function<bool(const Dmn_JsonValue &)> &predicate)
      -> bool {
    return waitMessage([&predicate](const Dmn_JsonValue &message) {
      return message.getString("type") == "state" && predicate(message);
    });
  }

  auto waitMessage(const std::function<bool(const Dmn_JsonValue &)> &predicate)
      -> bool {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
      auto text = m_engine.nextMessage(std::chrono::milliseconds{100});
      if (!text) {
        continue;
      }

      auto message = Dmn_JsonValue::parse(*text);
      if (!message) {
        ADD_FAILURE() << "engine produced invalid JSON: " << *text;

        return false;
      }

      const auto type = message->getString("type");
      if (type == "event") {
        m_events.push_back(*message);
      } else if (type == "persist") {
        m_persists.push_back(*message);
      } else if (type == "state") {
        m_last_state = *message;
      }

      if (predicate(*message)) {
        return true;
      }
    }

    return false;
  }

  std::vector<Dmn_JsonValue> m_events;
  std::vector<Dmn_JsonValue> m_persists;
  Dmn_JsonValue m_last_state;

private:
  Dmn_Engine &m_engine;
};

auto device(const Dmn_JsonValue &state,
            const std::string &address) -> const Dmn_JsonValue * {
  const auto *devices = state.find("devices");
  if (devices == nullptr) {
    return nullptr;
  }

  for (const auto &entry : devices->items()) {
    if (entry.getString("device") == address) {
      return &entry;
    }
  }

  return nullptr;
}

auto command(const std::string &type, const std::string &adapter = {},
             const std::string &address = {}) -> std::string {
  auto json = Dmn_JsonValue::object();
  json.set("type", type);
  if (!adapter.empty()) {
    json.set("adapter", adapter);
    json.set("device", address);
  }

  return json.dump();
}

auto selectCommand(const std::string &address,
                   const bool selected) -> std::string {
  auto json = Dmn_JsonValue::parse(command("select", kAdapter, address));
  json->set("selected", selected);

  return json->dump();
}

auto fastDemoEngine() -> std::unique_ptr<Dmn_Engine> {
  dmn::gui::Dmn_EngineOptions options;
  options.m_tick_interval = std::chrono::milliseconds{20};

  return std::make_unique<Dmn_Engine>(
      dmn::gui::makeDemoBackend(std::chrono::milliseconds{50}), options);
}

class FailingMonitorBackend final : public Dmn_EngineBackend {
public:
  [[nodiscard]] auto name() const -> std::string override { return "fake"; }

  void setHandlers(InventoryChangedHandler, MonitorEventHandler) override {}

  auto
  snapshot() -> std::expected<Dmn_BackendInventory, Dmn_BackendError> override {
    Dmn_BackendInventory inventory;
    inventory.m_adapters.push_back({"/hci0", kAdapter, "Adapter", true});
    Dmn_DeviceEntry entry;
    entry.m_identity = {kAdapter, kDevice};
    entry.m_display_name = "Phone";
    entry.m_paired = true;
    entry.m_connected = true;
    entry.m_present = true;
    inventory.m_devices.push_back(entry);

    return inventory;
  }

  auto setScanning(bool, const std::string &) -> Dmn_BackendResult override {
    return {};
  }

  auto connect(const Dmn_DeviceIdentity &) -> Dmn_BackendResult override {
    return {};
  }

  auto disconnect(const Dmn_DeviceIdentity &) -> Dmn_BackendResult override {
    return {};
  }

  void cancelConnect(const Dmn_DeviceIdentity &) override {}

  auto startMonitor(const Dmn_DeviceIdentity &, const std::atomic<bool> &,
                    const ProgressHandler &) -> Dmn_BackendResult override {
    return std::unexpected(Dmn_BackendError{"ANCS service not found"});
  }

  auto stopMonitor(const Dmn_DeviceIdentity &) -> Dmn_BackendResult override {
    return {};
  }
};

// Phone that starts out of range and later reconnects (BlueZ reports
// Connected again), mirroring a user arriving back home.
class ReconnectingBackend final : public Dmn_EngineBackend {
public:
  [[nodiscard]] auto name() const -> std::string override { return "fake"; }

  void setHandlers(InventoryChangedHandler, MonitorEventHandler) override {}

  auto
  snapshot() -> std::expected<Dmn_BackendInventory, Dmn_BackendError> override {
    Dmn_BackendInventory inventory;
    inventory.m_adapters.push_back({"/hci0", kAdapter, "Adapter", true});
    Dmn_DeviceEntry entry;
    entry.m_identity = {kAdapter, kDevice};
    entry.m_display_name = "Phone";
    entry.m_paired = true;
    entry.m_connected = m_in_range.load();
    entry.m_present = true;
    inventory.m_devices.push_back(entry);

    return inventory;
  }

  auto setScanning(bool, const std::string &) -> Dmn_BackendResult override {
    return {};
  }

  auto connect(const Dmn_DeviceIdentity &) -> Dmn_BackendResult override {
    return {};
  }

  auto disconnect(const Dmn_DeviceIdentity &) -> Dmn_BackendResult override {
    return {};
  }

  void cancelConnect(const Dmn_DeviceIdentity &) override {}

  auto startMonitor(const Dmn_DeviceIdentity &, const std::atomic<bool> &,
                    const ProgressHandler &) -> Dmn_BackendResult override {
    m_started.store(true);

    return {};
  }

  auto stopMonitor(const Dmn_DeviceIdentity &) -> Dmn_BackendResult override {
    return {};
  }

  std::atomic<bool> m_in_range{false};
  std::atomic<bool> m_started{false};
};

TEST(CosmicJsonTest, RoundTripsEscapesAndSurrogatePairs) {
  const auto parsed = Dmn_JsonValue::parse(
      R"({"a":"line\nquote\" \u00e9 \ud83d\ude00","b":[1,2.5,true,null]})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->getString("a"), "line\nquote\" \xC3\xA9 \xF0\x9F\x98\x80");
  ASSERT_EQ(parsed->find("b")->items().size(), 4U);

  const auto again = Dmn_JsonValue::parse(parsed->dump());
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(again->getString("a"), parsed->getString("a"));
}

TEST(CosmicJsonTest, RejectsMalformedInput) {
  EXPECT_FALSE(Dmn_JsonValue::parse("{").has_value());
  EXPECT_FALSE(Dmn_JsonValue::parse(R"({"a":1} trailing)").has_value());
  EXPECT_FALSE(Dmn_JsonValue::parse(R"("\ud800")").has_value());
  EXPECT_FALSE(Dmn_JsonValue::parse("\"bad \xFF byte\"").has_value());
  EXPECT_FALSE(Dmn_JsonValue::parse(std::string(100, '[')).has_value());
}

TEST(CosmicJsonTest, DumpReplacesInvalidUtf8AndEscapesControls) {
  const auto dumped = Dmn_JsonValue{std::string{"a\xFF\x01z"}}.dump();
  EXPECT_EQ(dumped, "\"a\xEF\xBF\xBD\\u0001z\"");
  EXPECT_TRUE(Dmn_JsonValue::parse(dumped).has_value());
}

TEST(CosmicEngineTest, RejectsMalformedCommands) {
  auto engine = fastDemoEngine();
  EXPECT_FALSE(engine->send("not json"));
  EXPECT_FALSE(engine->send("[]"));
  EXPECT_FALSE(engine->send(R"({"no_type":1})"));
}

TEST(CosmicEngineTest, AnnouncesReadyThenPublishesInventory) {
  auto engine = fastDemoEngine();
  const auto ready =
      Dmn_JsonValue::parse(engine->nextMessage(kWait).value_or("null"));
  ASSERT_TRUE(ready.has_value());
  EXPECT_EQ(ready->getString("type"), "ready");
  EXPECT_EQ(ready->getNumber("protocol", 0), 1);
  EXPECT_EQ(ready->getString("backend"), "demo");

  Pump pump{*engine};
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return state.find("devices")->items().size() == 3U;
  }));
}

TEST(CosmicEngineTest, ConnectRequiresSavedPairedDevice) {
  auto engine = fastDemoEngine();
  Pump pump{*engine};
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return state.find("devices")->items().size() == 3U;
  }));

  pump.send(command("connect", kAdapter, kDevice));
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return state.getString("status").find("Save and pair") != std::string::npos;
  }));
  EXPECT_FALSE(device(pump.m_last_state, kDevice)->getBool("connected", true));
}

TEST(CosmicEngineTest, FullDemoWorkflowPersistsIntentAndReceivesEvents) {
  auto engine = fastDemoEngine();
  Pump pump{*engine};
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return state.find("devices")->items().size() == 3U;
  }));

  pump.send(selectCommand(kDevice, true));
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return state.getBool("unsaved_changes", false);
  }));

  pump.send(command("save_selection"));
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return !state.getBool("unsaved_changes", true) &&
           device(state, kDevice)->getBool("saved", false);
  }));
  ASSERT_FALSE(pump.m_persists.empty());
  EXPECT_EQ(pump.m_persists.back().find("saved")->items().size(), 1U);

  pump.send(command("connect", kAdapter, kDevice));
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return device(state, kDevice)->getBool("connected", false) &&
           !state.getBool("busy", true);
  }));
  EXPECT_TRUE(device(pump.m_last_state, kDevice)
                  ->getBool("can_start_monitoring", false));

  auto monitor = Dmn_JsonValue::parse(command("monitor", kAdapter, kDevice));
  monitor->set("enabled", true);
  pump.send(monitor->dump());
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    return device(state, kDevice)->getString("monitor_state") == "active";
  }));
  while (pump.m_events.empty() ||
         pump.m_events.back().getString("kind") != "notification") {
    ASSERT_TRUE(pump.waitEvent());
  }

  const auto &event = pump.m_events.back();
  EXPECT_EQ(event.getString("device"), kDevice);
  EXPECT_FALSE(event.find("notification")->getString("title").empty());

  pump.send(command("disconnect", kAdapter, kDevice));
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    const auto *entry = device(state, kDevice);

    return !entry->getBool("connected", true) &&
           !entry->getBool("monitoring_requested", true) &&
           entry->getString("monitor_state") == "idle";
  }));
  EXPECT_TRUE(pump.m_persists.back().find("monitoring")->items().empty());

  pump.send(command("shutdown"));
  bool bye = false;
  for (int i = 0; i < 200 && !bye; ++i) {
    const auto text = engine->nextMessage(std::chrono::milliseconds{50});
    bye = text && text->find(R"("type":"bye")") != std::string::npos;
  }

  EXPECT_TRUE(bye);
  EXPECT_FALSE(engine->send(command("refresh")));
}

TEST(CosmicEngineTest, ConfigureRestoresIntentAndFailedStartIsReported) {
  dmn::gui::Dmn_EngineOptions options;
  options.m_tick_interval = std::chrono::milliseconds{20};
  Dmn_Engine engine{std::make_unique<FailingMonitorBackend>(), options};
  Pump pump{engine};

  auto configure = Dmn_JsonValue::object();
  configure.set("type", "configure");
  auto identity = Dmn_JsonValue::object();
  identity.set("adapter", kAdapter);
  identity.set("device", kDevice);
  configure.set("saved", Dmn_JsonValue::array().push(identity));
  configure.set("monitoring", Dmn_JsonValue::array().push(identity));
  pump.send(configure.dump());

  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    const auto *entry = device(state, kDevice);

    return entry != nullptr &&
           entry->getString("monitor_detail").find("ANCS service not found") !=
               std::string::npos;
  }));
  EXPECT_TRUE(device(pump.m_last_state, kDevice)
                  ->getBool("monitoring_requested", false));
  ASSERT_FALSE(pump.m_events.empty());
  EXPECT_EQ(pump.m_events.front().getString("kind"), "monitor_error");
}

TEST(CosmicEngineTest, ResumesMonitoringWhenSavedDeviceReconnects) {
  dmn::gui::Dmn_EngineOptions options;
  options.m_tick_interval = std::chrono::milliseconds{20};
  options.m_inventory_refresh_interval = std::chrono::milliseconds{50};
  auto backend = std::make_unique<ReconnectingBackend>();
  auto *raw = backend.get();
  Dmn_Engine engine{std::move(backend), options};
  Pump pump{engine};

  auto configure = Dmn_JsonValue::object();
  configure.set("type", "configure");
  auto identity = Dmn_JsonValue::object();
  identity.set("adapter", kAdapter);
  identity.set("device", kDevice);
  configure.set("saved", Dmn_JsonValue::array().push(identity));
  configure.set("monitoring", Dmn_JsonValue::array().push(identity));
  pump.send(configure.dump());

  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    const auto *entry = device(state, kDevice);

    return entry != nullptr && !entry->getBool("connected", true);
  }));
  EXPECT_FALSE(raw->m_started.load());

  raw->m_in_range.store(true);
  ASSERT_TRUE(pump.waitState([](const Dmn_JsonValue &state) {
    const auto *entry = device(state, kDevice);

    return entry != nullptr && entry->getString("monitor_state") == "active";
  }));
  EXPECT_TRUE(raw->m_started.load());
}

TEST(CosmicCabiTest, DemoEngineLifecycleAndNullSafety) {
  EXPECT_STREQ(dmn_bt_version(), "1");
  EXPECT_NE(dmn_bt_engine_send(nullptr, "{}"), 0);
  EXPECT_EQ(dmn_bt_engine_next_message(nullptr, 1), nullptr);
  dmn_bt_engine_destroy(nullptr);
  dmn_bt_string_free(nullptr);

  auto *engine = dmn_bt_engine_create_demo();
  ASSERT_NE(engine, nullptr);
  EXPECT_NE(dmn_bt_engine_send(engine, "garbage"), 0);
  EXPECT_NE(dmn_bt_engine_send(engine, nullptr), 0);

  char *first = dmn_bt_engine_next_message(engine, 5000);
  ASSERT_NE(first, nullptr);
  EXPECT_NE(std::string{first}.find(R"("type":"ready")"), std::string::npos);
  dmn_bt_string_free(first);

  // Destroying with work in flight must stop and join everything.
  EXPECT_EQ(dmn_bt_engine_send(engine, R"({"type":"scan","enabled":true})"), 0);
  dmn_bt_engine_destroy(engine);
}

} // namespace
