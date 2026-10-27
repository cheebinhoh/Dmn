/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file main.cpp
 * @brief GTK 4 / libadwaita frontend over the shared Dmn Bluetooth GUI engine.
 *
 * Overview
 * --------
 * All Bluetooth behavior (inventory, selection, connect/disconnect, ANCS
 * monitoring intent and retry) lives in dmn::gui::Dmn_Engine, the same engine
 * the COSMIC frontend uses. This file only renders the engine's JSON `state`
 * and `event` messages and forwards user intent as JSON commands.
 *
 * Threading
 * ---------
 * The engine is pull-based. A reader thread blocks on nextMessage() and hands
 * each message to the GTK main context, so widgets and AppState are touched
 * only on the main thread. Commands are sent from the main thread; send()
 * only enqueues.
 *
 * Demo mode
 * ---------
 * `--demo` runs the engine over its synthetic backend: no BlueZ, adapter or
 * phone is needed, and the user's saved devices are neither read nor written.
 */

#define G_LOG_DOMAIN "dmn-bluetooth-gtk"

#include "app/gui/core/desktop-alert-policy.hpp"
#include "app/gui/engine/dmn-engine.hpp"
#include "app/gui/engine/dmn-json.hpp"

#include <adwaita.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using dmn::gui::Dmn_JsonValue;

constexpr auto kApplicationId = DMN_BLUETOOTH_GTK_APP_ID;
constexpr auto kSettingsSchemaId = "com.cheebinhoh.DmnBluetooth";
constexpr auto kPollInterval = std::chrono::milliseconds{200};
constexpr auto kScanRetryInterval = std::chrono::seconds{10};
constexpr std::size_t kFeedLimit = 1000;
constexpr std::size_t kFeedVisibleLimit = 200;

enum class KindFilter { kAll, kNotifications, kProblems };

struct Device {
  std::string m_adapter;
  std::string m_address;
  std::string m_name;
  std::string m_status;
  std::string m_monitor_state;
  std::string m_monitor_detail;
  bool m_saved{};
  bool m_selected{};
  bool m_can_connect{};
  bool m_can_disconnect{};
  bool m_can_start_monitoring{};
  bool m_can_stop_monitoring{};

  [[nodiscard]] auto key() const -> std::string {
    return m_adapter + "/" + m_address;
  }

  [[nodiscard]] auto displayName() const -> std::string {
    return m_name.empty() ? m_address : m_name;
  }
};

struct Event {
  std::int64_t m_received_ms{};
  std::string m_adapter;
  std::string m_address;
  std::string m_device_name;
  std::string m_headline;
  std::string m_summary;
  std::string m_search_text;
  std::string m_details;
  int m_category{-1};
  bool m_is_notification{};
  bool m_is_problem{};

  [[nodiscard]] auto key() const -> std::string {
    return m_adapter + "/" + m_address;
  }
};

struct AppState {
  explicit AppState(GtkApplication *app) : m_app{app} {}

  GtkApplication *m_app;
  GSettings *m_settings{};
  bool m_demo{};

  std::unique_ptr<dmn::gui::Dmn_Engine> m_engine;
  std::thread m_reader;
  std::atomic<bool> m_stop_reader{false};

  bool m_ready{};
  bool m_adapters_known{};
  bool m_scanning{};
  bool m_busy{};
  bool m_unsaved{};
  std::string m_status;
  std::string m_notice;
  std::string m_sort_column{"name"};
  bool m_sort_ascending{true};
  std::chrono::steady_clock::time_point m_last_scan_request{};
  bool m_scan_requested{};

  std::vector<Device> m_devices;
  std::string m_devices_signature;
  std::string m_device_search;
  bool m_selected_only{};

  std::deque<Event> m_events;
  KindFilter m_kind_filter{KindFilter::kAll};
  std::string m_feed_device_filter;
  std::string m_feed_search;

  bool m_desktop_alerts_enabled{true};
  std::unordered_map<std::string, std::size_t> m_alert_counts;

  GtkWidget *m_window{};
  GtkWidget *m_devices_pane{};
  bool m_sidebar_open{true};
  GtkWidget *m_device_box{};
  GtkWidget *m_feed_box{};
  GtkWidget *m_feed_search_entry{};
  GtkWidget *m_status_label{};
  GtkWidget *m_save_button{};
  GtkWidget *m_discard_button{};
  GtkWidget *m_name_sort_button{};
  GtkWidget *m_status_sort_button{};
  GtkWidget *m_device_filter_button{};
  GtkWidget *m_category_filter{};
  int m_category_index{};
  AdwDialog *m_about_dialog{};
};

auto stateFor(gpointer object) -> AppState * {
  return static_cast<AppState *>(
      g_object_get_data(G_OBJECT(object), "dmn-bluetooth-state"));
}

// Runs std::function from a GLib signal; the closure owns the function.
struct Closure {
  std::function<void()> m_fn;
};

void connectVoid(gpointer instance, const char *signal,
                 std::function<void()> fn) {
  auto *closure = new Closure{std::move(fn)};
  g_signal_connect_data(
      instance, signal, G_CALLBACK(+[](gpointer, gpointer data) {
        static_cast<Closure *>(data)->m_fn();
      }),
      closure,
      [](gpointer data, GClosure *) { delete static_cast<Closure *>(data); },
      static_cast<GConnectFlags>(0));
}

void clearChildren(GtkWidget *box) {
  while (auto *child = gtk_widget_get_first_child(box)) {
    gtk_box_remove(GTK_BOX(box), child);
  }
}

auto makeLabel(const std::string &text, const char *cssClass = nullptr,
               const bool selectable = false) -> GtkWidget * {
  auto *label = gtk_label_new(text.c_str());
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_label_set_selectable(GTK_LABEL(label), selectable);
  if (cssClass != nullptr) {
    gtk_widget_add_css_class(label, cssClass);
  }

  return label;
}

const char *const kCategoryNames[] = {"Other",
                                      "Incoming call",
                                      "Missed call",
                                      "Voicemail",
                                      "Social",
                                      "Schedule",
                                      "Email",
                                      "News",
                                      "Health and fitness",
                                      "Business and finance",
                                      "Location",
                                      "Entertainment"};
constexpr int kCategoryCount = 12;

auto categoryName(const int category) -> std::string {
  return category >= 0 && category < kCategoryCount ? kCategoryNames[category]
                                                    : "Unknown category";
}

auto lower(std::string text) -> std::string {
  auto *folded = g_utf8_strdown(text.c_str(), -1);
  std::string result{folded};
  g_free(folded);

  return result;
}

//------------------------------------------------------------------------
// Engine I/O
//------------------------------------------------------------------------

auto command(const char *type) -> Dmn_JsonValue {
  auto json = Dmn_JsonValue::object();
  json.set("type", type);

  return json;
}

auto deviceCommand(const char *type, const Device &device) -> Dmn_JsonValue {
  auto json = command(type);
  json.set("adapter", device.m_adapter);
  json.set("device", device.m_address);

  return json;
}

void sendCommand(AppState *state, const Dmn_JsonValue &json) {
  if (state->m_engine == nullptr || !state->m_engine->send(json.dump())) {
    g_warning("The Bluetooth engine rejected a command");
  }
}

void setStatusText(AppState *state) {
  const auto &text =
      state->m_notice.empty() ? state->m_status : state->m_notice;
  gtk_label_set_text(GTK_LABEL(state->m_status_label), text.c_str());
}

//------------------------------------------------------------------------
// Persistence (never used in demo mode)
//------------------------------------------------------------------------

auto openSettings() -> GSettings * {
  // Prefer the installed schema, then the build-tree copy for development.
  const char *schemaDirectories[] = {DMN_BLUETOOTH_GTK_INSTALL_SCHEMA_DIR,
                                     DMN_BLUETOOTH_GTK_SCHEMA_DIR};
  GSettingsSchema *schema = nullptr;
  for (const auto *directory : schemaDirectories) {
    GError *error = nullptr;
    auto *source = g_settings_schema_source_new_from_directory(
        directory, g_settings_schema_source_get_default(), FALSE, &error);
    g_clear_error(&error);
    if (source == nullptr) {
      continue;
    }

    schema = g_settings_schema_source_lookup(source, kSettingsSchemaId, TRUE);
    g_settings_schema_source_unref(source);
    if (schema != nullptr) {
      break;
    }
  }

  if (schema == nullptr) {
    auto *source = g_settings_schema_source_get_default();
    if (source != nullptr) {
      schema = g_settings_schema_source_lookup(source, kSettingsSchemaId, TRUE);
    }
  }

  if (schema == nullptr) {
    g_warning("GSettings schema '%s' was not found", kSettingsSchemaId);

    return nullptr;
  }

  auto *settings = g_settings_new_full(schema, nullptr, nullptr);
  g_settings_schema_unref(schema);

  return settings;
}

auto readIdentities(GSettings *settings, const char *key) -> Dmn_JsonValue {
  auto list = Dmn_JsonValue::array();
  auto *value = g_settings_get_value(settings, key);
  GVariantIter iterator;
  const char *adapter = nullptr;
  const char *device = nullptr;
  g_variant_iter_init(&iterator, value);
  while (g_variant_iter_next(&iterator, "(&s&s)", &adapter, &device)) {
    auto identity = Dmn_JsonValue::object();
    identity.set("adapter", adapter);
    identity.set("device", device);
    list.push(std::move(identity));
  }

  g_variant_unref(value);

  return list;
}

void writeIdentities(GSettings *settings, const char *key,
                     const Dmn_JsonValue *list) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ss)"));
  if (list != nullptr) {
    for (const auto &item : list->items()) {
      g_variant_builder_add(&builder, "(ss)", item.getString("adapter").c_str(),
                            item.getString("device").c_str());
    }
  }

  if (!g_settings_set_value(settings, key, g_variant_builder_end(&builder))) {
    g_warning("Could not persist '%s'", key);
  }
}

//------------------------------------------------------------------------
// Desktop alerts: one generic, privacy-safe alert per device
//------------------------------------------------------------------------

auto alertIdentity(const std::string &key) -> dmn::gui::Dmn_DeviceIdentity {
  const auto slash = key.find('/');

  return {key.substr(0, slash), key.substr(slash + 1)};
}

void withdrawAlert(AppState *state, const std::string &key) {
  if (state->m_alert_counts.erase(key) != 0) {
    const auto alert = dmn::gui::makeDesktopAlert(alertIdentity(key), 1);
    g_application_withdraw_notification(G_APPLICATION(state->m_app),
                                        alert.m_id.c_str());
  }
}

void withdrawAllAlerts(AppState *state) {
  for (const auto &[key, count] : state->m_alert_counts) {
    (void)count;
    const auto alert = dmn::gui::makeDesktopAlert(alertIdentity(key), 1);
    g_application_withdraw_notification(G_APPLICATION(state->m_app),
                                        alert.m_id.c_str());
  }

  state->m_alert_counts.clear();
}

void sendAlert(AppState *state, const Event &event) {
  const auto key = event.key();
  const auto count = ++state->m_alert_counts[key];
  const auto alert = dmn::gui::makeDesktopAlert(alertIdentity(key), count);
  auto *notification = g_notification_new(alert.m_title.c_str());
  g_notification_set_body(notification, alert.m_body.c_str());
  g_notification_set_default_action(notification, "app.open");
  g_application_send_notification(G_APPLICATION(state->m_app),
                                  alert.m_id.c_str(), notification);
  g_object_unref(notification);
}

//------------------------------------------------------------------------
// Event feed
//------------------------------------------------------------------------

auto formatTime(const std::int64_t milliseconds,
                const char *format = "%H:%M:%S") -> std::string {
  auto *time = g_date_time_new_from_unix_local(milliseconds / 1000);
  if (time == nullptr) {
    return {};
  }

  auto *text = g_date_time_format(time, format);
  std::string result{text != nullptr ? text : ""};
  g_free(text);
  g_date_time_unref(time);

  return result;
}

auto makeEvent(const Dmn_JsonValue &json) -> Event {
  Event event;
  event.m_received_ms =
      static_cast<std::int64_t>(json.getNumber("received_ms", 0));
  event.m_adapter = json.getString("adapter");
  event.m_address = json.getString("device");
  event.m_device_name = json.getString("name");
  const auto kind = json.getString("kind");
  event.m_is_problem = kind == "error" || kind == "monitor_error";

  const auto *notification = json.find("notification");
  if (notification != nullptr && notification->isObject()) {
    event.m_is_notification = true;
    const auto notificationKind = notification->getString("kind");
    const auto title = notification->getString("title");
    const auto app = notification->getString("app_identifier");
    event.m_headline = !title.empty() ? title
                       : !app.empty() ? app
                                      : "Notification";
    std::string text;
    for (const auto *key : {"subtitle", "message"}) {
      const auto part = notification->getString(key);
      if (!part.empty()) {
        text += text.empty() ? part : " — " + part;
      }
    }

    if (text.empty() && notificationKind == "removed") {
      text = "Dismissed on the phone";
    } else if (text.empty() && notificationKind == "attributes_unavailable") {
      text = "Details were not available";
    }

    event.m_summary = text;
    event.m_search_text = app;
    event.m_category = static_cast<int>(notification->getNumber("category", 0));
    const auto line = [](const char *name, const std::string &value) {
      return std::string{name} + ": " +
             (value.empty() ? "Unavailable" : value) + "\n";
    };
    const auto number = [&](const char *key) {
      return std::to_string(
          static_cast<std::uint64_t>(notification->getNumber(key, 0)));
    };
    const char *eventName = notificationKind == "added"     ? "Added"
                            : notificationKind == "updated" ? "Updated"
                            : notificationKind == "removed"
                                ? "Removed"
                                : "Attributes unavailable";
    event.m_details =
        "Device: " + event.m_device_name + "\nAdapter: " + event.m_adapter +
        "\nDevice address: " + event.m_address + "\nReceived: " +
        formatTime(event.m_received_ms, "%Y-%m-%d %H:%M:%S %Z") +
        "\nEvent: " + eventName + "\nNotification UID: " + number("uid") +
        "\nCategory: " + categoryName(event.m_category) + " (" +
        number("category") + ")\n";
    if (notificationKind == "removed") {
      event.m_details += "Notification was removed; ANCS provides no current "
                         "attributes for this event.\n";
    } else {
      event.m_details +=
          line("App identifier", app) + line("Title", title) +
          line("Subtitle", notification->getString("subtitle")) +
          line("Message", notification->getString("message")) +
          line("Phone-provided date", notification->getString("date")) +
          "Category count: " + number("category_count") +
          "\nFlags: " + number("flags") + "\n";
    }
  } else if (kind == "state") {
    event.m_headline = "Monitor " + json.getString("state", "changed");
  } else if (kind == "monitor_error") {
    event.m_headline = "Monitor failed to start";
    event.m_summary = json.getString("error");
  } else {
    event.m_headline = "Monitor error";
    event.m_summary = json.getString("error");
  }

  event.m_search_text =
      lower(event.m_device_name + " " + event.m_headline + " " +
            event.m_summary + " " + event.m_search_text);

  return event;
}

auto eventVisible(const AppState *state, const Event &event) -> bool {
  if (!state->m_feed_device_filter.empty() &&
      event.key() != state->m_feed_device_filter) {
    return false;
  }

  if ((state->m_kind_filter == KindFilter::kNotifications &&
       !event.m_is_notification) ||
      (state->m_kind_filter == KindFilter::kProblems && !event.m_is_problem)) {
    return false;
  }

  if (state->m_category_index > 0 &&
      event.m_category != state->m_category_index - 1) {
    return false;
  }

  const auto needle = lower(state->m_feed_search);

  return needle.empty() ||
         event.m_search_text.find(needle) != std::string::npos;
}

void rebuildFeed(AppState *state) {
  clearChildren(state->m_feed_box);
  std::size_t shown = 0;
  for (auto it = state->m_events.rbegin();
       it != state->m_events.rend() && shown < kFeedVisibleLimit; ++it) {
    if (!eventVisible(state, *it)) {
      continue;
    }

    ++shown;
    auto *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(row, "card");
    gtk_widget_set_margin_bottom(row, 2);
    auto *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_top(inner, 8);
    gtk_widget_set_margin_bottom(inner, 8);
    gtk_widget_set_margin_start(inner, 10);
    gtk_widget_set_margin_end(inner, 10);
    gtk_box_append(GTK_BOX(inner), makeLabel(it->m_headline, "heading", true));
    if (!it->m_summary.empty()) {
      gtk_box_append(GTK_BOX(inner), makeLabel(it->m_summary, nullptr, true));
    }

    const auto caption =
        it->m_device_name + " · " + formatTime(it->m_received_ms);
    gtk_box_append(GTK_BOX(inner), makeLabel(caption, "caption"));
    gtk_box_append(GTK_BOX(row), inner);
    if (it->m_is_notification) {
      auto *button = gtk_button_new_with_label("Details");
      gtk_widget_add_css_class(button, "flat");
      gtk_widget_set_halign(button, GTK_ALIGN_START);
      connectVoid(button, "clicked", [state, details = it->m_details] {
        auto *dialog =
            adw_alert_dialog_new("Notification details", details.c_str());
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close",
                                      "Close");
        adw_dialog_present(dialog, state->m_window);
      });
      gtk_box_append(GTK_BOX(inner), button);
    }

    gtk_box_append(GTK_BOX(state->m_feed_box), row);
  }

  if (shown == 0) {
    gtk_box_append(GTK_BOX(state->m_feed_box),
                   makeLabel("Nothing to show.", "dim-label"));
  }

  gtk_widget_set_sensitive(state->m_device_filter_button,
                           !state->m_feed_device_filter.empty());
}

//------------------------------------------------------------------------
// Device list
//------------------------------------------------------------------------

auto deviceMatches(const AppState *state, const Device &device) -> bool {
  if (state->m_selected_only && !device.m_selected) {
    return false;
  }

  const auto needle = lower(state->m_device_search);
  if (needle.empty()) {
    return true;
  }

  for (const auto *part :
       {&device.m_name, &device.m_address, &device.m_adapter, &device.m_status,
        &device.m_monitor_state, &device.m_monitor_detail}) {
    if (lower(*part).find(needle) != std::string::npos) {
      return true;
    }
  }

  return false;
}

auto makeButton(const char *label, const bool enabled, const bool suggested,
                std::function<void()> action) -> GtkWidget * {
  auto *button = gtk_button_new_with_label(label);
  gtk_widget_set_sensitive(button, enabled);
  if (suggested) {
    gtk_widget_add_css_class(button, "suggested-action");
  }

  connectVoid(button, "clicked", std::move(action));

  return button;
}

auto makeDeviceCard(AppState *state, const Device &device) -> GtkWidget * {
  auto *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(card, "card");
  gtk_widget_set_margin_bottom(card, 2);
  auto *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(inner, 8);
  gtk_widget_set_margin_bottom(inner, 8);
  gtk_widget_set_margin_start(inner, 10);
  gtk_widget_set_margin_end(inner, 10);

  auto *check = gtk_check_button_new_with_label(device.displayName().c_str());
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check), device.m_selected);
  connectVoid(check, "toggled", [state, device, check] {
    auto json = deviceCommand("select", device);
    json.set("selected", gtk_check_button_get_active(GTK_CHECK_BUTTON(check)));
    sendCommand(state, json);
  });
  gtk_box_append(GTK_BOX(inner), check);
  gtk_box_append(
      GTK_BOX(inner),
      makeLabel(device.m_address + " · " + device.m_status, "caption"));
  if (device.m_monitor_state != "idle") {
    const auto detail =
        device.m_monitor_detail.empty()
            ? device.m_monitor_state
            : device.m_monitor_state + ": " + device.m_monitor_detail;
    gtk_box_append(GTK_BOX(inner),
                   makeLabel("Monitoring " + detail, "caption"));
  }

  auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(
      GTK_BOX(actions),
      makeButton("Connect", device.m_can_connect, false, [state, device] {
        sendCommand(state, deviceCommand("connect", device));
      }));
  gtk_box_append(
      GTK_BOX(actions),
      makeButton("Disconnect", device.m_can_disconnect, false, [state, device] {
        withdrawAlert(state, device.key());
        sendCommand(state, deviceCommand("disconnect", device));
      }));
  const bool stop = device.m_can_stop_monitoring;
  gtk_box_append(GTK_BOX(actions),
                 makeButton(stop ? "Stop monitoring" : "Start monitoring",
                            stop || device.m_can_start_monitoring, true,
                            [state, device, stop] {
                              if (stop) {
                                withdrawAlert(state, device.key());
                              }

                              auto json = deviceCommand("monitor", device);
                              json.set("enabled", !stop);
                              sendCommand(state, json);
                            }));
  auto *show = gtk_button_new_with_label("Show notifications");
  gtk_widget_add_css_class(show, "flat");
  connectVoid(show, "clicked", [state, device] {
    state->m_feed_device_filter = device.key();
    rebuildFeed(state);
  });
  gtk_box_append(GTK_BOX(actions), show);
  gtk_box_append(GTK_BOX(inner), actions);
  gtk_box_append(GTK_BOX(card), inner);

  return card;
}

void rebuildDevices(AppState *state) {
  clearChildren(state->m_device_box);

  std::vector<const Device *> previous;
  std::vector<const Device *> nearby;
  for (const auto &device : state->m_devices) {
    if (deviceMatches(state, device)) {
      (device.m_saved ? previous : nearby).push_back(&device);
    }
  }

  const auto addGroup = [state](const char *heading,
                                const std::vector<const Device *> &devices,
                                const char *empty) {
    auto *title = makeLabel(heading, "title-4");
    gtk_widget_set_margin_top(title, 6);
    gtk_box_append(GTK_BOX(state->m_device_box), title);
    if (devices.empty()) {
      gtk_box_append(GTK_BOX(state->m_device_box),
                     makeLabel(empty, "dim-label"));
    }

    for (const auto *device : devices) {
      gtk_box_append(GTK_BOX(state->m_device_box),
                     makeDeviceCard(state, *device));
    }
  };

  addGroup("Previous selected devices", previous,
           "No saved devices yet. Select a device and save to keep it here.");
  addGroup("Nearby devices", nearby,
           state->m_selected_only || !state->m_device_search.empty()
               ? "No devices match. Clear the search or Selected only to see "
                 "all."
               : "Scanning for devices…");

  gtk_widget_set_sensitive(state->m_save_button, state->m_unsaved);
  gtk_widget_set_sensitive(state->m_discard_button, state->m_unsaved);
}

auto parseDevices(const Dmn_JsonValue *devices) -> std::vector<Device> {
  std::vector<Device> result;
  if (devices == nullptr) {
    return result;
  }

  for (const auto &entry : devices->items()) {
    Device device;
    device.m_adapter = entry.getString("adapter");
    device.m_address = entry.getString("device");
    device.m_name = entry.getString("name");
    device.m_status = entry.getString("status");
    device.m_monitor_state = entry.getString("monitor_state", "idle");
    device.m_monitor_detail = entry.getString("monitor_detail");
    device.m_saved = entry.getBool("saved", false);
    device.m_selected = entry.getBool("selected", false);
    device.m_can_connect = entry.getBool("can_connect", false);
    device.m_can_disconnect = entry.getBool("can_disconnect", false);
    device.m_can_start_monitoring =
        entry.getBool("can_start_monitoring", false);
    device.m_can_stop_monitoring = entry.getBool("can_stop_monitoring", false);
    result.push_back(std::move(device));
  }

  return result;
}

void updateSortButtons(AppState *state) {
  const auto label = [state](const char *name, const char *column) {
    std::string text{name};
    if (state->m_sort_column == column) {
      text += state->m_sort_ascending ? " ▲" : " ▼";
    }

    return text;
  };

  gtk_button_set_label(GTK_BUTTON(state->m_name_sort_button),
                       label("Name", "name").c_str());
  gtk_button_set_label(GTK_BUTTON(state->m_status_sort_button),
                       label("Status", "status").c_str());
}

// The app scans for its whole lifetime: request it whenever the engine
// reports it idle, throttled so a failing adapter is not hammered.
void ensureScanning(AppState *state) {
  const auto now = std::chrono::steady_clock::now();
  const bool due = !state->m_scan_requested ||
                   now - state->m_last_scan_request >= kScanRetryInterval;
  if (due && state->m_ready && state->m_adapters_known && !state->m_scanning &&
      !state->m_busy) {
    state->m_scan_requested = true;
    state->m_last_scan_request = now;
    auto json = command("scan");
    json.set("enabled", true);
    sendCommand(state, json);
  }
}

//------------------------------------------------------------------------
// Engine message handling (GTK main thread)
//------------------------------------------------------------------------

void handleState(AppState *state, const Dmn_JsonValue &message) {
  const auto *adapters = message.find("adapters");
  state->m_adapters_known = adapters != nullptr && !adapters->items().empty();
  state->m_scanning = message.getBool("scanning", false);
  state->m_busy = message.getBool("busy", false);
  state->m_unsaved = message.getBool("unsaved_changes", false);
  state->m_status = message.getString("status");
  if (const auto *sort = message.find("sort"); sort != nullptr) {
    state->m_sort_column = sort->getString("column", "name");
    state->m_sort_ascending = sort->getBool("ascending", true);
    updateSortButtons(state);
  }

  state->m_devices = parseDevices(message.find("devices"));
  const auto *devices = message.find("devices");
  const auto signature = (devices != nullptr ? devices->dump() : "") +
                         (state->m_unsaved ? "u" : "-") + state->m_sort_column +
                         (state->m_sort_ascending ? "a" : "d");
  if (signature != state->m_devices_signature) {
    state->m_devices_signature = signature;
    rebuildDevices(state);
  }

  setStatusText(state);
  ensureScanning(state);
}

void handleEvent(AppState *state, const Dmn_JsonValue &message) {
  auto event = makeEvent(message);
  const bool addedNotification =
      event.m_is_notification &&
      message.find("notification")->getString("kind") == "added";
  state->m_events.push_back(event);
  if (state->m_events.size() > kFeedLimit) {
    state->m_events.pop_front();
  }

  if (addedNotification && state->m_desktop_alerts_enabled) {
    sendAlert(state, event);
  }

  rebuildFeed(state);
}

void handleMessage(AppState *state, const std::string &text) {
  auto message = Dmn_JsonValue::parse(text);
  if (!message) {
    state->m_notice = "Bad engine message.";
    setStatusText(state);

    return;
  }

  const auto type = message->getString("type");
  if (type == "ready") {
    const int protocol = static_cast<int>(message->getNumber("protocol", 0));
    state->m_ready = protocol == dmn::gui::kEngineProtocolVersion;
    if (!state->m_ready) {
      state->m_notice = "The Bluetooth engine uses protocol " +
                        std::to_string(protocol) +
                        "; this app needs protocol " +
                        std::to_string(dmn::gui::kEngineProtocolVersion) + ".";
      setStatusText(state);
    }
  } else if (type == "state") {
    handleState(state, *message);
  } else if (type == "event") {
    handleEvent(state, *message);
  } else if (type == "persist") {
    if (state->m_settings != nullptr && !state->m_demo) {
      writeIdentities(state->m_settings, "selected-devices",
                      message->find("saved"));
      writeIdentities(state->m_settings, "monitoring-devices",
                      message->find("monitoring"));
    }
  } else if (type == "error") {
    state->m_notice = message->getString("message");
    setStatusText(state);
  }
}

struct Delivery {
  AppState *m_state;
  std::string m_text;
};

void startReader(AppState *state) {
  state->m_reader = std::thread{[state] {
    while (!state->m_stop_reader.load(std::memory_order_acquire)) {
      auto text = state->m_engine->nextMessage(kPollInterval);
      if (!text) {
        continue;
      }

      auto *delivery = new Delivery{state, std::move(*text)};
      g_idle_add_full(
          G_PRIORITY_DEFAULT,
          [](gpointer data) -> gboolean {
            std::unique_ptr<Delivery> delivery{static_cast<Delivery *>(data)};
            handleMessage(delivery->m_state, delivery->m_text);

            return G_SOURCE_REMOVE;
          },
          delivery, nullptr);
    }
  }};
}

// Joins the reader, then the engine, which stops monitors and its workers.
void stopEngine(AppState *state) {
  state->m_stop_reader.store(true, std::memory_order_release);
  if (state->m_reader.joinable()) {
    state->m_reader.join();
  }

  state->m_engine.reset();
}

//------------------------------------------------------------------------
// Actions
//------------------------------------------------------------------------

void openLink(AppState *state, const std::string &url) {
  auto *launcher = gtk_uri_launcher_new(url.c_str());
  gtk_uri_launcher_launch(launcher, GTK_WINDOW(state->m_window), nullptr,
                          nullptr, nullptr);
  g_object_unref(launcher);
}

// A titled group of link rows, laid out like the COSMIC About page.
auto makeAboutSection(AppState *state, const char *title,
                      const std::vector<std::pair<std::string, std::string>>
                          &entries) -> GtkWidget * {
  auto *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
  for (const auto &[name, url] : entries) {
    auto *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name.c_str());
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    adw_action_row_add_suffix(
        ADW_ACTION_ROW(row),
        gtk_image_new_from_icon_name("adw-external-link-symbolic"));
    connectVoid(row, "activated", [state, url] { openLink(state, url); });
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  }

  return group;
}

// Same content and order as the COSMIC About page: icon, name, author,
// version, Links, Developers, License, comments, one-line copyright.
void aboutAction(GSimpleAction *, GVariant *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  if (state->m_about_dialog != nullptr) {
    adw_dialog_present(state->m_about_dialog, state->m_window);

    return;
  }

  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 24);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);

  auto *icon = gtk_image_new_from_icon_name(kApplicationId);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 128);
  gtk_box_append(GTK_BOX(content), icon);

  auto *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_halign(header, GTK_ALIGN_CENTER);
  auto *name = gtk_label_new("Dmn Bluetooth");
  gtk_widget_add_css_class(name, "title-2");
  gtk_box_append(GTK_BOX(header), name);
  gtk_box_append(GTK_BOX(header), gtk_label_new("Chee Bin Hoh"));
  auto *version = gtk_button_new_with_label(DMN_VERSION);
  gtk_widget_set_halign(version, GTK_ALIGN_CENTER);
  gtk_widget_set_can_focus(version, FALSE);
  gtk_box_append(GTK_BOX(header), version);
  gtk_box_append(GTK_BOX(content), header);

  gtk_box_append(
      GTK_BOX(content),
      makeAboutSection(state, "Links",
                       {{"Repository", "https://github.com/cheebinhoh/Dmn"},
                        {"Issues", "https://github.com/cheebinhoh/Dmn/issues"},
                        {"Email", "mailto:CheeBin.Hoh@gmail.com"}}));
  gtk_box_append(
      GTK_BOX(content),
      makeAboutSection(state, "Developers",
                       {{"Chee Bin Hoh", "https://github.com/cheebinhoh"}}));
  gtk_box_append(
      GTK_BOX(content),
      makeAboutSection(
          state, "License",
          {{"GPL-3.0-only",
            "https://github.com/cheebinhoh/Dmn?tab=GPL-3.0-1-ov-file"}}));

  auto *comments = makeLabel("Bluetooth device and ANCS notification monitor.");
  gtk_label_set_justify(GTK_LABEL(comments), GTK_JUSTIFY_CENTER);
  gtk_label_set_xalign(GTK_LABEL(comments), 0.5F);
  gtk_box_append(GTK_BOX(content), comments);
  auto *copyright = makeLabel("© 2026 Chee Bin Hoh");
  gtk_label_set_xalign(GTK_LABEL(copyright), 0.5F);
  gtk_box_append(GTK_BOX(content), copyright);

  auto *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  auto *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), content);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scrolled);

  auto *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, "About Dmn Bluetooth");
  adw_dialog_set_content_width(dialog, 420);
  adw_dialog_set_content_height(dialog, 880);
  adw_dialog_set_child(dialog, toolbar);
  state->m_about_dialog = dialog;
  g_signal_connect(dialog, "closed",
                   G_CALLBACK(+[](AdwDialog *, gpointer data) {
                     static_cast<AppState *>(data)->m_about_dialog = nullptr;
                   }),
                   state);
  adw_dialog_present(dialog, state->m_window);
}

void desktopAlertsAction(GSimpleAction *action, GVariant *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  state->m_desktop_alerts_enabled = !state->m_desktop_alerts_enabled;
  g_simple_action_set_state(
      action, g_variant_new_boolean(state->m_desktop_alerts_enabled));
  if (!state->m_desktop_alerts_enabled) {
    withdrawAllAlerts(state);
  }

  if (state->m_settings != nullptr && !state->m_demo) {
    g_settings_set_boolean(state->m_settings, "desktop-alerts",
                           state->m_desktop_alerts_enabled);
  }
}

void addAction(GtkApplication *app, const char *name, GCallback callback,
               gpointer data) {
  auto *action = g_simple_action_new(name, nullptr);
  g_signal_connect(action, "activate", callback, data);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  g_object_unref(action);
}

//------------------------------------------------------------------------
// Widgets
//------------------------------------------------------------------------

auto makeScrolled(GtkWidget *child) -> GtkWidget * {
  auto *scrolled = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), child);

  return scrolled;
}

auto makeVerticalBox(const int spacing) -> GtkWidget * {
  auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);

  return box;
}

void sortBy(AppState *state, const char *column) {
  const bool ascending =
      state->m_sort_column == column ? !state->m_sort_ascending : true;
  auto json = command("sort");
  json.set("column", column);
  json.set("ascending", ascending);
  sendCommand(state, json);
}

auto makeDevicesPane(AppState *state) -> GtkWidget * {
  auto *root = makeVerticalBox(8);

  auto *search = gtk_search_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(search), "");
  gtk_widget_set_hexpand(search, TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(search),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Search devices", -1);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search),
                                        "Search devices");
  connectVoid(search, "search-changed", [state, search] {
    state->m_device_search = gtk_editable_get_text(GTK_EDITABLE(search));
    rebuildDevices(state);
  });
  gtk_box_append(GTK_BOX(root), search);

  auto *options = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto *selectedOnly = gtk_check_button_new_with_label("Selected only");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(selectedOnly),
                              state->m_selected_only);
  connectVoid(selectedOnly, "toggled", [state, selectedOnly] {
    state->m_selected_only =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(selectedOnly));
    if (state->m_settings != nullptr && !state->m_demo) {
      g_settings_set_boolean(state->m_settings, "selected-only",
                             state->m_selected_only);
    }

    rebuildDevices(state);
  });
  gtk_box_append(GTK_BOX(options), selectedOnly);

  auto *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(options), spacer);

  state->m_name_sort_button = gtk_button_new_with_label("Name ▲");
  state->m_status_sort_button = gtk_button_new_with_label("Status");
  connectVoid(state->m_name_sort_button, "clicked",
              [state] { sortBy(state, "name"); });
  connectVoid(state->m_status_sort_button, "clicked",
              [state] { sortBy(state, "status"); });
  gtk_box_append(GTK_BOX(options), state->m_name_sort_button);
  gtk_box_append(GTK_BOX(options), state->m_status_sort_button);
  gtk_box_append(GTK_BOX(root), options);

  auto *saveRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  state->m_save_button = makeButton("Save", false, true, [state] {
    sendCommand(state, command("save_selection"));
  });
  state->m_discard_button = makeButton("Discard", false, false, [state] {
    sendCommand(state, command("discard_selection"));
  });
  gtk_box_append(GTK_BOX(saveRow), state->m_save_button);
  gtk_box_append(GTK_BOX(saveRow), state->m_discard_button);
  gtk_box_append(GTK_BOX(root), saveRow);

  state->m_device_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(root), makeScrolled(state->m_device_box));

  return root;
}

auto makeKindButton(AppState *state, const char *label, const KindFilter kind,
                    GtkWidget *group) -> GtkWidget * {
  auto *button = gtk_toggle_button_new_with_label(label);
  if (group != nullptr) {
    gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(button),
                                GTK_TOGGLE_BUTTON(group));
  } else {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), TRUE);
  }

  connectVoid(button, "toggled", [state, button, kind] {
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button))) {
      state->m_kind_filter = kind;
      rebuildFeed(state);
    }
  });

  return button;
}

auto makeFeedPane(AppState *state) -> GtkWidget * {
  auto *root = makeVerticalBox(8);
  gtk_box_append(GTK_BOX(root), makeLabel("Notifications", "title-4"));

  auto *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto *all = makeKindButton(state, "All", KindFilter::kAll, nullptr);
  gtk_box_append(GTK_BOX(filters), all);
  gtk_box_append(
      GTK_BOX(filters),
      makeKindButton(state, "Notifications", KindFilter::kNotifications, all));
  gtk_box_append(GTK_BOX(filters),
                 makeKindButton(state, "Problems", KindFilter::kProblems, all));
  state->m_device_filter_button = gtk_button_new_with_label("All devices");
  gtk_widget_set_sensitive(state->m_device_filter_button, FALSE);
  connectVoid(state->m_device_filter_button, "clicked", [state] {
    state->m_feed_device_filter.clear();
    rebuildFeed(state);
  });
  gtk_box_append(GTK_BOX(filters), state->m_device_filter_button);
  gtk_box_append(GTK_BOX(filters), makeButton("Clear", true, false, [state] {
                   state->m_events.clear();
                   withdrawAllAlerts(state);
                   rebuildFeed(state);
                 }));
  gtk_box_append(GTK_BOX(root), filters);

  auto *categories = gtk_string_list_new(nullptr);
  gtk_string_list_append(categories, "All categories");
  for (const auto *name : kCategoryNames) {
    gtk_string_list_append(categories, name);
  }

  state->m_category_filter =
      gtk_drop_down_new(G_LIST_MODEL(categories), nullptr);
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->m_category_filter),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Category filter", -1);
  g_signal_connect(
      state->m_category_filter, "notify::selected",
      G_CALLBACK(+[](GObject *object, GParamSpec *, gpointer data) {
        auto *state = static_cast<AppState *>(data);
        state->m_category_index =
            static_cast<int>(gtk_drop_down_get_selected(GTK_DROP_DOWN(object)));
        rebuildFeed(state);
      }),
      state);
  gtk_box_append(GTK_BOX(root), state->m_category_filter);

  state->m_feed_search_entry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(
      GTK_SEARCH_ENTRY(state->m_feed_search_entry), "Search notifications");
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->m_feed_search_entry),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Search notifications", -1);
  connectVoid(state->m_feed_search_entry, "search-changed", [state] {
    state->m_feed_search =
        gtk_editable_get_text(GTK_EDITABLE(state->m_feed_search_entry));
    rebuildFeed(state);
  });
  gtk_box_append(GTK_BOX(root), state->m_feed_search_entry);

  state->m_feed_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_box_append(GTK_BOX(root), makeScrolled(state->m_feed_box));

  return root;
}

auto makeHeader(AppState *state) -> GtkWidget * {
  auto *header = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);

  auto *toggle = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-symbolic");
  gtk_widget_set_tooltip_text(toggle, "Show or hide the device panel");
  gtk_accessible_update_property(GTK_ACCESSIBLE(toggle),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Show or hide the device panel", -1);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle),
                               state->m_sidebar_open);
  connectVoid(toggle, "toggled", [state, toggle] {
    state->m_sidebar_open =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
    gtk_widget_set_visible(state->m_devices_pane, state->m_sidebar_open);
    if (state->m_settings != nullptr && !state->m_demo) {
      g_settings_set_boolean(state->m_settings, "sidebar-open",
                             state->m_sidebar_open);
    }
  });
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), toggle);

  auto *title =
      gtk_label_new(state->m_demo ? "Dmn Bluetooth (demo)" : "Dmn Bluetooth");
  gtk_widget_add_css_class(title, "heading");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), title);

  auto *menu = g_menu_new();
  g_menu_append(menu, "Desktop alerts", "app.desktop-alerts");
  auto *aboutSection = g_menu_new();
  g_menu_append(aboutSection, "_About Dmn Bluetooth", "app.about");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(aboutSection));
  g_object_unref(aboutSection);

  auto *menuButton = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton),
                                "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menuButton),
                                 G_MENU_MODEL(menu));
  gtk_widget_set_tooltip_text(menuButton, "Main Menu");
  gtk_accessible_update_property(GTK_ACCESSIBLE(menuButton),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Main menu",
                                 -1);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menuButton);
  g_object_unref(menu);

  return header;
}

void loadPreferences(AppState *state) {
  if (state->m_demo) {
    return;
  }

  state->m_settings = openSettings();
  if (state->m_settings == nullptr) {
    state->m_notice = "The application settings schema is unavailable.";

    return;
  }

  state->m_sidebar_open =
      g_settings_get_boolean(state->m_settings, "sidebar-open");
  state->m_selected_only =
      g_settings_get_boolean(state->m_settings, "selected-only");
  state->m_desktop_alerts_enabled =
      g_settings_get_boolean(state->m_settings, "desktop-alerts");
}

void sendStartupCommands(AppState *state) {
  sendCommand(state, command("hello"));

  auto configure = command("configure");
  if (state->m_settings != nullptr && !state->m_demo) {
    configure.set("saved",
                  readIdentities(state->m_settings, "selected-devices"));
    configure.set("monitoring",
                  readIdentities(state->m_settings, "monitoring-devices"));
  }

  sendCommand(state, configure);
  sendCommand(state, command("refresh"));
}

void activate(GtkApplication *app, gpointer) {
  auto *state = stateFor(app);
  if (state != nullptr) {
    gtk_window_present(GTK_WINDOW(state->m_window));

    return;
  }

  state = new AppState{app};
  state->m_demo = g_object_get_data(G_OBJECT(app), "dmn-demo") != nullptr;
  g_object_set_data_full(
      G_OBJECT(app), "dmn-bluetooth-state", state,
      [](gpointer data) { delete static_cast<AppState *>(data); });
  loadPreferences(state);

  try {
    state->m_engine = std::make_unique<dmn::gui::Dmn_Engine>(
        state->m_demo ? dmn::gui::makeDemoBackend()
                      : dmn::gui::makeBluezBackend());
  } catch (const std::exception &error) {
    g_warning("Cannot start the Bluetooth engine: %s", error.what());
    state->m_notice =
        std::string{"Cannot start the Bluetooth engine: "} + error.what();
  }

  state->m_window = adw_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(state->m_window), "Dmn Bluetooth");
  gtk_window_set_default_size(GTK_WINDOW(state->m_window), 960, 680);
  gtk_widget_set_size_request(state->m_window, 360, 420);

  addAction(app, "about", G_CALLBACK(aboutAction), state);
  addAction(app, "quit",
            G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer data) {
              g_application_quit(G_APPLICATION(data));
            }),
            app);
  addAction(app, "open",
            G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer data) {
              g_application_activate(G_APPLICATION(data));
            }),
            app);
  addAction(app, "search",
            G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer data) {
              gtk_widget_grab_focus(
                  static_cast<AppState *>(data)->m_feed_search_entry);
            }),
            state);
  auto *alerts = g_simple_action_new_stateful(
      "desktop-alerts", nullptr,
      g_variant_new_boolean(state->m_desktop_alerts_enabled));
  g_signal_connect(alerts, "activate", G_CALLBACK(desktopAlertsAction), state);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(alerts));
  g_object_unref(alerts);

  auto *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
  state->m_devices_pane = makeDevicesPane(state);
  gtk_widget_set_visible(state->m_devices_pane, state->m_sidebar_open);
  gtk_paned_set_start_child(GTK_PANED(paned), state->m_devices_pane);
  gtk_paned_set_end_child(GTK_PANED(paned), makeFeedPane(state));
  gtk_paned_set_position(GTK_PANED(paned), 480);

  state->m_status_label = makeLabel(state->m_notice, "caption");
  gtk_widget_set_margin_start(state->m_status_label, 12);
  gtk_widget_set_margin_end(state->m_status_label, 12);
  gtk_widget_set_margin_bottom(state->m_status_label, 6);

  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_box_append(GTK_BOX(content), paned);
  gtk_box_append(GTK_BOX(content), state->m_status_label);

  auto *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), makeHeader(state));
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(state->m_window),
                                     toolbar);

  // Stack the panes when the window is narrow.
  auto *breakpoint =
      adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 600sp"));
  GValue vertical = G_VALUE_INIT;
  g_value_init(&vertical, GTK_TYPE_ORIENTATION);
  g_value_set_enum(&vertical, GTK_ORIENTATION_VERTICAL);
  adw_breakpoint_add_setter(breakpoint, G_OBJECT(paned), "orientation",
                            &vertical);
  g_value_unset(&vertical);
  adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(state->m_window),
                                        breakpoint);

  static const char *quitAccelerators[] = {"<Primary>q", nullptr};
  gtk_application_set_accels_for_action(app, "app.quit", quitAccelerators);
  static const char *searchAccelerators[] = {"<Primary>f", nullptr};
  gtk_application_set_accels_for_action(app, "app.search", searchAccelerators);
  static const char *closeAccelerators[] = {"<Primary>w", nullptr};
  gtk_application_set_accels_for_action(app, "win.close", closeAccelerators);

  rebuildDevices(state);
  rebuildFeed(state);
  setStatusText(state);

  if (state->m_engine != nullptr) {
    startReader(state);
    sendStartupCommands(state);
  }

  gtk_window_present(GTK_WINDOW(state->m_window));
}

void shutdownApplication(GApplication *app, gpointer) {
  if (auto *state = stateFor(app); state != nullptr) {
    stopEngine(state);
    if (state->m_settings != nullptr) {
      g_object_unref(state->m_settings);
      state->m_settings = nullptr;
    }
  }
}

} // namespace

auto main(int argc, char **argv) -> int {
  bool demo = false;
  int kept = 1;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--version") == 0) {
      g_print("%s\n", DMN_VERSION);

      return 0;
    }

    if (std::strcmp(argv[index], "--help") == 0 ||
        std::strcmp(argv[index], "-h") == 0) {
      g_print(
          "Usage: %s [OPTION…]\n"
          "Bluetooth device and ANCS notification monitor\n\n"
          "Help Options:\n"
          "  -h, --help     Show help options\n"
          "  --version      Show application version\n"
          "  --demo         Use synthetic phones and notifications instead\n"
          "                 of BlueZ; no Bluetooth hardware is needed and\n"
          "                 saved devices are left untouched\n",
          argv[0]);

      return 0;
    }

    if (std::strcmp(argv[index], "--demo") == 0) {
      demo = true;

      continue;
    }

    argv[kept++] = argv[index];
  }

  argc = kept;
  argv[argc] = nullptr;

  adw_init();
  auto *app = adw_application_new(kApplicationId, G_APPLICATION_DEFAULT_FLAGS);
  g_application_set_version(G_APPLICATION(app), DMN_VERSION);
  if (demo) {
    g_object_set_data(G_OBJECT(app), "dmn-demo", GINT_TO_POINTER(1));
  }

  g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
  g_signal_connect(app, "shutdown", G_CALLBACK(shutdownApplication), nullptr);
  const int result = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return result;
}
