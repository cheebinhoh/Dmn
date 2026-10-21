/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file main.cpp
 * @brief GTK application shell and asynchronous BlueZ device inventory.
 *
 * The GTK main context owns widgets, frontend state, and a bounded
 * process-local event feed. Reusable selection, event, and monitor lifecycle
 * models live in the GTK-independent GUI core. BlueZ inventory/link operations
 * run through a serialized worker backend; each ANCS monitor has its own
 * worker task. Owned results and callback events are queued back to the GTK
 * context before any widget is accessed.
 */

#define G_LOG_DOMAIN "dmn-bluetooth-gtk"

#include "app/gui/core/desktop-alert-policy.hpp"
#include "app/gui/core/device-monitor-controller.hpp"
#include "app/gui/core/device-monitor-event-store.hpp"
#include "app/gui/core/device-selection-model.hpp"

#include "dmn-ancs-bluez-monitor.hpp"
#include "dmn-bluez-client.hpp"

#include <adwaita.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr auto kApplicationId = "com.cheebinhoh.dmn-bluetooth";
constexpr auto kSettingsSchemaId = "com.cheebinhoh.DmnBluetooth";
constexpr auto kDiscoveryDuration = std::chrono::seconds{30};
constexpr auto kDeviceStateTimeout = std::chrono::seconds{30};
constexpr auto kDeviceStatePollInterval = std::chrono::milliseconds{50};
constexpr auto kDeviceSnapshotTimeout = std::chrono::seconds{2};
constexpr auto kDefaultWindowWidth = 960;
std::atomic_uint64_t g_eventSequence{1};

enum class Dmn_Operation {
  kInventory,
  kStartDiscovery,
  kStopDiscovery,
  kConnect,
  kDisconnect
};

auto operationName(const Dmn_Operation operation) -> const char * {
  switch (operation) {
  case Dmn_Operation::kInventory:
    return "inventory refresh";
  case Dmn_Operation::kStartDiscovery:
    return "discovery start";
  case Dmn_Operation::kStopDiscovery:
    return "discovery stop";
  case Dmn_Operation::kConnect:
    return "device connection";
  case Dmn_Operation::kDisconnect:
    return "device disconnection";
  }

  return "unknown operation";
}

/** @brief Owned result sent from the BlueZ worker to the GTK main context. */
struct BackendReply {
  Dmn_Operation m_operation{Dmn_Operation::kInventory};
  std::optional<dmn::Dmn_BluezInventory> m_inventory;
  std::string m_error;
  bool m_discovery_started{};
  bool m_discovery_stopped{};
  bool m_connection_cancelled{};
};

/** @brief Serialize every BlueZ call on the application's worker thread. */
class BluezInventoryBackend {
public:
  BluezInventoryBackend() = default;
  // The mutex and serialized client make this backend instance-specific.
  BluezInventoryBackend(const BluezInventoryBackend &) = delete;
  auto
  operator=(const BluezInventoryBackend &) -> BluezInventoryBackend & = delete;
  BluezInventoryBackend(BluezInventoryBackend &&) = delete;
  auto operator=(BluezInventoryBackend &&) -> BluezInventoryBackend & = delete;

  /**
   * @brief Perform a serialized BlueZ operation and capture an inventory.
   *
   * Calls are serialized on the worker thread. Discovery ownership is retained
   * so the UI can stop its scan during timeout or shutdown.
   *
   * @param operation Operation requested by the GTK main context.
   * @param adapterPath BlueZ object path for discovery, if needed.
   * @return Owned inventory, operation status, and any failure message.
   */
  auto perform(Dmn_Operation operation, const std::string &adapterPath,
               const std::optional<dmn::gui::Dmn_DeviceIdentity> &identity,
               const std::shared_ptr<std::atomic_bool> &cancelConnect)
      -> BackendReply {
    std::lock_guard lock{m_mutex};
    try {
      if (!m_client) {
        m_client = std::make_unique<dmn::Dmn_BluezClient>();
      }

      if (operation == Dmn_Operation::kStartDiscovery) {
        const auto result = m_client->startDiscovery(adapterPath);
        if (!result) {
          return {operation, std::nullopt, result.error().message};
        }

        m_discovery_active = true;
      } else if (operation == Dmn_Operation::kStopDiscovery) {
        const auto result = m_client->stopDiscovery();
        if (!result) {
          return {operation, std::nullopt, result.error().message};
        }

        m_discovery_active = false;
      }

      auto inventory = m_client->snapshot();
      if (!inventory) {
        return {operation, std::nullopt, inventory.error().message};
      }

      if (operation == Dmn_Operation::kConnect ||
          operation == Dmn_Operation::kDisconnect) {
        if (!identity) {
          return {operation, std::nullopt,
                  "device identity is missing for the requested operation"};
        }

        const auto device = std::find_if(
            inventory->devices.begin(), inventory->devices.end(),
            [&identity, &inventory](const auto &candidate) {
              const auto adapter = std::find_if(
                  inventory->adapters.begin(), inventory->adapters.end(),
                  [&candidate, &identity](const auto &candidateAdapter) {
                    return candidateAdapter.object_path ==
                               candidate.adapter_path &&
                           candidateAdapter.address ==
                               identity->m_adapter_address;
                  });
              return candidate.address == identity->m_device_address &&
                     adapter != inventory->adapters.end();
            });
        if (device == inventory->devices.end()) {
          return {operation, std::nullopt,
                  "selected device is no longer in the BlueZ inventory"};
        }

        const auto requestedConnected = operation == Dmn_Operation::kConnect;
        const auto stateDeadline =
            std::chrono::steady_clock::now() + kDeviceStateTimeout;
        const auto result =
            operation == Dmn_Operation::kConnect
                ? m_client->connectDevice(identity->m_device_address,
                                          identity->m_adapter_address,
                                          kDeviceStateTimeout)
                : m_client->disconnectDevice(identity->m_device_address,
                                             identity->m_adapter_address,
                                             kDeviceStateTimeout);
        // Connect is bounded but synchronous; honor cancellation as soon as
        // it returns, before waiting for BlueZ to publish the connected state.
        if (operation == Dmn_Operation::kConnect && cancelConnect != nullptr &&
            cancelConnect->load(std::memory_order_acquire)) {
          auto current = m_client->snapshot();

          return {operation,
                  current ? std::optional<dmn::Dmn_BluezInventory>{std::move(
                                *current)}
                          : std::nullopt,
                  {},
                  false,
                  false,
                  true};
        }

        if (!result) {
          return {operation, std::nullopt, result.error().message};
        }

        for (;;) {
          // A separate cancel task sends Disconnect while this worker waits.
          if (operation == Dmn_Operation::kConnect &&
              cancelConnect != nullptr &&
              cancelConnect->load(std::memory_order_acquire)) {
            auto current = m_client->snapshot();

            return {operation,
                    current ? std::optional<dmn::Dmn_BluezInventory>{std::move(
                                  *current)}
                            : std::nullopt,
                    {},
                    false,
                    false,
                    true};
          }

          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  stateDeadline - std::chrono::steady_clock::now());
          if (remaining <= std::chrono::milliseconds::zero()) {
            return {operation, std::nullopt,
                    std::string{"device did not reach the requested "} +
                        (requestedConnected ? "connected" : "disconnected") +
                        " state before the deadline"};
          }

          auto current = m_client->snapshot(std::min(
              remaining, std::chrono::duration_cast<std::chrono::milliseconds>(
                             kDeviceSnapshotTimeout)));
          if (!current) {
            return {operation, std::nullopt, current.error().message};
          }

          const auto currentDevice = std::find_if(
              current->devices.begin(), current->devices.end(),
              [&identity, &current](const auto &candidate) {
                const auto adapter = std::find_if(
                    current->adapters.begin(), current->adapters.end(),
                    [&candidate, &identity](const auto &candidateAdapter) {
                      return candidateAdapter.object_path ==
                                 candidate.adapter_path &&
                             candidateAdapter.address ==
                                 identity->m_adapter_address;
                    });
                return candidate.address == identity->m_device_address &&
                       adapter != current->adapters.end();
              });
          if (currentDevice == current->devices.end()) {
            return {operation, std::nullopt,
                    "selected device disappeared before its requested "
                    "connection state was observed"};
          }

          if (currentDevice->connected.value_or(false) == requestedConnected) {
            inventory = std::move(current);

            break;
          }

          std::this_thread::sleep_for(
              std::min(kDeviceStatePollInterval,
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           stateDeadline - std::chrono::steady_clock::now())));
        }
      }

      if (!inventory) {
        return {
            operation, std::nullopt, inventory.error().message,
            operation == Dmn_Operation::kStartDiscovery && m_discovery_active,
            operation == Dmn_Operation::kStopDiscovery && !m_discovery_active};
      }

      return {operation,
              std::move(*inventory),
              {},
              operation == Dmn_Operation::kStartDiscovery && m_discovery_active,
              operation == Dmn_Operation::kStopDiscovery &&
                  !m_discovery_active};
    } catch (const std::exception &exception) {
      return {operation, std::nullopt, exception.what()};
    }
  }

private:
  std::mutex m_mutex;
  std::unique_ptr<dmn::Dmn_BluezClient> m_client;
  bool m_discovery_active{};
};

struct AdapterOption {
  std::string m_path;
  std::string m_address;
};

/** @brief Per-device frontend state for one ANCS monitor session. */
struct MonitorSession {
  dmn::gui::Dmn_DeviceIdentity m_identity;
  std::shared_ptr<dmn::Dmn_AncsBluezMonitor> m_monitor;
  std::shared_ptr<std::atomic_bool> m_cancel_start;
  std::string m_status{"Not monitoring"};
  std::string m_error;
  bool m_busy{};
  bool m_stopping{};
  bool m_start_cancel_requested{};
  bool m_monitor_restart_required{};
  bool m_disconnect_after_stop{};
  bool m_connect_after_stop{};
};

/** @brief A row connection action waiting for another Bluetooth operation. */
struct QueuedConnectionAction {
  dmn::gui::Dmn_DeviceIdentity m_identity;
  bool m_connect{};
};

/** @brief GTK-main-context-owned widgets, settings, and selection state. */
struct AppState {
  explicit AppState(GtkApplication *application)
      : m_app{application},
        m_backend{std::make_shared<BluezInventoryBackend>()}, m_selection{} {}

  // GTK callbacks retain this address, and the destructor releases owned refs.
  AppState(const AppState &) = delete;
  auto operator=(const AppState &) -> AppState & = delete;
  AppState(AppState &&) = delete;
  auto operator=(AppState &&) -> AppState & = delete;

  ~AppState() {
    if (m_scan_poll_source != 0) {
      g_source_remove(m_scan_poll_source);
    }

    if (m_device_poll_source != 0) {
      g_source_remove(m_device_poll_source);
    }

    if (m_pane_restore_source != 0) {
      g_source_remove(m_pane_restore_source);
    }

    if (m_settings != nullptr) {
      g_object_unref(m_settings);
    }

    if (m_adapter_names != nullptr) {
      g_object_unref(m_adapter_names);
    }

    g_clear_object(&m_event_rows);
    g_clear_object(&m_event_filter);
    g_clear_object(&m_event_filtered_rows);
    g_clear_object(&m_event_sorter);
    g_clear_object(&m_event_sorted_rows);
    g_clear_object(&m_event_selection);
  }

  GtkApplication *m_app{};
  GtkWidget *m_window{};
  GtkWidget *m_paned{};
  GtkWidget *m_sidebar_page{};
  GtkWidget *m_device_page{};
  GtkWidget *m_back_button{};
  GtkWidget *m_device_list{};
  GtkWidget *m_adapter_dropdown{};
  GtkWidget *m_scan_actions{};
  GtkWidget *m_selection_actions{};
  GtkWidget *m_status_label{};
  GtkWidget *m_name_sort_button{};
  GtkWidget *m_status_sort_button{};
  GtkWidget *m_selected_only_filter{};
  GtkWidget *m_refresh_button{};
  GtkWidget *m_scan_button{};
  GtkWidget *m_stop_scan_button{};
  GtkWidget *m_save_button{};
  GtkWidget *m_discard_button{};
  GtkWidget *m_details_page{};
  GtkWidget *m_feed_heading{};
  GtkWidget *m_feed_controls{};
  GtkWidget *m_feed_search{};
  GtkWidget *m_feed_kind_filter{};
  GtkWidget *m_feed_category_filter{};
  GtkWidget *m_feed_all_devices_filter{};
  GtkWidget *m_feed_notice{};
  GtkWidget *m_feed_clear_filters{};
  GtkWidget *m_feed_clear_notifications{};
  GtkWidget *m_feed_empty{};
  GtkWidget *m_feed_scroll{};
  GtkWidget *m_feed_view{};
  GtkStringList *m_adapter_names{};
  GListStore *m_event_rows{};
  GtkCustomFilter *m_event_filter{};
  GtkFilterListModel *m_event_filtered_rows{};
  GtkCustomSorter *m_event_sorter{};
  GtkSortListModel *m_event_sorted_rows{};
  GtkSingleSelection *m_event_selection{};
  GSettings *m_settings{};
  std::shared_ptr<BluezInventoryBackend> m_backend;
  dmn::gui::Dmn_DeviceSelectionModel m_selection;
  dmn::gui::Dmn_DeviceMonitorController m_monitor_controller;
  std::vector<AdapterOption> m_adapters;
  std::string m_selected_adapter_address;
  bool m_busy{};
  bool m_scanning{};
  bool m_rendering{};
  bool m_collapsed{};
  bool m_showing_device_page{};
  bool m_pane_position_ready{};
  double m_pane_fraction{0.5};
  bool m_inventory_loaded{};
  std::string m_inventory_error;
  std::optional<dmn::gui::Dmn_DeviceIdentity> m_viewed_device;
  std::vector<dmn::gui::Dmn_DeviceIdentity> m_monitoring_intents;
  std::unordered_map<std::string, MonitorSession> m_monitor_sessions;
  std::unordered_map<std::string, std::size_t> m_desktop_alert_counts;
  bool m_desktop_alerts_enabled{};
  std::optional<dmn::gui::Dmn_DeviceIdentity> m_connecting_device;
  std::shared_ptr<std::atomic_bool> m_connect_cancel_token;
  bool m_connection_task_active{};
  bool m_connection_cancel_requested{};
  bool m_connection_cancel_task_active{};
  std::optional<dmn::gui::Dmn_DeviceIdentity> m_disconnecting_device;
  std::optional<dmn::gui::Dmn_DeviceIdentity> m_reconnect_after_disconnect;
  std::optional<QueuedConnectionAction> m_queued_connection_action;
  dmn::gui::Dmn_DeviceMonitorEventStore m_event_store;
  dmn::gui::Dmn_DeviceMonitorEventFilter m_event_filters;
  // Keep the feed revision separate from routine inventory refreshes so
  // rebuilding device status does not destroy selectable notification text.
  std::uint64_t m_event_feed_revision{};
  std::uint64_t m_rendered_event_feed_revision{};
  std::optional<dmn::gui::Dmn_DeviceIdentity> m_rendered_feed_device;
  bool m_event_feed_rendered{};
  bool m_updating_event_filters{};
  std::chrono::system_clock::time_point m_session_started{
      std::chrono::system_clock::now()};
  bool m_close_requested{};
  std::size_t m_monitor_operations{};
  guint m_scan_poll_source{};
  guint m_device_poll_source{};
  guint m_pane_restore_source{};
  std::chrono::steady_clock::time_point m_scan_deadline{};
};

/** @brief Owned arguments copied into a GTask worker invocation. */
struct TaskData {
  std::shared_ptr<BluezInventoryBackend> m_backend;
  Dmn_Operation m_operation{Dmn_Operation::kInventory};
  std::string m_adapter_path;
  std::optional<dmn::gui::Dmn_DeviceIdentity> m_identity;
  std::shared_ptr<std::atomic_bool> m_cancel_connect;
};

struct ConnectionCancelTaskData {
  dmn::gui::Dmn_DeviceIdentity m_identity;
};

struct ConnectionCancelReply {
  dmn::gui::Dmn_DeviceIdentity m_identity;
  std::string m_error;
};

/** @brief Callback context identifying one selection row and its owner. */
struct RowContext {
  AppState *m_state{};
  dmn::gui::Dmn_DeviceIdentity m_identity;
};

enum class Dmn_MonitorOperation { kStart, kStop };

/** @brief Worker arguments for one per-device monitor transition. */
struct MonitorTaskData {
  MonitorTaskData(
      GtkApplication *app, const dmn::gui::Dmn_DeviceIdentity &identity,
      const Dmn_MonitorOperation operation,
      const std::shared_ptr<dmn::Dmn_AncsBluezMonitor> &monitor,
      const std::shared_ptr<std::atomic_bool> &cancelStart,
      const std::chrono::system_clock::time_point notificationCutoff)
      : m_app{app}, m_identity{identity}, m_operation{operation},
        m_monitor{monitor}, m_cancel_start{cancelStart},
        m_notification_cutoff{notificationCutoff} {}

  ~MonitorTaskData() { g_object_unref(m_app); }

  // This task data owns a GObject reference and must not be duplicated.
  MonitorTaskData(const MonitorTaskData &) = delete;
  auto operator=(const MonitorTaskData &) -> MonitorTaskData & = delete;
  MonitorTaskData(MonitorTaskData &&) = delete;
  auto operator=(MonitorTaskData &&) -> MonitorTaskData & = delete;

  GtkApplication *m_app{};
  dmn::gui::Dmn_DeviceIdentity m_identity;
  Dmn_MonitorOperation m_operation{Dmn_MonitorOperation::kStart};
  std::shared_ptr<dmn::Dmn_AncsBluezMonitor> m_monitor;
  std::shared_ptr<std::atomic_bool> m_cancel_start;
  std::chrono::system_clock::time_point m_notification_cutoff;
};

/** @brief Owned per-device monitor result returned to the GTK context. */
struct MonitorReply {
  dmn::gui::Dmn_DeviceIdentity m_identity;
  Dmn_MonitorOperation m_operation{Dmn_MonitorOperation::kStart};
  std::shared_ptr<dmn::Dmn_AncsBluezMonitor> m_monitor;
  std::string m_error;
  bool m_cancelled{};
};

auto stateFor(GObject *object) -> AppState * {
  return static_cast<AppState *>(
      g_object_get_data(object, "dmn-bluetooth-state"));
}

void withdrawDesktopAlerts(AppState *state) {
  for (const auto &[notificationId, count] : state->m_desktop_alert_counts) {
    (void)count;
    g_application_withdraw_notification(G_APPLICATION(state->m_app),
                                        notificationId.c_str());
  }

  state->m_desktop_alert_counts.clear();
}

void withdrawDesktopAlert(AppState *state,
                          const dmn::gui::Dmn_DeviceIdentity &identity) {
  const auto alert = dmn::gui::makeDesktopAlert(identity, 1);
  if (state->m_desktop_alert_counts.erase(alert.m_id) != 0) {
    g_application_withdraw_notification(G_APPLICATION(state->m_app),
                                        alert.m_id.c_str());
  }
}

void sendDesktopAlert(AppState *state,
                      const dmn::gui::Dmn_DeviceIdentity &identity) {
  if (!state->m_desktop_alerts_enabled) {
    return;
  }

  const auto notificationId = dmn::gui::makeDesktopAlert(identity, 1).m_id;
  auto &count = state->m_desktop_alert_counts[notificationId];
  ++count;
  const auto alert = dmn::gui::makeDesktopAlert(identity, count);
  auto *notification = g_notification_new(alert.m_title.c_str());
  g_notification_set_body(notification, alert.m_body.c_str());
  g_notification_set_default_action(notification, "app.open");
  g_application_send_notification(G_APPLICATION(state->m_app),
                                  alert.m_id.c_str(), notification);
  g_object_unref(notification);
}

void refreshView(AppState *state, bool rebuildDevices = true);
void rebuildEventFeed(AppState *state);
void setStatus(AppState *state, const std::string &message,
               bool announce = true);
auto isSelected(const std::vector<dmn::gui::Dmn_DeviceIdentity> &selection,
                const dmn::gui::Dmn_DeviceIdentity &identity) -> bool;
void synchronizeMonitorSessions(AppState *state);
void requestConnectionCancel(AppState *state,
                             const dmn::gui::Dmn_DeviceIdentity &identity);

auto identityKey(const dmn::gui::Dmn_DeviceIdentity &identity) -> std::string {
  return identity.m_adapter_address + "|" + identity.m_device_address;
}

auto findMonitorSession(AppState *state,
                        const dmn::gui::Dmn_DeviceIdentity &identity)
    -> MonitorSession * {
  const auto found = state->m_monitor_sessions.find(identityKey(identity));

  return found == state->m_monitor_sessions.end() ? nullptr : &found->second;
}

auto eventKindName(const dmn::Dmn_AncsEvent &event) -> const char * {
  switch (event.kind) {
  case dmn::Dmn_AncsEvent::Kind::StateChanged:
    return "Monitor state";
  case dmn::Dmn_AncsEvent::Kind::NotificationAdded:
    return "Added";
  case dmn::Dmn_AncsEvent::Kind::NotificationUpdated:
    return "Updated";
  case dmn::Dmn_AncsEvent::Kind::NotificationRemoved:
    return "Removed";
  case dmn::Dmn_AncsEvent::Kind::AttributesUnavailable:
    return "Attributes unavailable";
  case dmn::Dmn_AncsEvent::Kind::Error:
    return "Monitor error";
  }

  return "Unknown event";
}

auto monitorStateName(const dmn::Dmn_AncsState state) -> const char * {
  switch (state) {
  case dmn::Dmn_AncsState::Stopped:
    return "Stopped";
  case dmn::Dmn_AncsState::Starting:
    return "Starting ANCS";
  case dmn::Dmn_AncsState::ResolvingServices:
    return "Waiting for services";
  case dmn::Dmn_AncsState::DiscoveringANCS:
    return "Checking ANCS";
  case dmn::Dmn_AncsState::SubscribingDataSource:
    return "Subscribing to notification data";
  case dmn::Dmn_AncsState::SubscribingNotificationSource:
    return "Subscribing to notification events";
  case dmn::Dmn_AncsState::Ready:
    return "Ready · waiting for new notifications";
  case dmn::Dmn_AncsState::Stopping:
    return "Stopping monitor";
  case dmn::Dmn_AncsState::Failed:
    return "Monitor failed";
  }

  return "Unknown monitor state";
}

struct MonitorEventDispatch {
  MonitorEventDispatch(GtkApplication *app,
                       const dmn::gui::Dmn_DeviceIdentity &identity,
                       const std::uint64_t sequence,
                       const std::chrono::system_clock::time_point received,
                       const dmn::Dmn_AncsEvent &event)
      : m_app{app}, m_identity{identity}, m_sequence{sequence},
        m_received{received}, m_event{event} {}

  ~MonitorEventDispatch() { g_object_unref(m_app); }

  // This dispatch owns a GObject reference and must not be duplicated.
  MonitorEventDispatch(const MonitorEventDispatch &) = delete;
  auto
  operator=(const MonitorEventDispatch &) -> MonitorEventDispatch & = delete;
  MonitorEventDispatch(MonitorEventDispatch &&) = delete;
  auto operator=(MonitorEventDispatch &&) -> MonitorEventDispatch & = delete;

  GtkApplication *m_app{};
  dmn::gui::Dmn_DeviceIdentity m_identity;
  std::uint64_t m_sequence{};
  std::chrono::system_clock::time_point m_received;
  dmn::Dmn_AncsEvent m_event;
};

void dispatchMonitorEvent(GtkApplication *app,
                          const dmn::gui::Dmn_DeviceIdentity &identity,
                          const dmn::Dmn_AncsEvent &event) {
  auto *dispatch = new MonitorEventDispatch{
      GTK_APPLICATION(g_object_ref(app)), identity,
      g_eventSequence.fetch_add(1, std::memory_order_relaxed),
      std::chrono::system_clock::now(), event};
  g_idle_add_full(
      G_PRIORITY_DEFAULT,
      [](gpointer userData) -> gboolean {
        std::unique_ptr<MonitorEventDispatch> dispatch{
            static_cast<MonitorEventDispatch *>(userData)};
        auto *state = stateFor(G_OBJECT(dispatch->m_app));
        if (state != nullptr) {
          auto *session = findMonitorSession(state, dispatch->m_identity);
          bool monitorNeedsReconcile = false;
          if (session != nullptr) {
            const auto requireMonitorRestart = [&] {
              const bool requested = isSelected(state->m_monitoring_intents,
                                                dispatch->m_identity) &&
                                     !state->m_close_requested;
              if (requested && !session->m_monitor_restart_required) {
                (void)state->m_monitor_controller.recordStartFailure(
                    dispatch->m_identity,
                    dmn::gui::Dmn_DeviceMonitorController::Clock::now(),
                    static_cast<unsigned int>(g_random_int_range(80, 101)));
              }

              session->m_monitor_restart_required = requested;
            };
            if (dispatch->m_event.kind ==
                    dmn::Dmn_AncsEvent::Kind::StateChanged &&
                dispatch->m_event.state) {
              session->m_status = monitorStateName(*dispatch->m_event.state);
              if (*dispatch->m_event.state == dmn::Dmn_AncsState::Failed) {
                session->m_error = "The ANCS monitor entered a failed state.";
              }

              if ((*dispatch->m_event.state == dmn::Dmn_AncsState::Failed ||
                   *dispatch->m_event.state == dmn::Dmn_AncsState::Stopped) &&
                  session->m_monitor != nullptr && !session->m_busy &&
                  !session->m_stopping) {
                // ANCS failure does not clear user intent; reconcile and retry.
                requireMonitorRestart();
                monitorNeedsReconcile = true;
              }
            } else if (dispatch->m_event.kind ==
                           dmn::Dmn_AncsEvent::Kind::Error &&
                       dispatch->m_event.error) {
              session->m_error = dispatch->m_event.error->message;
              session->m_status = "Monitor failed";
              if (session->m_monitor != nullptr && !session->m_busy &&
                  !session->m_stopping) {
                requireMonitorRestart();
                monitorNeedsReconcile = true;
              }
            }
          }

          if (session != nullptr && !session->m_stopping &&
              (session->m_monitor != nullptr || session->m_busy) &&
              dmn::gui::shouldSendDesktopAlert(dispatch->m_event)) {
            sendDesktopAlert(state, dispatch->m_identity);
          }

          state->m_event_store.append(
              {dispatch->m_identity, dispatch->m_sequence, dispatch->m_received,
               std::move(dispatch->m_event)});
          ++state->m_event_feed_revision;
          refreshView(state);
          if (monitorNeedsReconcile) {
            synchronizeMonitorSessions(state);
          }
        }

        return G_SOURCE_REMOVE;
      },
      dispatch, nullptr);
}

void postMonitorProgress(GtkApplication *app,
                         const dmn::gui::Dmn_DeviceIdentity &identity,
                         const dmn::Dmn_AncsMonitorProgress progress,
                         const std::shared_ptr<std::atomic_bool> &cancelStart) {
  const auto message = [progress]() -> const char * {
    switch (progress) {
    case dmn::Dmn_AncsMonitorProgress::Inventory:
      return "Checking Bluetooth device";
    case dmn::Dmn_AncsMonitorProgress::Discovery:
      return "Discovering Bluetooth device";
    case dmn::Dmn_AncsMonitorProgress::Pairing:
      return "Pairing Bluetooth device";
    case dmn::Dmn_AncsMonitorProgress::Connection:
      return "Connecting Bluetooth device";
    case dmn::Dmn_AncsMonitorProgress::AncsStartup:
      return "Starting ANCS monitor";
    }

    return "Preparing monitor";
  }();

  struct ProgressDispatch {
    ProgressDispatch(GtkApplication *app,
                     const dmn::gui::Dmn_DeviceIdentity &identity,
                     std::string status,
                     const std::shared_ptr<std::atomic_bool> &cancelStart)
        : m_app{app}, m_identity{identity}, m_status{std::move(status)},
          m_cancel_start{cancelStart} {}

    ~ProgressDispatch() { g_object_unref(m_app); }

    // This dispatch owns a GObject reference and must not be duplicated.
    ProgressDispatch(const ProgressDispatch &) = delete;
    auto operator=(const ProgressDispatch &) -> ProgressDispatch & = delete;
    ProgressDispatch(ProgressDispatch &&) = delete;
    auto operator=(ProgressDispatch &&) -> ProgressDispatch & = delete;

    GtkApplication *m_app{};
    dmn::gui::Dmn_DeviceIdentity m_identity;
    std::string m_status;
    std::shared_ptr<std::atomic_bool> m_cancel_start;
  };
  auto *dispatch = new ProgressDispatch{GTK_APPLICATION(g_object_ref(app)),
                                        identity, message, cancelStart};
  g_idle_add_full(
      G_PRIORITY_DEFAULT,
      [](gpointer userData) -> gboolean {
        std::unique_ptr<ProgressDispatch> dispatch{
            static_cast<ProgressDispatch *>(userData)};
        auto *state = stateFor(G_OBJECT(dispatch->m_app));
        if (state != nullptr &&
            (dispatch->m_cancel_start == nullptr ||
             !dispatch->m_cancel_start->load(std::memory_order_acquire))) {
          if (auto *session = findMonitorSession(state, dispatch->m_identity);
              session != nullptr) {
            session->m_status = dispatch->m_status;
          }

          setStatus(state, dispatch->m_status);
          refreshView(state);
        }

        return G_SOURCE_REMOVE;
      },
      dispatch, nullptr);
}

void requestOperation(
    AppState *state, Dmn_Operation operation, std::string adapterPath = {},
    std::optional<dmn::gui::Dmn_DeviceIdentity> identity = std::nullopt,
    bool announceStatus = true);
void requestMonitorOperation(AppState *state,
                             const dmn::gui::Dmn_DeviceIdentity &identity,
                             Dmn_MonitorOperation operation,
                             bool disconnectAfterStop = false);
void maybeFinishClose(AppState *state);
void updateMonitorPolling(AppState *state);
void requestClose(AppState *state);
auto setMonitoringIntent(AppState *state,
                         const dmn::gui::Dmn_DeviceIdentity &identity,
                         bool enabled) -> bool;
auto persistMonitoringIntents(AppState *state) -> bool;

auto toGuiInventory(const dmn::Dmn_BluezInventory &inventory)
    -> std::vector<dmn::gui::Dmn_DeviceEntry> {
  std::unordered_map<std::string, std::string> adapterAddresses;
  for (const auto &adapter : inventory.adapters) {
    if (!adapter.object_path.empty() && !adapter.address.empty()) {
      adapterAddresses.emplace(adapter.object_path, adapter.address);
    }
  }

  std::vector<dmn::gui::Dmn_DeviceEntry> devices;
  devices.reserve(inventory.devices.size());
  for (const auto &device : inventory.devices) {
    const auto adapter = adapterAddresses.find(device.adapter_path);
    if (adapter == adapterAddresses.end() || device.address.empty()) {
      continue;
    }

    devices.push_back({{adapter->second, device.address},
                       device.name,
                       device.paired,
                       device.connected,
                       true,
                       device.seen_during_discovery});
  }

  return devices;
}

void updateAdapters(AppState *state, const dmn::Dmn_BluezInventory &inventory) {
  state->m_adapters.clear();
  gtk_string_list_splice(
      state->m_adapter_names, 0,
      g_list_model_get_n_items(G_LIST_MODEL(state->m_adapter_names)), nullptr);

  for (const auto &adapter : inventory.adapters) {
    if (!adapter.powered.value_or(false) || adapter.object_path.empty() ||
        adapter.address.empty()) {
      continue;
    }

    auto label = adapter.name.empty() ? adapter.address : adapter.name;
    if (label != adapter.address) {
      label += " · " + adapter.address;
    }

    state->m_adapters.push_back({adapter.object_path, adapter.address});
    gtk_string_list_append(state->m_adapter_names, label.c_str());
  }

  const auto selected = std::find_if(
      state->m_adapters.begin(), state->m_adapters.end(),
      [state](const auto &adapter) {
        return adapter.m_address == state->m_selected_adapter_address;
      });
  if (selected != state->m_adapters.end()) {
    gtk_drop_down_set_selected(
        GTK_DROP_DOWN(state->m_adapter_dropdown),
        static_cast<guint>(std::distance(state->m_adapters.begin(), selected)));
  } else if (!state->m_adapters.empty()) {
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->m_adapter_dropdown), 0);
  } else {
    state->m_selected_adapter_address.clear();
  }

  gtk_widget_set_sensitive(state->m_adapter_dropdown,
                           !state->m_adapters.empty() && !state->m_busy &&
                               !state->m_close_requested);
  gtk_widget_set_sensitive(state->m_scan_button,
                           !state->m_adapters.empty() && !state->m_busy &&
                               !state->m_close_requested && !state->m_scanning);
}

void setStatus(AppState *state, const std::string &message,
               const bool announce) {
  const bool changed =
      message != gtk_label_get_text(GTK_LABEL(state->m_status_label));
  gtk_label_set_text(GTK_LABEL(state->m_status_label), message.c_str());
  if (announce && changed && gtk_widget_get_mapped(state->m_status_label)) {
    gtk_accessible_announce(GTK_ACCESSIBLE(state->m_status_label),
                            message.c_str(),
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  }
}

void updateSplitLayout(AppState *state) {
  gtk_widget_set_visible(state->m_sidebar_page,
                         !state->m_collapsed || !state->m_showing_device_page);
  gtk_widget_set_visible(state->m_device_page,
                         !state->m_collapsed || state->m_showing_device_page);
  gtk_widget_set_visible(state->m_back_button, state->m_collapsed);
}

void backToDevicesClicked(GtkButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  state->m_showing_device_page = false;
  updateSplitLayout(state);
}

void panePositionChanged(GObject *object, GParamSpec *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  if (state->m_settings == nullptr || !state->m_pane_position_ready ||
      state->m_collapsed) {
    return;
  }

  const auto position = gtk_paned_get_position(GTK_PANED(object));
  const auto width = gtk_widget_get_width(GTK_WIDGET(object));
  if (position > 0 && width > 0) {
    state->m_pane_fraction =
        std::clamp(static_cast<double>(position) / width, 0.1, 0.9);
    if (!g_settings_set_double(state->m_settings, "pane-fraction",
                               state->m_pane_fraction)) {
      g_warning("Could not persist the device pane proportion");
    }
  }
}

gboolean restorePanePosition(gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  const auto width = gtk_widget_get_width(state->m_paned);
  if (width <= 0 || !gtk_window_is_maximized(GTK_WINDOW(state->m_window))) {
    return G_SOURCE_CONTINUE;
  }

  gtk_paned_set_position(
      GTK_PANED(state->m_paned),
      static_cast<int>(std::lround(width * state->m_pane_fraction)));
  state->m_pane_position_ready = true;
  state->m_pane_restore_source = 0;

  return G_SOURCE_REMOVE;
}

void windowBreakpointChanged(GObject *object, GParamSpec *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  state->m_collapsed = adw_application_window_get_current_breakpoint(
                           ADW_APPLICATION_WINDOW(object)) != nullptr;
  const auto orientation = state->m_collapsed ? GTK_ORIENTATION_VERTICAL
                                              : GTK_ORIENTATION_HORIZONTAL;
  gtk_orientable_set_orientation(GTK_ORIENTABLE(state->m_scan_actions),
                                 orientation);
  gtk_orientable_set_orientation(GTK_ORIENTABLE(state->m_selection_actions),
                                 orientation);
  for (auto *button : {state->m_refresh_button, state->m_scan_button,
                       state->m_stop_scan_button}) {
    gtk_widget_set_hexpand(button, state->m_collapsed);
  }

  gtk_widget_set_hexpand(state->m_save_button, state->m_collapsed);
  gtk_widget_set_hexpand(state->m_discard_button, state->m_collapsed);
  updateSplitLayout(state);
}

void updateButtonState(AppState *state) {
  gtk_widget_set_sensitive(state->m_refresh_button,
                           !state->m_busy && !state->m_close_requested);
  gtk_widget_set_sensitive(state->m_save_button,
                           !state->m_busy && !state->m_close_requested &&
                               state->m_selection.hasUnsavedChanges());
  gtk_widget_set_sensitive(state->m_discard_button,
                           !state->m_busy && !state->m_close_requested &&
                               state->m_selection.hasUnsavedChanges());
  gtk_widget_set_sensitive(state->m_scan_button,
                           !state->m_busy && !state->m_close_requested &&
                               !state->m_scanning &&
                               !state->m_adapters.empty());
  gtk_widget_set_sensitive(state->m_stop_scan_button,
                           !state->m_busy && !state->m_close_requested &&
                               state->m_scanning);
  gtk_widget_set_sensitive(state->m_adapter_dropdown,
                           !state->m_busy && !state->m_close_requested &&
                               !state->m_adapters.empty());
}

void rowToggled(GtkCheckButton *button, gpointer userData) {
  auto *context = static_cast<RowContext *>(userData);
  auto *state = context->m_state;
  if (state->m_rendering) {
    return;
  }

  const bool selected = gtk_check_button_get_active(button);
  if (!state->m_selection.setDraftSelected(context->m_identity, selected)) {
    state->m_rendering = true;
    gtk_check_button_set_active(button, false);
    state->m_rendering = false;
    setStatus(state, "This device cannot be added to the saved selection.");

    return;
  }

  refreshView(state, gtk_check_button_get_active(
                         GTK_CHECK_BUTTON(state->m_selected_only_filter)));
}

void selectedOnlyFilterToggled(GtkCheckButton *button, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  const bool selectedOnly = gtk_check_button_get_active(button);
  if (state->m_settings != nullptr &&
      !g_settings_set_boolean(state->m_settings, "selected-only",
                              selectedOnly)) {
    g_warning("Could not persist the Selected only filter preference");
  }

  refreshView(state, true);
}

void sortDevices(AppState *state, const dmn::gui::Dmn_DeviceSortColumn column) {
  const bool ascending = state->m_selection.sortColumn() == column
                             ? !state->m_selection.sortAscending()
                             : true;
  state->m_selection.sortDevices(column, ascending);
  refreshView(state);
}

void nameSortClicked(GtkButton *, gpointer userData) {
  sortDevices(static_cast<AppState *>(userData),
              dmn::gui::Dmn_DeviceSortColumn::kName);
}

void statusSortClicked(GtkButton *, gpointer userData) {
  sortDevices(static_cast<AppState *>(userData),
              dmn::gui::Dmn_DeviceSortColumn::kStatus);
}

void rowContextDestroy(gpointer data, GClosure *) {
  delete static_cast<RowContext *>(data);
}

void viewDeviceClicked(GtkButton *, gpointer userData) {
  auto *context = static_cast<RowContext *>(userData);
  auto *state = context->m_state;
  state->m_viewed_device = context->m_identity;
  gtk_check_button_set_active(
      GTK_CHECK_BUTTON(state->m_feed_all_devices_filter), FALSE);
  state->m_showing_device_page = true;
  updateSplitLayout(state);
  refreshView(state);
}

auto isDisconnectPending(const AppState *state,
                         const dmn::gui::Dmn_DeviceIdentity &identity,
                         const MonitorSession *session) -> bool;

void connectionClicked(GtkButton *, gpointer userData) {
  auto *context = static_cast<RowContext *>(userData);
  auto *state = context->m_state;
  if (state->m_connecting_device.has_value() &&
      *state->m_connecting_device == context->m_identity) {
    requestConnectionCancel(state, context->m_identity);

    return;
  }

  auto *session = findMonitorSession(state, context->m_identity);
  if (session != nullptr && session->m_disconnect_after_stop) {
    session->m_disconnect_after_stop = false;
    setStatus(state, "Disconnect canceled; this device will remain connected.");
    refreshView(state, false);

    return;
  }

  if (state->m_disconnecting_device.has_value() &&
      *state->m_disconnecting_device == context->m_identity) {
    state->m_reconnect_after_disconnect = context->m_identity;
    setStatus(state, "Will reconnect after the current disconnect completes.");
    refreshView(state, false);

    return;
  }

  const auto device = std::find_if(
      state->m_selection.devices().begin(), state->m_selection.devices().end(),
      [context](const auto &entry) {
        return entry.m_identity == context->m_identity;
      });
  if (device == state->m_selection.devices().end() || !device->m_present) {
    setStatus(state, "Refresh the device inventory before connecting.");

    return;
  }

  if (!isSelected(state->m_selection.savedSelection(), context->m_identity)) {
    setStatus(state, "Save this device before connecting or monitoring it.");

    return;
  }

  if (!device->m_paired.value_or(false)) {
    setStatus(state,
              "Pair this device in Bluetooth Settings before connecting.");

    return;
  }

  if (session != nullptr && session->m_busy) {
    if (!device->m_connected.value_or(false) && session->m_monitor != nullptr &&
        session->m_stopping) {
      session->m_connect_after_stop = true;
      setStatus(state, "Will connect after the monitor stops.");
      refreshView(state, false);

      return;
    }

    if (!device->m_connected.value_or(false) &&
        session->m_cancel_start != nullptr &&
        !session->m_start_cancel_requested) {
      if (!setMonitoringIntent(state, context->m_identity, false)) {
        setStatus(state, "Could not clear the monitoring preference; "
                         "monitor startup was not canceled.");

        return;
      }

      session->m_monitor_restart_required = false;
      session->m_connect_after_stop = true;
      session->m_start_cancel_requested = true;
      session->m_stopping = true;
      session->m_cancel_start->store(true, std::memory_order_release);
      state->m_monitor_controller.resetRetries(context->m_identity);
      session->m_status = "Canceling disconnected monitor start";
      setStatus(state, "Canceling monitor startup before connecting…");
      refreshView(state, false);

      return;
    }

    if (device->m_connected.value_or(false) && session->m_monitor != nullptr) {
      if (!setMonitoringIntent(state, context->m_identity, false)) {
        setStatus(state, "Could not clear the monitoring preference; "
                         "disconnect was not started.");

        return;
      }

      session->m_monitor_restart_required = false;
      session->m_disconnect_after_stop = true;
      setStatus(state, "Stopping monitoring before disconnecting…");
      refreshView(state, false);

      return;
    }

    if (device->m_connected.value_or(false) &&
        session->m_cancel_start != nullptr &&
        !session->m_start_cancel_requested) {
      if (!setMonitoringIntent(state, context->m_identity, false)) {
        setStatus(state, "Could not clear the monitoring preference; "
                         "monitor startup was not canceled.");

        return;
      }

      session->m_monitor_restart_required = false;
      session->m_disconnect_after_stop = true;
      session->m_start_cancel_requested = true;
      session->m_stopping = true;
      session->m_cancel_start->store(true, std::memory_order_release);
      state->m_monitor_controller.resetRetries(context->m_identity);
      session->m_status = "Canceling monitor start";
      setStatus(state, "Canceling monitor startup before disconnecting…");
      refreshView(state, false);

      return;
    }

    setStatus(state, "Wait for this device's monitor operation to finish.");

    return;
  }

  if (!device->m_connected.value_or(false) && session != nullptr &&
      session->m_monitor != nullptr) {
    session->m_connect_after_stop = true;
    requestMonitorOperation(state, context->m_identity,
                            Dmn_MonitorOperation::kStop);

    return;
  }

  if (device->m_connected.value_or(false)) {
    if (isSelected(state->m_monitoring_intents, context->m_identity) &&
        !setMonitoringIntent(state, context->m_identity, false)) {
      setStatus(state, "Could not clear the monitoring preference; "
                       "disconnect was not started.");

      return;
    }

    if (session != nullptr) {
      session->m_monitor_restart_required = false;
    }

    if (session != nullptr && session->m_monitor != nullptr) {
      session->m_disconnect_after_stop = true;
      requestMonitorOperation(state, context->m_identity,
                              Dmn_MonitorOperation::kStop, true);

      return;
    }

    if (state->m_busy) {
      state->m_queued_connection_action =
          QueuedConnectionAction{context->m_identity, false};
      setStatus(state, "Disconnect queued until the current Bluetooth "
                       "operation finishes.");
      refreshView(state, false);

      return;
    }

    requestOperation(state, Dmn_Operation::kDisconnect, {},
                     context->m_identity);

    return;
  }

  if (state->m_busy) {
    state->m_queued_connection_action =
        QueuedConnectionAction{context->m_identity, true};
    setStatus(state, "Connect queued until the current Bluetooth operation "
                     "finishes.");
    refreshView(state, false);

    return;
  }

  requestOperation(state, Dmn_Operation::kConnect, {}, context->m_identity);
}

void monitoringClicked(GtkButton *, gpointer userData) {
  auto *context = static_cast<RowContext *>(userData);
  auto *state = context->m_state;
  if (state->m_close_requested) {
    setStatus(state, "The application is closing.");

    return;
  }

  const auto device = std::find_if(
      state->m_selection.devices().begin(), state->m_selection.devices().end(),
      [context](const auto &entry) {
        return entry.m_identity == context->m_identity;
      });
  if (device == state->m_selection.devices().end() || !device->m_present) {
    setStatus(state, "Refresh the device inventory before monitoring.");

    return;
  }

  if (!isSelected(state->m_selection.savedSelection(), context->m_identity)) {
    setStatus(state, "Save this device before starting its monitor.");

    return;
  }

  if (!device->m_paired.value_or(false)) {
    setStatus(state,
              "Pair this device in Bluetooth Settings before monitoring.");

    return;
  }

  if (!device->m_connected.value_or(false)) {
    setStatus(state, "Connect this device before starting its monitor.");

    return;
  }

  auto *session = findMonitorSession(state, context->m_identity);
  if (isSelected(state->m_monitoring_intents, context->m_identity)) {
    if (!setMonitoringIntent(state, context->m_identity, false)) {
      setStatus(state, "Could not clear the saved monitoring preference.");

      return;
    }

    state->m_monitor_controller.resetRetries(context->m_identity);
    if (session != nullptr) {
      session->m_monitor_restart_required = false;
      if (session->m_busy && session->m_monitor == nullptr &&
          session->m_cancel_start != nullptr &&
          !session->m_start_cancel_requested) {
        session->m_start_cancel_requested = true;
        session->m_stopping = true;
        session->m_cancel_start->store(true, std::memory_order_release);
        session->m_status = "Canceling monitor start";
        setStatus(state, "Canceling ANCS monitoring startup…");
        refreshView(state, false);

        return;
      }

      if (session->m_busy) {
        setStatus(state, "Monitoring preference cleared.");
        refreshView(state, false);

        return;
      }

      if (session->m_monitor != nullptr && !state->m_busy) {
        requestMonitorOperation(state, context->m_identity,
                                Dmn_MonitorOperation::kStop);

        return;
      }

      session->m_error.clear();
      session->m_status = "Not monitoring";
    }

    setStatus(state, "Monitoring preference cleared.");
    refreshView(state, false);

    return;
  }

  if (state->m_selection.hasUnsavedChanges()) {
    setStatus(state, "Save or discard selection changes before monitoring.");

    return;
  }

  if (!setMonitoringIntent(state, context->m_identity, true)) {
    setStatus(state, "Could not save the monitoring preference; monitoring "
                     "was not started.");

    return;
  }

  auto &retrySession =
      state->m_monitor_sessions[identityKey(context->m_identity)];
  retrySession.m_identity = context->m_identity;
  state->m_monitor_controller.resetRetries(context->m_identity);
  if (retrySession.m_busy) {
    refreshView(state, false);

    return;
  }

  if (retrySession.m_monitor != nullptr) {
    retrySession.m_monitor_restart_required = true;
    if (!state->m_busy) {
      requestMonitorOperation(state, context->m_identity,
                              Dmn_MonitorOperation::kStop);
    }
  } else if (!state->m_busy) {
    requestMonitorOperation(state, context->m_identity,
                            Dmn_MonitorOperation::kStart);
  }

  refreshView(state, false);
}

auto isSelected(const std::vector<dmn::gui::Dmn_DeviceIdentity> &selection,
                const dmn::gui::Dmn_DeviceIdentity &identity) -> bool {
  return std::find(selection.begin(), selection.end(), identity) !=
         selection.end();
}

auto setMonitoringIntent(AppState *state,
                         const dmn::gui::Dmn_DeviceIdentity &identity,
                         const bool enabled) -> bool {
  if (state->m_settings == nullptr) {
    g_warning(
        "Cannot persist monitoring intent because GSettings is unavailable");

    return false;
  }

  const auto previous = state->m_monitoring_intents;
  const auto found = std::find(state->m_monitoring_intents.begin(),
                               state->m_monitoring_intents.end(), identity);
  if (enabled && found == state->m_monitoring_intents.end()) {
    state->m_monitoring_intents.push_back(identity);
  } else if (!enabled && found != state->m_monitoring_intents.end()) {
    state->m_monitoring_intents.erase(found);
  } else {
    return true;
  }

  if (!persistMonitoringIntents(state)) {
    state->m_monitoring_intents = previous;

    return false;
  }

  return true;
}

auto persistMonitoringIntents(AppState *state) -> bool {
  if (state->m_settings == nullptr) {
    g_warning(
        "Cannot persist monitoring intent because GSettings is unavailable");

    return false;
  }

  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ss)"));
  for (const auto &intent : state->m_monitoring_intents) {
    g_variant_builder_add(&builder, "(ss)", intent.m_adapter_address.c_str(),
                          intent.m_device_address.c_str());
  }

  if (!g_settings_set_value(state->m_settings, "monitoring-devices",
                            g_variant_builder_end(&builder))) {
    g_warning("Could not persist the Bluetooth monitoring preference");

    return false;
  }

  return true;
}

auto rowActions(const AppState *state, const dmn::gui::Dmn_DeviceEntry &device,
                const MonitorSession *session)
    -> dmn::gui::Dmn_DeviceActionAvailability {
  const bool disconnectPending =
      isDisconnectPending(state, device.m_identity, session);
  auto effectiveDevice = device;
  if (disconnectPending) {
    effectiveDevice.m_connected = false;
  }

  return dmn::gui::deviceActionAvailability(
      effectiveDevice,
      {.m_saved =
           isSelected(state->m_selection.savedSelection(), device.m_identity),
       .m_selection_dirty = state->m_selection.hasUnsavedChanges(),
       .m_monitor_active = session != nullptr && session->m_monitor != nullptr,
       .m_monitor_requested =
           isSelected(state->m_monitoring_intents, device.m_identity),
       .m_monitor_busy = session != nullptr && session->m_busy,
       .m_monitor_start_pending = session != nullptr && session->m_busy &&
                                  session->m_monitor == nullptr,
       .m_monitor_cancel_requested =
           session != nullptr && session->m_start_cancel_requested,
       .m_connection_pending = state->m_connecting_device.has_value() &&
                               *state->m_connecting_device == device.m_identity,
       .m_disconnect_pending = disconnectPending,
       .m_connect_after_monitor_stop =
           session != nullptr && session->m_connect_after_stop,
       .m_operation_busy = state->m_busy,
       .m_close_requested = state->m_close_requested});
}

auto isDisconnectPending(const AppState *state,
                         const dmn::gui::Dmn_DeviceIdentity &identity,
                         const MonitorSession *session) -> bool {
  return (session != nullptr && session->m_disconnect_after_stop) ||
         (state->m_disconnecting_device.has_value() &&
          *state->m_disconnecting_device == identity);
}

auto eventDescription(const dmn::Dmn_AncsEvent &event) -> std::string {
  std::string description{eventKindName(event)};
  if (event.state) {
    description += " · ";
    description += monitorStateName(*event.state);
  }

  if (event.notification) {
    const auto &notification = *event.notification;
    if (notification.kind == dmn::Dmn_AncsNotificationKind::Removed) {
      description += " · UID ";
      description += std::to_string(notification.uid);

      return description;
    }

    if (notification.app_identifier) {
      description += " · ";
      description += *notification.app_identifier;
    }

    if (notification.title) {
      description += " · ";
      description += *notification.title;
    }

    if (notification.subtitle) {
      description += " · ";
      description += *notification.subtitle;
    }

    if (notification.message) {
      description += " · ";
      description += *notification.message;
    }

    if (notification.kind ==
        dmn::Dmn_AncsNotificationKind::AttributesUnavailable) {
      description += " · Notification attributes are unavailable";
    }
  }

  if (event.error) {
    description += " · ";
    description += event.error->message;
  }

  return description;
}

auto eventReceivedTime(const std::chrono::system_clock::time_point received)
    -> std::string {
  const auto receivedTime = std::chrono::system_clock::to_time_t(received);
  std::tm localTime{};
  localtime_r(&receivedTime, &localTime);
  std::ostringstream output;
  output << std::put_time(&localTime, "%H:%M:%S");

  return output.str();
}

auto eventReceivedDateTime(const std::chrono::system_clock::time_point received)
    -> std::string {
  const auto receivedTime = std::chrono::system_clock::to_time_t(received);
  std::tm localTime{};
  localtime_r(&receivedTime, &localTime);
  std::ostringstream output;
  output << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S %Z");

  return output.str();
}

auto deviceNameForIdentity(const AppState *state,
                           const dmn::gui::Dmn_DeviceIdentity &identity)
    -> std::string {
  const auto device = std::find_if(
      state->m_selection.devices().begin(), state->m_selection.devices().end(),
      [&identity](const auto &entry) { return entry.m_identity == identity; });

  return device == state->m_selection.devices().end()
             ? identity.m_device_address
             : dmn::gui::deviceDisplayName(*device);
}

void updateEventFeedHeading(AppState *state) {
  if (!state->m_viewed_device) {
    return;
  }

  const bool allDevices = state->m_feed_all_devices_filter != nullptr &&
                          gtk_check_button_get_active(GTK_CHECK_BUTTON(
                              state->m_feed_all_devices_filter));
  const auto title =
      allDevices ? "Notifications · All devices"
                 : "Notifications · " +
                       deviceNameForIdentity(state, *state->m_viewed_device);
  gtk_label_set_text(GTK_LABEL(state->m_feed_heading), title.c_str());
}

auto eventCategoryName(const std::uint8_t category) -> const char * {
  switch (category) {
  case 0:
    return "Other";
  case 1:
    return "Incoming call";
  case 2:
    return "Missed call";
  case 3:
    return "Voicemail";
  case 4:
    return "Social";
  case 5:
    return "Schedule";
  case 6:
    return "Email";
  case 7:
    return "News";
  case 8:
    return "Health and fitness";
  case 9:
    return "Business and finance";
  case 10:
    return "Location";
  case 11:
    return "Entertainment";
  default:
    return "Unknown category";
  }
}

auto eventKindForFilter(const guint selected)
    -> std::optional<dmn::Dmn_AncsEvent::Kind> {
  using Kind = dmn::Dmn_AncsEvent::Kind;
  switch (selected) {
  case 1:
    return Kind::NotificationAdded;
  case 2:
    return Kind::NotificationUpdated;
  case 3:
    return Kind::NotificationRemoved;
  case 4:
    return Kind::AttributesUnavailable;
  case 5:
    return Kind::StateChanged;
  case 6:
    return Kind::Error;
  default:
    return std::nullopt;
  }
}

auto eventCategoryForFilter(const guint selected)
    -> std::optional<std::uint8_t> {
  if (selected == 0 || selected > 12) {
    return std::nullopt;
  }

  return static_cast<std::uint8_t>(selected - 1);
}

auto eventSequenceFromItem(gpointer item) -> std::uint64_t {
  if (!GTK_IS_STRING_OBJECT(item)) {
    return 0;
  }

  const auto *text = gtk_string_object_get_string(GTK_STRING_OBJECT(item));

  return text == nullptr ? 0 : g_ascii_strtoull(text, nullptr, 10);
}

auto eventMatchesGtkFilter(gpointer item, gpointer userData) -> gboolean {
  const auto sequence = eventSequenceFromItem(item);
  const auto *state = static_cast<const AppState *>(userData);
  const auto *event = state->m_event_store.eventForSequence(sequence);

  return event != nullptr &&
         dmn::gui::matchesEventFilter(
             *event, state->m_event_filters,
             deviceNameForIdentity(state, event->m_identity));
}

auto compareEventSequence(gconstpointer first, gconstpointer second,
                          gpointer) -> gint {
  const auto firstSequence = eventSequenceFromItem(const_cast<gpointer>(first));
  const auto secondSequence =
      eventSequenceFromItem(const_cast<gpointer>(second));
  if (firstSequence > secondSequence) {
    return -1;
  }

  if (firstSequence < secondSequence) {
    return 1;
  }

  return 0;
}

auto eventSummary(const dmn::Dmn_AncsEvent &event) -> std::string {
  if (event.notification) {
    const auto &notification = *event.notification;
    if (notification.kind == dmn::Dmn_AncsNotificationKind::Removed) {
      return "Notification removed · UID " + std::to_string(notification.uid);
    }

    std::string summary;
    const auto append = [&summary](const std::optional<std::string> &value) {
      if (value && !value->empty()) {
        if (!summary.empty()) {
          summary += " · ";
        }

        summary += *value;
      }
    };
    append(notification.app_identifier);
    append(notification.title);
    append(notification.subtitle);
    append(notification.message);
    if (summary.empty()) {
      summary = "Notification attributes unavailable";
    }

    return summary;
  }

  return eventDescription(event);
}

auto eventColumnText(const AppState *state,
                     const dmn::gui::Dmn_DeviceMonitorEvent &event,
                     const guint column) -> std::string {
  switch (column) {
  case 0:
    return deviceNameForIdentity(state, event.m_identity);
  case 1:
    return eventReceivedTime(event.m_received);
  case 2: {
    std::string kind{eventKindName(event.m_event)};
    if (event.m_event.notification) {
      kind += " · ";
      kind += eventCategoryName(event.m_event.notification->category);
    }

    return kind;
  }
  default:
    return eventSummary(event.m_event);
  }
}

void setDetailLine(std::ostringstream &details, const char *name,
                   const std::optional<std::string> &value) {
  details << name << ": " << (value && !value->empty() ? *value : "Unavailable")
          << '\n';
}

void showEventDetails(GtkColumnView *, const guint position,
                      gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  auto *item =
      g_list_model_get_item(G_LIST_MODEL(state->m_event_sorted_rows), position);
  if (item == nullptr) {
    return;
  }

  const auto sequence = eventSequenceFromItem(item);
  g_object_unref(item);
  const auto *event = state->m_event_store.eventForSequence(sequence);
  if (event == nullptr) {
    return;
  }

  std::ostringstream details;
  details << "Device: " << deviceNameForIdentity(state, event->m_identity)
          << "\nAdapter: " << event->m_identity.m_adapter_address
          << "\nDevice address: " << event->m_identity.m_device_address
          << "\nReceived: " << eventReceivedDateTime(event->m_received)
          << "\nEvent: " << eventKindName(event->m_event) << '\n';
  if (event->m_event.notification) {
    const auto &notification = *event->m_event.notification;
    details << "Notification UID: " << notification.uid << '\n'
            << "Category: " << eventCategoryName(notification.category) << " ("
            << static_cast<unsigned int>(notification.category) << ")\n";
    if (notification.kind == dmn::Dmn_AncsNotificationKind::Removed) {
      details << "Notification was removed; ANCS provides no current "
                 "attributes for this event.\n";
    } else {
      setDetailLine(details, "App identifier", notification.app_identifier);
      setDetailLine(details, "Title", notification.title);
      setDetailLine(details, "Subtitle", notification.subtitle);
      setDetailLine(details, "Message", notification.message);
      setDetailLine(details, "Phone-provided date", notification.date);
      details << "Category count: "
              << static_cast<unsigned int>(notification.category_count)
              << "\nFlags: " << static_cast<unsigned int>(notification.flags)
              << '\n';
    }
  }

  if (event->m_event.state) {
    details << "Monitor state: " << monitorStateName(*event->m_event.state)
            << '\n';
  }

  if (event->m_event.error) {
    details << "Error: " << event->m_event.error->message << '\n';
  }

  auto *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Notification details");
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(state->m_window));
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 420);
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(content, 18);
  gtk_widget_set_margin_bottom(content, 18);
  gtk_widget_set_margin_start(content, 18);
  gtk_widget_set_margin_end(content, 18);
  auto *text = gtk_label_new(details.str().c_str());
  gtk_label_set_xalign(GTK_LABEL(text), 0.0F);
  gtk_label_set_yalign(GTK_LABEL(text), 0.0F);
  gtk_label_set_wrap(GTK_LABEL(text), TRUE);
  gtk_label_set_selectable(GTK_LABEL(text), TRUE);
  auto *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
  auto *close = gtk_button_new_with_label("Close");
  gtk_widget_set_halign(close, GTK_ALIGN_END);
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_destroy),
                           dialog);
  gtk_box_append(GTK_BOX(content), scroll);
  gtk_box_append(GTK_BOX(content), close);
  gtk_window_set_child(GTK_WINDOW(dialog), content);
  gtk_window_present(GTK_WINDOW(dialog));
}

auto copySelectedMessage(GtkLabel *messageLabel) -> bool {
  gint start{};
  gint end{};
  if (!gtk_label_get_selection_bounds(messageLabel, &start, &end)) {
    return false;
  }

  const auto *text = gtk_label_get_text(messageLabel);
  const auto *selectionStart = g_utf8_offset_to_pointer(text, start);
  const auto *selectionEnd = g_utf8_offset_to_pointer(text, end);
  auto *selectedText = g_strndup(selectionStart, selectionEnd - selectionStart);
  if (selectedText == nullptr || *selectedText == '\0') {
    g_free(selectedText);

    return false;
  }

  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(messageLabel)),
                         selectedText);
  g_free(selectedText);

  return true;
}

void copyMessageClicked(GtkButton *, gpointer userData) {
  auto *messageLabel = GTK_LABEL(userData);
  copySelectedMessage(messageLabel);
  auto *popover = GTK_POPOVER(
      g_object_get_data(G_OBJECT(messageLabel), "message-copy-popover"));
  gtk_popover_popdown(popover);
}

void showCopyMenu(GtkGestureClick *gesture, gint, const gdouble x,
                  const gdouble y, gpointer userData) {
  auto *messageLabel = GTK_LABEL(userData);
  auto *popover = GTK_POPOVER(
      g_object_get_data(G_OBJECT(messageLabel), "message-copy-popover"));
  auto *copyButton = gtk_widget_get_first_child(gtk_popover_get_child(popover));
  gint start{};
  gint end{};
  gtk_widget_set_sensitive(
      copyButton, gtk_label_get_selection_bounds(messageLabel, &start, &end));

  auto *cell = gtk_widget_get_parent(GTK_WIDGET(messageLabel));
  const graphene_point_t labelPoint{static_cast<float>(x),
                                    static_cast<float>(y)};
  graphene_point_t cellPoint{};
  if (gtk_widget_compute_point(GTK_WIDGET(messageLabel), cell, &labelPoint,
                               &cellPoint)) {
    const GdkRectangle position{static_cast<int>(cellPoint.x),
                                static_cast<int>(cellPoint.y), 1, 1};
    gtk_popover_set_pointing_to(popover, &position);
  }

  gtk_popover_popup(popover);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

struct EventColumnContext {
  AppState *m_state{};
  guint m_column{};
};

gboolean showEventCellTooltip(GtkWidget *widget, gint, gint,
                              gboolean keyboardTooltip, GtkTooltip *tooltip,
                              gpointer) {
  if (keyboardTooltip) {
    return FALSE;
  }

  const auto *text = gtk_label_get_text(GTK_LABEL(widget));
  if (text == nullptr || *text == '\0') {
    return FALSE;
  }

  auto *tooltipLabel = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(tooltipLabel), 0.0F);
  gtk_label_set_yalign(GTK_LABEL(tooltipLabel), 0.0F);
  gtk_label_set_wrap(GTK_LABEL(tooltipLabel), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(tooltipLabel), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_size_request(
      tooltipLabel, std::max(1, gtk_widget_get_width(widget) - 32), -1);
  gtk_tooltip_set_custom(tooltip, tooltipLabel);

  return TRUE;
}

void eventCellSetup(GtkSignalListItemFactory *, GtkListItem *item,
                    gpointer userData) {
  const auto *context = static_cast<const EventColumnContext *>(userData);
  auto *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  auto *label = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  gtk_label_set_yalign(GTK_LABEL(label), 0.0F);
  gtk_label_set_wrap(GTK_LABEL(label), context->m_column == 3);
  gtk_label_set_selectable(GTK_LABEL(label), context->m_column == 3);
  if (context->m_column == 3) {
    gtk_widget_set_has_tooltip(label, TRUE);
    g_signal_connect(label, "query-tooltip", G_CALLBACK(showEventCellTooltip),
                     nullptr);
  }

  gtk_widget_set_margin_top(label, 6);
  gtk_widget_set_margin_bottom(label, 6);
  gtk_widget_set_margin_start(label, 8);
  gtk_widget_set_margin_end(label, 8);

  GtkWidget *popover{};
  if (context->m_column == 3) {
    popover = gtk_popover_new();
    auto *menu = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    auto *copy = gtk_button_new_with_label("Copy");
    auto *accelerator = gtk_label_new("Ctrl+C");
    gtk_widget_add_css_class(copy, "flat");
    gtk_widget_add_css_class(accelerator, "dim-label");
    gtk_box_append(GTK_BOX(menu), copy);
    gtk_box_append(GTK_BOX(menu), accelerator);
    gtk_popover_set_child(GTK_POPOVER(popover), menu);
    g_object_set_data(G_OBJECT(label), "message-copy-popover", popover);
    g_signal_connect(copy, "clicked", G_CALLBACK(copyMessageClicked), label);

    auto *contextClick = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(contextClick),
                                  GDK_BUTTON_SECONDARY);
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(contextClick), GTK_PHASE_CAPTURE);
    g_signal_connect(contextClick, "pressed", G_CALLBACK(showCopyMenu), label);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(contextClick));
  }

  gtk_box_append(GTK_BOX(cell), label);
  if (popover != nullptr) {
    gtk_widget_set_parent(popover, cell);
  }

  gtk_list_item_set_child(item, cell);
}

void eventCellBind(GtkSignalListItemFactory *, GtkListItem *item,
                   gpointer userData) {
  const auto *context = static_cast<const EventColumnContext *>(userData);
  auto *row = gtk_list_item_get_item(item);
  const auto sequence = eventSequenceFromItem(row);
  const auto *event =
      context->m_state->m_event_store.eventForSequence(sequence);
  if (event == nullptr) {
    return;
  }

  const auto text =
      eventColumnText(context->m_state, *event, context->m_column);
  auto *cell = gtk_list_item_get_child(item);
  gtk_label_set_text(GTK_LABEL(gtk_widget_get_first_child(cell)), text.c_str());
}

void eventFilterChanged(GObject *, GParamSpec *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  if (state->m_updating_event_filters) {
    return;
  }

  state->m_event_filters.m_search =
      gtk_editable_get_text(GTK_EDITABLE(state->m_feed_search));
  state->m_event_filters.m_kind = eventKindForFilter(
      gtk_drop_down_get_selected(GTK_DROP_DOWN(state->m_feed_kind_filter)));
  state->m_event_filters.m_category = eventCategoryForFilter(
      gtk_drop_down_get_selected(GTK_DROP_DOWN(state->m_feed_category_filter)));
  state->m_event_filters.m_identity =
      gtk_check_button_get_active(
          GTK_CHECK_BUTTON(state->m_feed_all_devices_filter))
          ? std::nullopt
          : state->m_viewed_device;
  updateEventFeedHeading(state);

  gtk_filter_changed(GTK_FILTER(state->m_event_filter),
                     GTK_FILTER_CHANGE_DIFFERENT);
  rebuildEventFeed(state);
}

void eventSearchChanged(GtkSearchEntry *, gpointer userData) {
  eventFilterChanged(nullptr, nullptr, userData);
}

void eventFilterToggled(GtkCheckButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  eventFilterChanged(nullptr, nullptr, state);
  refreshView(state, false);
}

void clearEventFiltersClicked(GtkButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  state->m_updating_event_filters = true;
  gtk_editable_set_text(GTK_EDITABLE(state->m_feed_search), "");
  gtk_drop_down_set_selected(GTK_DROP_DOWN(state->m_feed_kind_filter), 0);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(state->m_feed_category_filter), 0);
  gtk_check_button_set_active(
      GTK_CHECK_BUTTON(state->m_feed_all_devices_filter), FALSE);
  state->m_updating_event_filters = false;
  eventFilterChanged(nullptr, nullptr, state);
}

void clearNotificationsClicked(GtkButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  withdrawDesktopAlerts(state);
  if (state->m_event_store.clear() == 0) {
    return;
  }

  ++state->m_event_feed_revision;
  state->m_event_feed_rendered = false;
  rebuildEventFeed(state);
  setStatus(state, "Session notifications cleared.");
}

gboolean scrollEventFeedToTop(gpointer userData) {
  auto *adjustment = GTK_ADJUSTMENT(userData);
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_lower(adjustment));

  return G_SOURCE_REMOVE;
}

void updateEventFeedStatus(AppState *state) {
  const auto retainedCount = state->m_event_store.size();
  const auto visibleCount =
      g_list_model_get_n_items(G_LIST_MODEL(state->m_event_sorted_rows));
  const auto evictedCount = state->m_event_store.evictedCount();
  const auto evictionNotice =
      evictedCount == 0
          ? std::string{}
          : std::to_string(evictedCount) +
                " older event(s) were removed to keep the session feed within "
                "its 1,000-event limit.";
  gtk_label_set_text(GTK_LABEL(state->m_feed_notice), evictionNotice.c_str());
  gtk_widget_set_visible(state->m_feed_notice, evictedCount != 0);
  gtk_widget_set_sensitive(state->m_feed_clear_notifications,
                           retainedCount != 0);

  if (visibleCount != 0) {
    gtk_widget_set_visible(state->m_feed_empty, FALSE);
    gtk_widget_set_visible(state->m_feed_view, TRUE);

    return;
  }

  std::string emptyText;
  if (retainedCount != 0) {
    emptyText = state->m_event_filters.m_search.empty() &&
                        !state->m_event_filters.m_kind &&
                        !state->m_event_filters.m_category &&
                        state->m_event_filters.m_identity
                    ? "No notifications have been received for this device "
                      "during this app session."
                    : "No notifications match these filters.";
  } else {
    const auto *session =
        state->m_viewed_device == std::nullopt
            ? nullptr
            : findMonitorSession(state, *state->m_viewed_device);
    emptyText =
        session != nullptr &&
                session->m_status == "Ready · waiting for new notifications"
            ? "ANCS is ready. Only new notifications received while this "
              "monitor is running appear here. iOS may ask for access after a "
              "new notification arrives."
            : "No notifications have been received in this app session.";
  }

  gtk_label_set_text(GTK_LABEL(state->m_feed_empty), emptyText.c_str());
  gtk_widget_set_visible(state->m_feed_view, FALSE);
  gtk_widget_set_visible(state->m_feed_empty, TRUE);
}

void createEventFeedModel(AppState *state) {
  state->m_event_rows = g_list_store_new(GTK_TYPE_STRING_OBJECT);
  state->m_event_filter =
      gtk_custom_filter_new(eventMatchesGtkFilter, state, nullptr);
  state->m_event_filtered_rows = gtk_filter_list_model_new(
      G_LIST_MODEL(g_object_ref(state->m_event_rows)),
      GTK_FILTER(g_object_ref(state->m_event_filter)));
  state->m_event_sorter =
      gtk_custom_sorter_new(compareEventSequence, state, nullptr);
  state->m_event_sorted_rows = gtk_sort_list_model_new(
      G_LIST_MODEL(g_object_ref(state->m_event_filtered_rows)),
      GTK_SORTER(g_object_ref(state->m_event_sorter)));
  state->m_event_selection = gtk_single_selection_new(
      G_LIST_MODEL(g_object_ref(state->m_event_sorted_rows)));
  gtk_single_selection_set_autoselect(state->m_event_selection, FALSE);
  gtk_single_selection_set_can_unselect(state->m_event_selection, TRUE);
  state->m_feed_view = gtk_column_view_new(
      GTK_SELECTION_MODEL(g_object_ref(state->m_event_selection)));
  gtk_column_view_set_show_column_separators(
      GTK_COLUMN_VIEW(state->m_feed_view), TRUE);
  gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(state->m_feed_view),
                                          TRUE);
  gtk_widget_set_hexpand(state->m_feed_view, TRUE);
  gtk_widget_set_vexpand(state->m_feed_view, TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->m_feed_view),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Live notification events", -1);

  static constexpr const char *kTitles[]{"Device", "Received",
                                         "Type / category", "Notification"};
  for (guint columnIndex = 0; columnIndex < G_N_ELEMENTS(kTitles);
       ++columnIndex) {
    auto *factory = GTK_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
    auto *setupContext = new EventColumnContext{state, columnIndex};
    auto *bindContext = new EventColumnContext{state, columnIndex};
    g_signal_connect_data(
        factory, "setup", G_CALLBACK(eventCellSetup), setupContext,
        [](gpointer data, GClosure *) {
          delete static_cast<EventColumnContext *>(data);
        },
        G_CONNECT_DEFAULT);
    g_signal_connect_data(
        factory, "bind", G_CALLBACK(eventCellBind), bindContext,
        [](gpointer data, GClosure *) {
          delete static_cast<EventColumnContext *>(data);
        },
        G_CONNECT_DEFAULT);
    auto *column = gtk_column_view_column_new(kTitles[columnIndex], factory);
    gtk_column_view_column_set_resizable(column, TRUE);
    gtk_column_view_column_set_expand(column, columnIndex == 3);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(state->m_feed_view), column);
    g_object_unref(column);
  }

  g_signal_connect(state->m_feed_view, "activate", G_CALLBACK(showEventDetails),
                   state);
}

void rebuildEventFeed(AppState *state) {
  if (!state->m_viewed_device) {
    state->m_event_feed_rendered = true;
    state->m_rendered_feed_device.reset();
    state->m_rendered_event_feed_revision = state->m_event_feed_revision;
    gtk_widget_set_visible(state->m_feed_heading, FALSE);
    gtk_widget_set_visible(state->m_feed_controls, FALSE);
    gtk_widget_set_visible(state->m_feed_scroll, FALSE);

    return;
  }

  const auto nextIdentity = gtk_check_button_get_active(GTK_CHECK_BUTTON(
                                state->m_feed_all_devices_filter))
                                ? std::optional<dmn::gui::Dmn_DeviceIdentity>{}
                                : state->m_viewed_device;
  updateEventFeedHeading(state);
  const bool sourceChanged =
      !state->m_event_feed_rendered ||
      state->m_rendered_feed_device != state->m_viewed_device ||
      state->m_rendered_event_feed_revision != state->m_event_feed_revision;
  const bool identityChanged =
      state->m_event_filters.m_identity != nextIdentity;
  state->m_event_filters.m_identity = nextIdentity;
  if (sourceChanged) {
    g_list_store_remove_all(state->m_event_rows);
    for (const auto *event : state->m_event_store.events()) {
      const auto sequence = std::to_string(event->m_sequence);
      auto *row = gtk_string_object_new(sequence.c_str());
      g_list_store_append(state->m_event_rows, row);
      g_object_unref(row);
    }

    state->m_event_feed_rendered = true;
    state->m_rendered_feed_device = state->m_viewed_device;
    state->m_rendered_event_feed_revision = state->m_event_feed_revision;
  }

  if (sourceChanged || identityChanged) {
    gtk_filter_changed(GTK_FILTER(state->m_event_filter),
                       GTK_FILTER_CHANGE_DIFFERENT);
  }

  gtk_widget_set_visible(state->m_feed_heading, TRUE);
  gtk_widget_set_visible(state->m_feed_controls, TRUE);
  gtk_widget_set_visible(state->m_feed_scroll, TRUE);
  updateEventFeedStatus(state);

  if (!sourceChanged) {
    return;
  }

  auto *adjustment = gtk_scrolled_window_get_vadjustment(
      GTK_SCROLLED_WINDOW(state->m_feed_scroll));
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scrollEventFeedToTop,
                  g_object_ref(adjustment),
                  [](gpointer data) { g_object_unref(data); });
}

auto visibleDevices(AppState *state)
    -> std::vector<const dmn::gui::Dmn_DeviceEntry *> {
  std::vector<const dmn::gui::Dmn_DeviceEntry *> visible;
  for (const auto &device : state->m_selection.devices()) {
    if (gtk_check_button_get_active(
            GTK_CHECK_BUTTON(state->m_selected_only_filter)) &&
        !isSelected(state->m_selection.draftSelection(), device.m_identity)) {
      continue;
    }

    visible.push_back(&device);
  }

  return visible;
}

void rebuildDeviceList(AppState *state) {
  state->m_rendering = true;
  gtk_list_box_remove_all(GTK_LIST_BOX(state->m_device_list));

  if (state->m_selection.devices().empty()) {
    const char *message = "No known devices. Scan to find a nearby phone.";
    if (!state->m_inventory_error.empty()) {
      message = "Bluetooth devices could not be loaded. Use Refresh devices to "
                "retry.";
    } else if (!state->m_inventory_loaded) {
      message = "Loading Bluetooth devices…";
    }

    auto *empty = gtk_label_new(message);
    gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
    gtk_widget_set_margin_top(empty, 24);
    gtk_widget_set_margin_bottom(empty, 24);
    gtk_widget_set_margin_start(empty, 16);
    gtk_widget_set_margin_end(empty, 16);
    gtk_list_box_append(GTK_LIST_BOX(state->m_device_list), empty);
    state->m_rendering = false;

    return;
  }

  const auto devices = visibleDevices(state);
  if (devices.empty()) {
    auto *empty = gtk_label_new(
        "No selected devices. Select devices to include them in this view.");
    gtk_label_set_wrap(GTK_LABEL(empty), TRUE);
    gtk_widget_set_margin_top(empty, 24);
    gtk_widget_set_margin_bottom(empty, 24);
    gtk_widget_set_margin_start(empty, 16);
    gtk_widget_set_margin_end(empty, 16);
    gtk_list_box_append(GTK_LIST_BOX(state->m_device_list), empty);
    state->m_rendering = false;

    return;
  }

  for (const auto *deviceEntry : devices) {
    const auto &device = *deviceEntry;
    auto *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(row, 10);
    gtk_widget_set_margin_bottom(row, 10);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);

    const auto title = dmn::gui::deviceDisplayName(device);
    auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const bool monitorRequested =
        isSelected(state->m_monitoring_intents, device.m_identity);
    auto *check = gtk_check_button_new();
    const auto checkLabel = "Select " + title;
    gtk_accessible_update_property(GTK_ACCESSIBLE(check),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   checkLabel.c_str(), -1);
    const bool selected =
        isSelected(state->m_selection.draftSelection(), device.m_identity);
    const bool saved =
        isSelected(state->m_selection.savedSelection(), device.m_identity);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), selected);
    gtk_widget_set_sensitive(check, device.m_present || selected || saved);
    gtk_widget_set_tooltip_text(
        check, "Select this device in the current selection draft");

    auto *viewButton = gtk_button_new_with_label(title.c_str());
    gtk_widget_add_css_class(viewButton, "flat");
    gtk_widget_set_hexpand(viewButton, TRUE);
    gtk_widget_set_halign(viewButton, GTK_ALIGN_START);

    gtk_box_append(GTK_BOX(header), check);
    gtk_box_append(GTK_BOX(header), viewButton);

    auto facts = dmn::gui::deviceDisplayStatus(device);
    auto *session = findMonitorSession(state, device.m_identity);
    const bool connectionPending =
        state->m_connecting_device.has_value() &&
        *state->m_connecting_device == device.m_identity;
    const bool disconnectPending =
        isDisconnectPending(state, device.m_identity, session);
    const bool connected =
        device.m_connected.value_or(false) && !disconnectPending;
    const auto actions = rowActions(state, device, session);
    const bool monitorActionEnabled =
        actions.m_can_stop_monitoring || actions.m_can_start_monitoring;
    if (connectionPending) {
      facts += state->m_connection_cancel_requested ? " · Canceling connection"
                                                    : " · Connecting…";
    }

    if (session != nullptr && monitorActionEnabled) {
      facts += " · " + session->m_status;
    }

    auto *detail = gtk_label_new(facts.c_str());
    gtk_widget_set_halign(detail, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(detail), TRUE);
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0F);
    gtk_widget_add_css_class(detail, "dim-label");

    auto *rowActionsBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(rowActionsBox, GTK_ALIGN_END);
    auto *connectionButton = gtk_button_new_with_label(
        connected || connectionPending ? "Disconnect" : "Connect");
    const auto connectionLabel =
        std::string{connected || connectionPending ? "Disconnect "
                                                   : "Connect "} +
        title;
    gtk_accessible_update_property(GTK_ACCESSIBLE(connectionButton),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   connectionLabel.c_str(), -1);
    const bool monitorStarting = session != nullptr && session->m_busy &&
                                 !session->m_stopping &&
                                 !session->m_start_cancel_requested;
    const bool showStopMonitoring = monitorStarting || monitorRequested;
    gtk_widget_set_sensitive(connectionButton,
                             connectionPending ? actions.m_can_disconnect
                             : connected       ? actions.m_can_disconnect
                                               : actions.m_can_connect);
    if (connectionPending) {
      gtk_widget_set_tooltip_text(connectionButton,
                                  "Disconnect to cancel this connection "
                                  "attempt");
    }

    if (!device.m_paired.value_or(false)) {
      gtk_widget_set_tooltip_text(
          connectionButton, "Pair this device in Bluetooth Settings first");
    }

    auto *monitorButton = gtk_button_new_with_label(
        showStopMonitoring ? "Stop monitoring" : "Start monitoring");
    const auto monitorLabel =
        std::string{showStopMonitoring ? "Stop monitoring "
                                       : "Start monitoring "} +
        title;
    gtk_accessible_update_property(GTK_ACCESSIBLE(monitorButton),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   monitorLabel.c_str(), -1);
    if (monitorStarting && session->m_monitor == nullptr) {
      gtk_widget_set_tooltip_text(monitorButton,
                                  "Cancel startup and clear the saved "
                                  "monitoring preference");
    } else if (monitorRequested && !disconnectPending) {
      gtk_widget_set_tooltip_text(
          monitorButton, "Stop monitoring and clear the saved monitoring "
                         "preference");
    } else if (!device.m_connected.value_or(false)) {
      gtk_widget_set_tooltip_text(
          monitorButton,
          "Connect this device and wait for Bluetooth to report Connected "
          "before starting monitoring");
    }

    gtk_widget_set_sensitive(monitorButton, actions.m_can_stop_monitoring ||
                                                actions.m_can_start_monitoring);

    gtk_box_append(GTK_BOX(rowActionsBox), connectionButton);
    gtk_box_append(GTK_BOX(rowActionsBox), monitorButton);
    gtk_box_append(GTK_BOX(row), header);
    gtk_box_append(GTK_BOX(row), detail);
    gtk_box_append(GTK_BOX(row), rowActionsBox);
    gtk_list_box_append(GTK_LIST_BOX(state->m_device_list), row);

    auto *selectionContext = new RowContext{state, device.m_identity};
    g_signal_connect_data(check, "toggled", G_CALLBACK(rowToggled),
                          selectionContext, rowContextDestroy,
                          G_CONNECT_DEFAULT);
    auto *viewContext = new RowContext{state, device.m_identity};
    g_signal_connect_data(viewButton, "clicked", G_CALLBACK(viewDeviceClicked),
                          viewContext, rowContextDestroy, G_CONNECT_DEFAULT);
    auto *connectionContext = new RowContext{state, device.m_identity};
    g_signal_connect_data(connectionButton, "clicked",
                          G_CALLBACK(connectionClicked), connectionContext,
                          rowContextDestroy, G_CONNECT_DEFAULT);
    auto *monitorContext = new RowContext{state, device.m_identity};
    g_signal_connect_data(monitorButton, "clicked",
                          G_CALLBACK(monitoringClicked), monitorContext,
                          rowContextDestroy, G_CONNECT_DEFAULT);
  }

  state->m_rendering = false;
}

void synchronizeDeviceListSelection(AppState *state) {
  const auto devices = visibleDevices(state);
  auto *list = GTK_LIST_BOX(state->m_device_list);
  guint rowCount = 0;
  for (auto *child = gtk_widget_get_first_child(GTK_WIDGET(list));
       child != nullptr; child = gtk_widget_get_next_sibling(child)) {
    ++rowCount;
  }

  if (rowCount != devices.size()) {
    rebuildDeviceList(state);

    return;
  }

  state->m_rendering = true;
  for (guint index = 0; index < rowCount; ++index) {
    auto *row = gtk_list_box_get_row_at_index(list, static_cast<int>(index));
    auto *content = gtk_list_box_row_get_child(row);
    auto *header = gtk_widget_get_first_child(content);
    auto *check = gtk_widget_get_first_child(header);
    auto *viewButton = gtk_widget_get_next_sibling(check);
    auto *detail = gtk_widget_get_next_sibling(header);
    auto *rowActionsBox = gtk_widget_get_next_sibling(detail);
    auto *connectionButton = gtk_widget_get_first_child(rowActionsBox);
    auto *monitorButton = gtk_widget_get_next_sibling(connectionButton);
    const auto &device = *devices[index];
    const auto title = dmn::gui::deviceDisplayName(device);
    auto *session = findMonitorSession(state, device.m_identity);
    const bool selected =
        isSelected(state->m_selection.draftSelection(), device.m_identity);
    const bool saved =
        isSelected(state->m_selection.savedSelection(), device.m_identity);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), selected);
    gtk_widget_set_sensitive(GTK_WIDGET(check),
                             device.m_present || selected || saved);
    const auto checkLabel = "Select " + title;
    gtk_accessible_update_property(GTK_ACCESSIBLE(check),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   checkLabel.c_str(), -1);
    gtk_button_set_label(GTK_BUTTON(viewButton), title.c_str());
    auto facts = dmn::gui::deviceDisplayStatus(device);
    const bool connectionPending =
        state->m_connecting_device.has_value() &&
        *state->m_connecting_device == device.m_identity;
    const bool disconnectPending =
        isDisconnectPending(state, device.m_identity, session);
    const bool connected =
        device.m_connected.value_or(false) && !disconnectPending;
    const auto actions = rowActions(state, device, session);
    const bool monitorActionEnabled =
        actions.m_can_stop_monitoring || actions.m_can_start_monitoring;
    if (connectionPending) {
      facts += state->m_connection_cancel_requested ? " · Canceling connection"
                                                    : " · Connecting…";
    }

    if (session != nullptr && monitorActionEnabled) {
      facts += " · " + session->m_status;
    }

    gtk_label_set_text(GTK_LABEL(detail), facts.c_str());
    gtk_button_set_label(GTK_BUTTON(connectionButton),
                         connected || connectionPending ? "Disconnect"
                                                        : "Connect");
    const auto connectionLabel =
        std::string{connected || connectionPending ? "Disconnect "
                                                   : "Connect "} +
        title;
    gtk_accessible_update_property(GTK_ACCESSIBLE(connectionButton),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   connectionLabel.c_str(), -1);
    const bool monitorStarting = session != nullptr && session->m_busy &&
                                 !session->m_stopping &&
                                 !session->m_start_cancel_requested;
    const bool monitorRequested =
        isSelected(state->m_monitoring_intents, device.m_identity);
    const bool showStopMonitoring = monitorStarting || monitorRequested;
    gtk_widget_set_sensitive(connectionButton,
                             connectionPending ? actions.m_can_disconnect
                             : connected       ? actions.m_can_disconnect
                                               : actions.m_can_connect);
    if (connectionPending) {
      gtk_widget_set_tooltip_text(connectionButton,
                                  "Disconnect to cancel this connection "
                                  "attempt");
    } else if (!device.m_paired.value_or(false)) {
      gtk_widget_set_tooltip_text(connectionButton,
                                  "Pair this device in Bluetooth Settings "
                                  "first");
    } else {
      gtk_widget_set_tooltip_text(connectionButton, nullptr);
    }

    gtk_button_set_label(GTK_BUTTON(monitorButton), showStopMonitoring
                                                        ? "Stop monitoring"
                                                        : "Start monitoring");
    const auto monitorLabel =
        std::string{showStopMonitoring ? "Stop monitoring "
                                       : "Start monitoring "} +
        title;
    gtk_accessible_update_property(GTK_ACCESSIBLE(monitorButton),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   monitorLabel.c_str(), -1);
    if (monitorStarting && session->m_monitor == nullptr) {
      gtk_widget_set_tooltip_text(monitorButton,
                                  "Cancel startup and clear the saved "
                                  "monitoring preference");
    } else if (monitorRequested && !disconnectPending) {
      gtk_widget_set_tooltip_text(
          monitorButton,
          "Stop monitoring and clear the saved monitoring preference");
    } else {
      gtk_widget_set_tooltip_text(
          monitorButton,
          connected ? nullptr
                    : "Connect this device and wait for Bluetooth to report "
                      "Connected before starting monitoring");
    }

    gtk_widget_set_sensitive(monitorButton, actions.m_can_stop_monitoring ||
                                                actions.m_can_start_monitoring);
  }

  state->m_rendering = false;
}

void refreshView(AppState *state, const bool rebuildDevices) {
  if (rebuildDevices) {
    rebuildDeviceList(state);
  } else {
    // Keeping existing rows avoids resetting the scroller after selection
    // edits.
    synchronizeDeviceListSelection(state);
  }

  updateButtonState(state);
  const auto sortIndicator = state->m_selection.sortAscending() ? " ↑" : " ↓";
  const bool nameSort =
      state->m_selection.sortColumn() == dmn::gui::Dmn_DeviceSortColumn::kName;
  const std::string nameLabel =
      std::string{"Name"} + (nameSort ? sortIndicator : "");
  const std::string statusLabel =
      std::string{"Status"} + (!nameSort ? sortIndicator : "");
  gtk_button_set_label(GTK_BUTTON(state->m_name_sort_button),
                       nameLabel.c_str());
  gtk_button_set_label(GTK_BUTTON(state->m_status_sort_button),
                       statusLabel.c_str());
  gtk_widget_set_tooltip_text(state->m_name_sort_button,
                              nameSort ? (state->m_selection.sortAscending()
                                              ? "Sort by name ascending"
                                              : "Sort by name descending")
                                       : "Sort by name ascending");
  gtk_widget_set_tooltip_text(state->m_status_sort_button,
                              !nameSort ? (state->m_selection.sortAscending()
                                               ? "Sort by status ascending"
                                               : "Sort by status descending")
                                        : "Sort by status ascending");

  const auto selected = state->m_selection.savedSelection();
  if (!state->m_inventory_error.empty()) {
    adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                              "Bluetooth inventory unavailable");
    adw_status_page_set_description(ADW_STATUS_PAGE(state->m_details_page),
                                    state->m_inventory_error.c_str());
    rebuildEventFeed(state);

    return;
  }

  if (!state->m_viewed_device) {
    adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                              "Choose a Bluetooth device");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(state->m_details_page),
        selected.empty()
            ? "Choose a device on the left to view its live status and "
              "notifications. Save devices before starting their monitors."
            : "Choose a device on the left to view its live status and "
              "notifications. Saved devices are not connected automatically.");
    rebuildEventFeed(state);

    return;
  }

  if (state->m_feed_all_devices_filter != nullptr &&
      gtk_check_button_get_active(
          GTK_CHECK_BUTTON(state->m_feed_all_devices_filter))) {
    adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                              "All devices");
    adw_status_page_set_description(ADW_STATUS_PAGE(state->m_details_page), "");
    rebuildEventFeed(state);

    return;
  }

  const auto device = std::find_if(
      state->m_selection.devices().begin(), state->m_selection.devices().end(),
      [state](const auto &entry) {
        return entry.m_identity == *state->m_viewed_device;
      });
  const auto deviceName = device == state->m_selection.devices().end()
                              ? state->m_viewed_device->m_device_address
                              : dmn::gui::deviceDisplayName(*device);
  adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                            deviceName.c_str());
  auto description =
      device == state->m_selection.devices().end()
          ? "Device is not present in the latest Bluetooth inventory."
          : dmn::gui::deviceDisplayStatus(*device);
  if (isSelected(selected, *state->m_viewed_device)) {
    description += " · Saved";
  } else {
    description += " · Not saved";
  }

  const auto *session = findMonitorSession(state, *state->m_viewed_device);
  if (session != nullptr && !session->m_error.empty()) {
    description += " · ";
    description += session->m_error;
  } else if (isSelected(state->m_monitoring_intents, *state->m_viewed_device) &&
             (session == nullptr || session->m_monitor == nullptr)) {
    description += " · Monitoring will start when this saved device is "
                   "connected.";
  } else if (device != state->m_selection.devices().end() &&
             device->m_paired.value_or(false) &&
             device->m_connected.value_or(false) &&
             (session == nullptr || session->m_monitor == nullptr)) {
    description +=
        " · Start monitoring to receive new notifications in this session.";
  }

  adw_status_page_set_description(ADW_STATUS_PAGE(state->m_details_page),
                                  description.c_str());
  rebuildEventFeed(state);
}

void loadSavedSelection(AppState *state) {
  if (state->m_settings == nullptr) {
    return;
  }

  auto *savedDevices =
      g_settings_get_value(state->m_settings, "selected-devices");
  if (savedDevices == nullptr) {
    g_warning("Could not read the saved Bluetooth device selection");

    return;
  }

  std::vector<dmn::gui::Dmn_DeviceIdentity> selection;
  GVariantIter iterator;
  const char *adapterAddress = nullptr;
  const char *deviceAddress = nullptr;
  g_variant_iter_init(&iterator, savedDevices);
  while (g_variant_iter_next(&iterator, "(&s&s)", &adapterAddress,
                             &deviceAddress)) {
    selection.push_back({adapterAddress, deviceAddress});
  }

  g_variant_unref(savedDevices);

  if (selection.empty()) {
    auto *legacy = g_settings_get_value(state->m_settings, "selected-device");
    if (legacy == nullptr) {
      g_warning("Could not read the legacy saved Bluetooth selection");

      return;
    }

    const char *legacyAdapter = nullptr;
    const char *legacyDevice = nullptr;
    g_variant_get(legacy, "(&s&s)", &legacyAdapter, &legacyDevice);
    if (*legacyAdapter != '\0' && *legacyDevice != '\0') {
      selection.push_back({legacyAdapter, legacyDevice});
    }

    g_variant_unref(legacy);
  }

  if (!selection.empty()) {
    state->m_selection.loadSavedSelection(std::move(selection));
  }
}

void loadMonitoringIntents(AppState *state) {
  if (state->m_settings == nullptr) {
    return;
  }

  auto *monitoringDevices =
      g_settings_get_value(state->m_settings, "monitoring-devices");
  if (monitoringDevices == nullptr) {
    g_warning("Could not read saved Bluetooth monitoring preferences");

    return;
  }

  GVariantIter iterator;
  const char *adapterAddress = nullptr;
  const char *deviceAddress = nullptr;
  g_variant_iter_init(&iterator, monitoringDevices);
  while (g_variant_iter_next(&iterator, "(&s&s)", &adapterAddress,
                             &deviceAddress)) {
    dmn::gui::Dmn_DeviceIdentity identity{adapterAddress, deviceAddress};
    std::transform(
        identity.m_adapter_address.begin(), identity.m_adapter_address.end(),
        identity.m_adapter_address.begin(), [](const unsigned char character) {
          return static_cast<char>(std::toupper(character));
        });
    std::transform(
        identity.m_device_address.begin(), identity.m_device_address.end(),
        identity.m_device_address.begin(), [](const unsigned char character) {
          return static_cast<char>(std::toupper(character));
        });
    if (identity.m_adapter_address.empty() ||
        identity.m_device_address.empty() ||
        !isSelected(state->m_selection.savedSelection(), identity) ||
        isSelected(state->m_monitoring_intents, identity)) {
      continue;
    }

    state->m_monitoring_intents.push_back(identity);
    auto &session = state->m_monitor_sessions[identityKey(identity)];
    session.m_identity = std::move(identity);
    session.m_status = "Monitoring when connected";
  }

  g_variant_unref(monitoringDevices);
}

void saveSelection(AppState *state) {
  if (state->m_settings == nullptr) {
    setStatus(state,
              "Settings are unavailable; device selection was not saved.");

    return;
  }

  const auto selection = state->m_selection.draftSelection();
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ss)"));
  for (const auto &identity : selection) {
    g_variant_builder_add(&builder, "(ss)", identity.m_adapter_address.c_str(),
                          identity.m_device_address.c_str());
  }

  auto *value = g_variant_builder_end(&builder);
  if (!g_settings_set_value(state->m_settings, "selected-devices", value)) {
    g_warning("Could not persist the Bluetooth device selection");
    setStatus(state, "The device selection could not be saved to settings.");

    return;
  }

  state->m_selection.saveSelection();
  const char *legacyAdapter =
      selection.empty() ? "" : selection.front().m_adapter_address.c_str();
  const char *legacyDevice =
      selection.empty() ? "" : selection.front().m_device_address.c_str();
  const bool legacySaved =
      g_settings_set_value(state->m_settings, "selected-device",
                           g_variant_new("(ss)", legacyAdapter, legacyDevice));
  if (!legacySaved) {
    g_warning("Could not clear the legacy Bluetooth device selection");
  }

  const auto previousIntents = state->m_monitoring_intents;
  std::erase_if(state->m_monitoring_intents,
                [&selection](const auto &identity) {
                  return !isSelected(selection, identity);
                });
  const bool intentsSaved = state->m_monitoring_intents == previousIntents ||
                            persistMonitoringIntents(state);
  if (!intentsSaved) {
    state->m_monitoring_intents = previousIntents;
  }

  auto status =
      selection.empty() ? std::string{"Device selection cleared."}
      : selection.size() == 1
          ? std::string{"One device selection saved."}
          : std::to_string(selection.size()) + " device selections saved.";
  if (!legacySaved) {
    status += " The legacy selection setting could not be updated; the current "
              "selection is active.";
  }

  if (!intentsSaved) {
    status += " Monitoring preferences could not be updated; the previous "
              "monitoring intent was retained.";
  }

  setStatus(state, status);
  refreshView(state, false);
  synchronizeMonitorSessions(state);
  updateMonitorPolling(state);
}

auto settingsForApp() -> GSettings * {
  // Prefer the installed schema, then the build-tree copy for development.
  const char *schemaDirectories[] = {DMN_BLUETOOTH_GTK_INSTALL_SCHEMA_DIR,
                                     DMN_BLUETOOTH_GTK_SCHEMA_DIR};
  GSettingsSchema *schema = nullptr;
  for (const auto *schemaDirectory : schemaDirectories) {
    GError *error = nullptr;
    auto *source = g_settings_schema_source_new_from_directory(
        schemaDirectory, g_settings_schema_source_get_default(), FALSE, &error);
    if (source == nullptr) {
      if (error != nullptr) {
        g_debug("Could not load GSettings schemas from %s: %s", schemaDirectory,
                error->message);
      }

      g_clear_error(&error);

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
    if (source == nullptr) {
      g_warning("No GSettings schema source is available");

      return nullptr;
    }

    schema = g_settings_schema_source_lookup(source, kSettingsSchemaId, TRUE);
  }

  if (schema == nullptr) {
    g_warning("GSettings schema '%s' was not found", kSettingsSchemaId);

    return nullptr;
  }

  auto *settings = g_settings_new_full(schema, nullptr, nullptr);
  g_settings_schema_unref(schema);

  return settings;
}

void taskWorker(GTask *task, gpointer, gpointer taskData, GCancellable *) {
  auto *data = static_cast<TaskData *>(taskData);
  // Return only owned values from the worker; GTK objects stay on the main
  // context and are updated by the GTask completion callback.
  auto reply =
      data->m_backend->perform(data->m_operation, data->m_adapter_path,
                               data->m_identity, data->m_cancel_connect);
  g_task_return_pointer(
      task, new BackendReply{std::move(reply)},
      [](gpointer result) { delete static_cast<BackendReply *>(result); });
}

void connectionCancelTaskWorker(GTask *task, gpointer, gpointer taskData,
                                GCancellable *) {
  auto *data = static_cast<ConnectionCancelTaskData *>(taskData);
  ConnectionCancelReply reply{data->m_identity, {}};
  try {
    dmn::Dmn_BluezClient client;
    const auto inventory = client.snapshot(kDeviceSnapshotTimeout);
    if (!inventory) {
      reply.m_error = inventory.error().message;
    } else {
      const auto result = client.cancelConnectionAttempt(
          data->m_identity.m_device_address, data->m_identity.m_adapter_address,
          kDeviceStateTimeout);
      if (!result) {
        reply.m_error = result.error().message;
      }
    }
  } catch (const std::exception &exception) {
    reply.m_error = exception.what();
  }

  g_task_return_pointer(task, new ConnectionCancelReply{std::move(reply)},
                        [](gpointer result) {
                          delete static_cast<ConnectionCancelReply *>(result);
                        });
}

auto monitorStageName(const dmn::Dmn_AncsMonitorStage stage) -> const char * {
  switch (stage) {
  case dmn::Dmn_AncsMonitorStage::InvalidArgument:
    return "invalid configuration";
  case dmn::Dmn_AncsMonitorStage::NotReady:
    return "monitor not ready";
  case dmn::Dmn_AncsMonitorStage::Inventory:
    return "Bluetooth inventory";
  case dmn::Dmn_AncsMonitorStage::Discovery:
    return "Bluetooth discovery";
  case dmn::Dmn_AncsMonitorStage::DeviceNotFound:
    return "device lookup";
  case dmn::Dmn_AncsMonitorStage::Pairing:
    return "pairing";
  case dmn::Dmn_AncsMonitorStage::Connection:
    return "connection";
  case dmn::Dmn_AncsMonitorStage::AncsStartup:
    return "ANCS startup";
  case dmn::Dmn_AncsMonitorStage::Cancelled:
    return "canceled";
  case dmn::Dmn_AncsMonitorStage::Cleanup:
    return "monitor cleanup";
  }

  return "unknown monitor failure";
}

void monitorTaskWorker(GTask *task, gpointer, gpointer taskData,
                       GCancellable *) {
  auto *data = static_cast<MonitorTaskData *>(taskData);
  MonitorReply reply{data->m_identity, data->m_operation, nullptr, {}};
  try {
    if (data->m_operation == Dmn_MonitorOperation::kStart) {
      auto app = std::shared_ptr<GtkApplication>(
          GTK_APPLICATION(g_object_ref(data->m_app)),
          [](GtkApplication *application) { g_object_unref(application); });
      auto monitor = std::make_shared<dmn::Dmn_AncsBluezMonitor>();
      const auto handlerResult =
          monitor->setEventHandler([app, identity = data->m_identity,
                                    cancelStart = data->m_cancel_start](
                                       const dmn::Dmn_AncsEvent &event) {
            if (cancelStart != nullptr &&
                cancelStart->load(std::memory_order_acquire)) {
              return;
            }

            dispatchMonitorEvent(app.get(), identity, event);
          });
      if (!handlerResult) {
        reply.m_error = handlerResult.error().message;
      } else {
        dmn::Dmn_AncsMonitorConfig config;
        config.adapter_selector = data->m_identity.m_adapter_address;
        config.require_existing_connection = true;
        config.request_remaining_profiles_when_connected = true;
        config.ancs.notification_date_cutoff = data->m_notification_cutoff;
        config.cancellation_requested = [cancelStart = data->m_cancel_start] {
          return cancelStart != nullptr &&
                 cancelStart->load(std::memory_order_acquire);
        };
        config.progress_handler =
            [app, identity = data->m_identity,
             cancelStart = data->m_cancel_start](
                const dmn::Dmn_AncsMonitorProgress progress) {
              postMonitorProgress(app.get(), identity, progress, cancelStart);
            };
        auto started =
            monitor->start(data->m_identity.m_device_address, config);
        if (!started) {
          if (started.error().stage == dmn::Dmn_AncsMonitorStage::Cancelled) {
            reply.m_cancelled = true;
          } else {
            reply.m_error =
                std::string{monitorStageName(started.error().stage)} + ": " +
                started.error().message;
          }
        } else if (data->m_cancel_start != nullptr &&
                   data->m_cancel_start->load(std::memory_order_acquire)) {
          const auto stopped = monitor->stop();
          if (!stopped) {
            reply.m_error = stopped.error().message;
          } else {
            reply.m_cancelled = true;
          }
        } else {
          reply.m_monitor = std::move(monitor);
        }
      }
    } else if (data->m_monitor == nullptr) {
      reply.m_error = "monitor session is unavailable";
    } else {
      const auto stopped = data->m_monitor->stop();
      if (!stopped) {
        reply.m_error = stopped.error().message;
        reply.m_monitor = data->m_monitor;
      }
    }
  } catch (const std::exception &exception) {
    reply.m_error = exception.what();
    if (data->m_operation == Dmn_MonitorOperation::kStop) {
      reply.m_monitor = data->m_monitor;
    }
  }

  g_task_return_pointer(
      task, new MonitorReply{std::move(reply)},
      [](gpointer result) { delete static_cast<MonitorReply *>(result); });
}

void monitorOperationFinished(GObject *sourceObject, GAsyncResult *result,
                              gpointer) {
  auto *state = stateFor(sourceObject);
  GError *error = nullptr;
  auto *reply = static_cast<MonitorReply *>(
      g_task_propagate_pointer(G_TASK(result), &error));
  if (state->m_monitor_operations > 0) {
    --state->m_monitor_operations;
  }

  const auto identity =
      reply != nullptr ? reply->m_identity : dmn::gui::Dmn_DeviceIdentity{};
  auto *session = findMonitorSession(state, identity);
  bool shouldDisconnect = false;
  bool shouldConnect = false;
  bool shouldStopCanceledStart = false;
  bool monitorStartFailed = false;
  bool monitorStopFailed = false;
  if (session != nullptr) {
    const bool startCancellationRequested = session->m_start_cancel_requested;
    const bool disconnectAfterOperation = session->m_disconnect_after_stop;
    const bool connectAfterOperation = session->m_connect_after_stop;
    session->m_busy = false;
    session->m_stopping = false;
    session->m_cancel_start.reset();
    session->m_start_cancel_requested = false;
    if (error != nullptr) {
      session->m_error = error->message;
      session->m_status = "Monitor operation failed";
      monitorStartFailed = reply != nullptr &&
                           reply->m_operation == Dmn_MonitorOperation::kStart;
      monitorStopFailed =
          reply != nullptr && reply->m_operation == Dmn_MonitorOperation::kStop;
      shouldDisconnect = disconnectAfterOperation;
      shouldConnect = connectAfterOperation;
      if (shouldConnect) {
        session->m_connect_after_stop = false;
      }

      if (monitorStopFailed) {
        session->m_disconnect_after_stop = false;
      }
    } else if (reply != nullptr && !reply->m_error.empty()) {
      session->m_error = reply->m_error;
      session->m_status = reply->m_operation == Dmn_MonitorOperation::kStart
                              ? "Monitor failed"
                              : "Monitor cleanup failed";
      monitorStartFailed = reply->m_operation == Dmn_MonitorOperation::kStart;
      monitorStopFailed = reply->m_operation == Dmn_MonitorOperation::kStop;
      shouldDisconnect = disconnectAfterOperation;
      shouldConnect = connectAfterOperation;
      if (reply->m_operation == Dmn_MonitorOperation::kStop) {
        session->m_disconnect_after_stop = false;
        session->m_connect_after_stop = false;
      }

      if (shouldConnect) {
        session->m_connect_after_stop = false;
      }
    } else if (reply != nullptr && reply->m_cancelled) {
      session->m_monitor.reset();
      session->m_monitor_restart_required = false;
      session->m_error.clear();
      session->m_status = "Not monitoring";
      shouldDisconnect = disconnectAfterOperation;
      shouldConnect = connectAfterOperation;
      session->m_disconnect_after_stop = false;
      session->m_connect_after_stop = false;
    } else if (reply != nullptr &&
               reply->m_operation == Dmn_MonitorOperation::kStart) {
      session->m_monitor = std::move(reply->m_monitor);
      session->m_error.clear();
      shouldStopCanceledStart =
          session->m_monitor != nullptr && startCancellationRequested;
      session->m_status = shouldStopCanceledStart
                              ? "Stopping canceled monitor startup"
                          : session->m_monitor != nullptr
                              ? monitorStateName(session->m_monitor->state())
                              : "Monitor failed";
      monitorStartFailed =
          session->m_monitor == nullptr && !shouldStopCanceledStart;
      if (session->m_monitor != nullptr && !shouldStopCanceledStart) {
        const auto monitorState = session->m_monitor->state();
        const bool monitorEnded = monitorState == dmn::Dmn_AncsState::Failed ||
                                  monitorState == dmn::Dmn_AncsState::Stopped;
        if (monitorEnded &&
            isSelected(state->m_monitoring_intents, session->m_identity)) {
          session->m_monitor_restart_required = true;
          (void)state->m_monitor_controller.recordStartFailure(
              session->m_identity,
              dmn::gui::Dmn_DeviceMonitorController::Clock::now(),
              static_cast<unsigned int>(g_random_int_range(80, 101)));
        } else {
          state->m_monitor_controller.recordStartSuccess(session->m_identity);
        }
      }

      if (monitorStartFailed) {
        session->m_monitor_restart_required = false;
      }
    } else if (reply != nullptr) {
      const bool retryMonitor = session->m_monitor_restart_required;
      shouldDisconnect = session->m_disconnect_after_stop;
      shouldConnect = session->m_connect_after_stop;
      session->m_disconnect_after_stop = false;
      session->m_connect_after_stop = false;
      withdrawDesktopAlert(state, session->m_identity);
      session->m_monitor.reset();
      session->m_monitor_restart_required = retryMonitor;
      session->m_error.clear();
      session->m_status =
          retryMonitor ? "Retrying monitor start"
          : isSelected(state->m_monitoring_intents, session->m_identity)
              ? "Monitoring when connected"
              : "Not monitoring";
    }
  }

  if (monitorStartFailed && session != nullptr &&
      isSelected(state->m_monitoring_intents, session->m_identity)) {
    const auto retryDelay = state->m_monitor_controller.recordStartFailure(
        session->m_identity,
        dmn::gui::Dmn_DeviceMonitorController::Clock::now(),
        static_cast<unsigned int>(g_random_int_range(80, 101)));
    session->m_status = "Retrying monitor start";
    g_message("ANCS monitor start failed for %s; retrying in %lld milliseconds",
              session->m_identity.m_device_address.c_str(),
              static_cast<long long>(retryDelay.count()));
  }

  if (error != nullptr) {
    g_warning("ANCS monitor task failed: %s", error->message);
    setStatus(state, error->message);
    g_error_free(error);
  } else if (reply != nullptr && !reply->m_error.empty()) {
    g_warning(
        "ANCS monitor %s failed for %s: %s",
        reply->m_operation == Dmn_MonitorOperation::kStart ? "start" : "stop",
        reply->m_identity.m_device_address.c_str(), reply->m_error.c_str());
    setStatus(state, reply->m_error);
  } else if (reply != nullptr && reply->m_cancelled) {
    g_message("ANCS monitor startup canceled for %s",
              reply->m_identity.m_device_address.c_str());
    setStatus(state, "ANCS monitor startup canceled.");
  } else if (shouldStopCanceledStart) {
    g_message(
        "ANCS monitor startup completed after cancellation for %s; stopping it",
        reply->m_identity.m_device_address.c_str());
    setStatus(state, "Stopping canceled ANCS monitor startup.");
  } else if (reply != nullptr) {
    g_message("ANCS monitor %s completed for %s",
              reply->m_operation == Dmn_MonitorOperation::kStart ? "start"
                                                                 : "stop",
              reply->m_identity.m_device_address.c_str());
    if (reply->m_operation == Dmn_MonitorOperation::kStart) {
      setStatus(state, "ANCS monitoring started for the selected device.");
    } else {
      setStatus(state, "ANCS monitoring stopped.");
    }
  }

  delete reply;
  refreshView(state, false);
  updateMonitorPolling(state);

  if (state->m_close_requested && session != nullptr &&
      session->m_status == "Monitor cleanup failed") {
    state->m_close_requested = false;
    updateButtonState(state);
    updateMonitorPolling(state);
  } else if (shouldStopCanceledStart) {
    requestMonitorOperation(state, identity, Dmn_MonitorOperation::kStop);
  } else if (shouldDisconnect) {
    if (session != nullptr) {
      session->m_disconnect_after_stop = false;
    }

    requestOperation(state, Dmn_Operation::kDisconnect, {}, identity);
  } else if (shouldConnect) {
    requestOperation(state, Dmn_Operation::kConnect, {}, identity);
  } else {
    if (!state->m_close_requested && !monitorStopFailed) {
      synchronizeMonitorSessions(state);
    }

    maybeFinishClose(state);
  }
}

void requestMonitorOperation(AppState *state,
                             const dmn::gui::Dmn_DeviceIdentity &identity,
                             const Dmn_MonitorOperation operation,
                             const bool disconnectAfterStop) {
  if (state->m_busy ||
      (state->m_close_requested && operation == Dmn_MonitorOperation::kStart)) {
    setStatus(state, "Wait for the current Bluetooth operation to finish.");

    return;
  }

  auto &session = state->m_monitor_sessions[identityKey(identity)];
  session.m_identity = identity;
  if (session.m_busy) {
    return;
  }

  if (operation == Dmn_MonitorOperation::kStart &&
      session.m_monitor != nullptr) {
    return;
  }

  if (operation == Dmn_MonitorOperation::kStop &&
      session.m_monitor == nullptr) {
    setStatus(state, "This device does not have an active monitor.");

    return;
  }

  session.m_busy = true;
  if (operation == Dmn_MonitorOperation::kStart) {
    session.m_status = "Starting ANCS monitor";
    session.m_error.clear();
    session.m_stopping = false;
    session.m_start_cancel_requested = false;
    session.m_monitor_restart_required = false;
    session.m_cancel_start = std::make_shared<std::atomic_bool>(false);
  } else {
    session.m_status = "Stopping ANCS monitor";
    session.m_disconnect_after_stop = disconnectAfterStop;
    session.m_stopping = true;
  }

  ++state->m_monitor_operations;
  setStatus(state, operation == Dmn_MonitorOperation::kStart
                       ? "Starting ANCS monitoring…"
                       : "Stopping ANCS monitoring…");
  refreshView(state, false);

  auto *task = g_task_new(G_OBJECT(state->m_app), nullptr,
                          monitorOperationFinished, nullptr);
  auto *data = new MonitorTaskData{GTK_APPLICATION(g_object_ref(state->m_app)),
                                   identity,
                                   operation,
                                   session.m_monitor,
                                   session.m_cancel_start,
                                   state->m_session_started};
  g_task_set_task_data(task, data, [](gpointer taskData) {
    delete static_cast<MonitorTaskData *>(taskData);
  });
  g_task_run_in_thread(task, monitorTaskWorker);
  g_object_unref(task);
  updateMonitorPolling(state);
}

gboolean monitorPoll(gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  if (state->m_close_requested) {
    state->m_device_poll_source = 0;

    return G_SOURCE_REMOVE;
  }

  if (!state->m_busy && !state->m_scanning) {
    requestOperation(state, Dmn_Operation::kInventory, {}, std::nullopt, false);
  }

  return G_SOURCE_CONTINUE;
}

void updateMonitorPolling(AppState *state) {
  const bool monitorActive = std::any_of(
      state->m_monitor_sessions.begin(), state->m_monitor_sessions.end(),
      [](const auto &entry) {
        return entry.second.m_monitor != nullptr || entry.second.m_busy;
      });
  const bool shouldPoll =
      !state->m_close_requested &&
      (!state->m_selection.savedSelection().empty() || monitorActive);

  if (shouldPoll && state->m_device_poll_source == 0) {
    state->m_device_poll_source = g_timeout_add_seconds(4, monitorPoll, state);
  } else if (!shouldPoll && state->m_device_poll_source != 0) {
    g_source_remove(state->m_device_poll_source);
    state->m_device_poll_source = 0;
  }
}

void finishConnectionCancellation(AppState *state) {
  if (!state->m_connection_cancel_requested ||
      state->m_connection_task_active ||
      state->m_connection_cancel_task_active) {
    return;
  }

  state->m_connecting_device.reset();
  state->m_connect_cancel_token.reset();
  state->m_connection_cancel_requested = false;
  state->m_busy = false;
  setStatus(state, "Connection attempt canceled; refreshing device state.");
  updateButtonState(state);
  requestOperation(state, Dmn_Operation::kInventory, {}, std::nullopt, false);
}

void connectionCancelFinished(GObject *sourceObject, GAsyncResult *result,
                              gpointer) {
  auto *state = stateFor(sourceObject);
  GError *error = nullptr;
  auto *reply = static_cast<ConnectionCancelReply *>(
      g_task_propagate_pointer(G_TASK(result), &error));
  state->m_connection_cancel_task_active = false;

  if (error != nullptr) {
    state->m_connection_cancel_requested = false;
    if (state->m_connect_cancel_token != nullptr) {
      state->m_connect_cancel_token->store(false, std::memory_order_release);
    }

    g_warning("Could not cancel the Bluetooth connection attempt: %s",
              error->message);
    setStatus(state, error->message);
    g_error_free(error);
  } else if (reply != nullptr && !reply->m_error.empty()) {
    state->m_connection_cancel_requested = false;
    if (state->m_connect_cancel_token != nullptr) {
      state->m_connect_cancel_token->store(false, std::memory_order_release);
    }

    g_warning("Could not cancel the Bluetooth connection attempt: %s",
              reply->m_error.c_str());
    setStatus(state, reply->m_error);
  }

  delete reply;
  state->m_busy = state->m_connection_task_active;
  if (!state->m_connection_cancel_requested &&
      !state->m_connection_task_active) {
    state->m_connecting_device.reset();
    state->m_connect_cancel_token.reset();
    if (!state->m_close_requested) {
      synchronizeMonitorSessions(state);
    }
  } else {
    finishConnectionCancellation(state);
  }

  updateButtonState(state);
  refreshView(state, false);
  maybeFinishClose(state);
}

void requestConnectionCancel(AppState *state,
                             const dmn::gui::Dmn_DeviceIdentity &identity) {
  if (state->m_connection_cancel_requested ||
      state->m_connection_cancel_task_active ||
      !state->m_connecting_device.has_value() ||
      *state->m_connecting_device != identity ||
      state->m_connect_cancel_token == nullptr) {
    return;
  }

  state->m_connection_cancel_requested = true;
  state->m_connection_cancel_task_active = true;
  state->m_connect_cancel_token->store(true, std::memory_order_release);
  setStatus(state, "Disconnecting to cancel the connection attempt…");
  refreshView(state, false);

  auto *task = g_task_new(G_OBJECT(state->m_app), nullptr,
                          connectionCancelFinished, nullptr);
  auto *data = new ConnectionCancelTaskData{identity};
  g_task_set_task_data(task, data, [](gpointer taskData) {
    delete static_cast<ConnectionCancelTaskData *>(taskData);
  });
  g_task_run_in_thread(task, connectionCancelTaskWorker);
  g_object_unref(task);
}

void synchronizeMonitorSessions(AppState *state) {
  std::vector<dmn::gui::Dmn_MonitorControllerDeviceState> devices;
  devices.reserve(state->m_selection.devices().size() +
                  state->m_monitor_sessions.size());
  const auto cancelDisconnectedStart = [](MonitorSession *session,
                                          const bool connected) {
    if (session != nullptr && session->m_busy &&
        session->m_monitor == nullptr && !connected &&
        session->m_cancel_start != nullptr &&
        !session->m_start_cancel_requested) {
      session->m_monitor_restart_required = false;
      session->m_start_cancel_requested = true;
      session->m_stopping = true;
      session->m_cancel_start->store(true, std::memory_order_release);
    }
  };
  for (const auto &device : state->m_selection.devices()) {
    auto *session = findMonitorSession(state, device.m_identity);
    cancelDisconnectedStart(session, device.m_present &&
                                         device.m_paired.value_or(false) &&
                                         device.m_connected.value_or(false));
    devices.push_back(
        {device.m_identity,
         isSelected(state->m_selection.savedSelection(), device.m_identity),
         isSelected(state->m_monitoring_intents, device.m_identity),
         device.m_present, device.m_paired.value_or(false),
         device.m_connected.value_or(false),
         session != nullptr && session->m_monitor != nullptr,
         session != nullptr && session->m_monitor_restart_required,
         session != nullptr && session->m_busy});
  }

  for (auto &[key, session] : state->m_monitor_sessions) {
    (void)key;
    const auto exists = std::any_of(
        devices.begin(), devices.end(), [&session](const auto &device) {
          return device.m_identity == session.m_identity;
        });
    if (!exists) {
      cancelDisconnectedStart(&session, false);
      devices.push_back(
          {session.m_identity,
           isSelected(state->m_selection.savedSelection(), session.m_identity),
           isSelected(state->m_monitoring_intents, session.m_identity), false,
           false, false, session.m_monitor != nullptr,
           session.m_monitor_restart_required, session.m_busy});
    }
  }

  const bool allowStarts = !state->m_close_requested && !state->m_busy &&
                           !state->m_selection.hasUnsavedChanges() &&
                           !state->m_connection_cancel_requested;
  const auto commands = state->m_monitor_controller.reconcile(
      devices, allowStarts,
      dmn::gui::Dmn_DeviceMonitorController::Clock::now());
  if (state->m_busy || state->m_close_requested) {
    updateMonitorPolling(state);

    return;
  }

  for (const auto &command : commands) {
    const auto operation =
        command.m_action ==
                dmn::gui::Dmn_MonitorControllerCommand::Action::kStart
            ? Dmn_MonitorOperation::kStart
            : Dmn_MonitorOperation::kStop;
    const auto *session = findMonitorSession(state, command.m_identity);
    requestMonitorOperation(state, command.m_identity, operation,
                            operation == Dmn_MonitorOperation::kStop &&
                                session != nullptr &&
                                session->m_disconnect_after_stop);
  }

  updateMonitorPolling(state);
}

void applyInventory(AppState *state, const dmn::Dmn_BluezInventory &inventory) {
  g_debug("Received Bluetooth inventory: %zu adapter(s), %zu device(s)",
          inventory.adapters.size(), inventory.devices.size());
  updateAdapters(state, inventory);
  state->m_selection.replaceInventory(toGuiInventory(inventory));
  state->m_inventory_loaded = true;
  state->m_inventory_error.clear();
  refreshView(state);
  setStatus(state,
            state->m_scanning ? "Nearby Bluetooth devices updated."
                              : "Bluetooth devices updated.",
            false);
}

gboolean scanPoll(gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  if (!state->m_scanning) {
    state->m_scan_poll_source = 0;

    return G_SOURCE_REMOVE;
  }

  if (std::chrono::steady_clock::now() >= state->m_scan_deadline) {
    if (state->m_busy) {
      return G_SOURCE_CONTINUE;
    }

    state->m_scan_poll_source = 0;
    requestOperation(state, Dmn_Operation::kStopDiscovery);

    return G_SOURCE_REMOVE;
  }

  if (!state->m_busy) {
    requestOperation(state, Dmn_Operation::kInventory, {}, std::nullopt, false);
  }

  return G_SOURCE_CONTINUE;
}

void operationFinished(GObject *sourceObject, GAsyncResult *result, gpointer) {
  auto *state = stateFor(sourceObject);
  GError *error = nullptr;
  auto *reply = static_cast<BackendReply *>(
      g_task_propagate_pointer(G_TASK(result), &error));
  const bool isConnectOperation =
      reply != nullptr && reply->m_operation == Dmn_Operation::kConnect;
  const bool isDisconnectOperation =
      reply != nullptr && reply->m_operation == Dmn_Operation::kDisconnect;
  const bool stopFailed = reply != nullptr &&
                          reply->m_operation == Dmn_Operation::kStopDiscovery &&
                          !reply->m_error.empty();
  std::optional<dmn::gui::Dmn_DeviceIdentity> reconnectAfterDisconnect;

  if (isConnectOperation) {
    state->m_connection_task_active = false;
  }

  if (isDisconnectOperation) {
    state->m_disconnecting_device.reset();
    if (error == nullptr && reply->m_error.empty()) {
      reconnectAfterDisconnect = state->m_reconnect_after_disconnect;
    }

    state->m_reconnect_after_disconnect.reset();
  }

  state->m_busy =
      isConnectOperation ? state->m_connection_cancel_task_active : false;
  if (error != nullptr) {
    g_warning("Bluetooth background task failed: %s", error->message);
    setStatus(state, error->message);
    g_error_free(error);
  } else if (reply != nullptr && reply->m_connection_cancelled) {
    if (reply->m_inventory) {
      applyInventory(state, *reply->m_inventory);
    }

    setStatus(state, "Connection attempt canceled.");
  } else if (reply != nullptr) {
    if (!reply->m_error.empty()) {
      g_warning("Bluetooth %s failed: %s", operationName(reply->m_operation),
                reply->m_error.c_str());
      setStatus(state, reply->m_error);
      if (reply->m_operation == Dmn_Operation::kInventory) {
        state->m_inventory_error = reply->m_error;
      }

      if (reply->m_discovery_started) {
        state->m_scanning = true;
        state->m_scan_deadline =
            std::chrono::steady_clock::now() + kDiscoveryDuration;
        if (state->m_scan_poll_source == 0) {
          state->m_scan_poll_source = g_timeout_add_seconds(1, scanPoll, state);
        }
      } else if (reply->m_operation == Dmn_Operation::kStartDiscovery) {
        state->m_scanning = false;
      }

      if (reply->m_discovery_stopped) {
        state->m_scanning = false;
        if (state->m_scan_poll_source != 0) {
          g_source_remove(state->m_scan_poll_source);
          state->m_scan_poll_source = 0;
        }
      } else if (reply->m_operation == Dmn_Operation::kStopDiscovery) {
        state->m_scanning = true;
      }
    } else {
      g_debug("Bluetooth %s completed", operationName(reply->m_operation));
      if (reply->m_inventory) {
        applyInventory(state, *reply->m_inventory);
      }

      if (reply->m_operation == Dmn_Operation::kStartDiscovery) {
        state->m_scanning = true;
        state->m_scan_deadline =
            std::chrono::steady_clock::now() + kDiscoveryDuration;
        if (state->m_scan_poll_source == 0) {
          state->m_scan_poll_source = g_timeout_add_seconds(1, scanPoll, state);
        }

        g_message("Bluetooth discovery started; it will stop after 30 seconds");
        setStatus(
            state,
            "Scanning for nearby devices. The scan stops after 30 seconds.");
      } else if (reply->m_operation == Dmn_Operation::kStopDiscovery) {
        state->m_scanning = false;
        if (state->m_scan_poll_source != 0) {
          g_source_remove(state->m_scan_poll_source);
          state->m_scan_poll_source = 0;
        }

        g_message("Bluetooth discovery stopped");
        setStatus(state, "Bluetooth scan stopped.");
      }
    }

    delete reply;
  }

  if (isConnectOperation) {
    if (state->m_connection_cancel_requested) {
      finishConnectionCancellation(state);
    } else {
      state->m_connecting_device.reset();
      state->m_connect_cancel_token.reset();
    }
  }

  if (state->m_close_requested && stopFailed) {
    state->m_close_requested = false;
    updateButtonState(state);
    refreshView(state, false);

    return;
  }

  if (reconnectAfterDisconnect.has_value() && !state->m_close_requested) {
    requestOperation(state, Dmn_Operation::kConnect, {},
                     reconnectAfterDisconnect);

    return;
  }

  state->m_busy = state->m_connection_cancel_task_active || state->m_busy;
  if (!state->m_busy && !state->m_close_requested &&
      state->m_queued_connection_action.has_value()) {
    const auto queued = *state->m_queued_connection_action;
    state->m_queued_connection_action.reset();
    const auto device = std::find_if(
        state->m_selection.devices().begin(),
        state->m_selection.devices().end(), [&queued](const auto &entry) {
          return entry.m_identity == queued.m_identity;
        });
    if (device == state->m_selection.devices().end() || !device->m_present) {
      setStatus(state,
                "Queued connection action canceled because the device is "
                "unavailable.");
    } else if (queued.m_connect != device->m_connected.value_or(false)) {
      RowContext context{state, queued.m_identity};
      connectionClicked(nullptr, &context);
    }
  }

  updateButtonState(state);
  if (!state->m_close_requested && !state->m_connection_cancel_requested) {
    synchronizeMonitorSessions(state);
  }

  refreshView(state, false);
  maybeFinishClose(state);
}

void requestOperation(AppState *state, const Dmn_Operation operation,
                      std::string adapterPath,
                      std::optional<dmn::gui::Dmn_DeviceIdentity> identity,
                      const bool announceStatus) {
  if (state->m_busy) {
    return;
  }

  if (operation == Dmn_Operation::kStartDiscovery && adapterPath.empty()) {
    setStatus(state, "Choose a powered Bluetooth adapter before scanning.");

    return;
  }

  state->m_busy = true;
  std::shared_ptr<std::atomic_bool> cancelConnect;
  if (operation == Dmn_Operation::kDisconnect && identity.has_value()) {
    state->m_disconnecting_device = identity;
    state->m_reconnect_after_disconnect.reset();
  }

  if (operation == Dmn_Operation::kConnect && identity.has_value()) {
    state->m_connecting_device = identity;
    state->m_connection_cancel_requested = false;
    state->m_connection_task_active = true;
    cancelConnect = std::make_shared<std::atomic_bool>(false);
    state->m_connect_cancel_token = cancelConnect;
  }

  g_debug("Starting Bluetooth %s", operationName(operation));
  if (operation == Dmn_Operation::kInventory && state->m_scanning) {
    setStatus(state, "Updating nearby Bluetooth devices…", announceStatus);
  } else if (operation == Dmn_Operation::kInventory) {
    setStatus(state, "Refreshing Bluetooth devices…", announceStatus);
  } else if (operation == Dmn_Operation::kStartDiscovery) {
    setStatus(state, "Starting Bluetooth scan…", announceStatus);
  } else if (operation == Dmn_Operation::kStopDiscovery) {
    setStatus(state, "Stopping Bluetooth scan…", announceStatus);
  } else if (operation == Dmn_Operation::kConnect) {
    setStatus(state, "Connecting to the selected device…", announceStatus);
  } else if (operation == Dmn_Operation::kDisconnect) {
    setStatus(state, "Disconnecting the selected device…", announceStatus);
  } else {
    setStatus(state, "Stopping Bluetooth scan…", announceStatus);
  }

  updateButtonState(state);
  refreshView(state, false);

  auto *task =
      g_task_new(G_OBJECT(state->m_app), nullptr, operationFinished, nullptr);
  auto *data = new TaskData{state->m_backend, operation, std::move(adapterPath),
                            std::move(identity), std::move(cancelConnect)};
  g_task_set_task_data(task, data, [](gpointer taskData) {
    delete static_cast<TaskData *>(taskData);
  });
  g_task_run_in_thread(task, taskWorker);
  g_object_unref(task);
}

void refreshClicked(GtkButton *, gpointer userData) {
  requestOperation(static_cast<AppState *>(userData), Dmn_Operation::kInventory,
                   {}, std::nullopt);
}

void scanClicked(GtkButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  const auto selected =
      gtk_drop_down_get_selected(GTK_DROP_DOWN(state->m_adapter_dropdown));
  if (selected >= state->m_adapters.size()) {
    setStatus(state, "Choose a powered Bluetooth adapter before scanning.");

    return;
  }

  state->m_selected_adapter_address = state->m_adapters[selected].m_address;
  requestOperation(state, Dmn_Operation::kStartDiscovery,
                   state->m_adapters[selected].m_path, std::nullopt);
}

void stopScanClicked(GtkButton *, gpointer userData) {
  requestOperation(static_cast<AppState *>(userData),
                   Dmn_Operation::kStopDiscovery, {}, std::nullopt);
}

void selectionClicked(GtkButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  saveSelection(state);
}

void discardClicked(GtkButton *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  state->m_selection.discardChanges();
  setStatus(state, "Unsaved device selection discarded.");
  refreshView(state, false);
  synchronizeMonitorSessions(state);
}

void adapterChanged(GObject *object, GParamSpec *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
  if (selected < state->m_adapters.size()) {
    state->m_selected_adapter_address = state->m_adapters[selected].m_address;
  }
}

gboolean windowCloseRequested(GtkWindow *, gpointer userData) {
  requestClose(static_cast<AppState *>(userData));

  return TRUE;
}

void requestClose(AppState *state) {
  if (state->m_close_requested) {
    return;
  }

  state->m_close_requested = true;
  g_message("Application shutdown requested");
  if (state->m_device_poll_source != 0) {
    g_source_remove(state->m_device_poll_source);
    state->m_device_poll_source = 0;
  }

  setStatus(state, "Stopping Bluetooth monitors before quitting…");
  refreshView(state, false);
  maybeFinishClose(state);
}

void maybeFinishClose(AppState *state) {
  if (!state->m_close_requested || state->m_busy ||
      state->m_monitor_operations > 0) {
    return;
  }

  if (state->m_scanning) {
    requestOperation(state, Dmn_Operation::kStopDiscovery);

    return;
  }

  for (auto &[key, session] : state->m_monitor_sessions) {
    (void)key;
    if (session.m_monitor != nullptr && !session.m_busy) {
      requestMonitorOperation(state, session.m_identity,
                              Dmn_MonitorOperation::kStop);

      return;
    }
  }

  withdrawDesktopAlerts(state);
  g_message("Bluetooth monitors stopped; quitting application");
  g_application_quit(G_APPLICATION(state->m_app));
}

void quitAction(GSimpleAction *, GVariant *, gpointer userData) {
  requestClose(static_cast<AppState *>(userData));
}

void openAction(GSimpleAction *, GVariant *, gpointer userData) {
  g_application_activate(G_APPLICATION(userData));
}

void desktopAlertsAction(GSimpleAction *action, GVariant *, gpointer userData) {
  auto *state = static_cast<AppState *>(userData);
  if (state->m_settings == nullptr) {
    setStatus(state, "Desktop alert preference is unavailable.");

    return;
  }

  const bool enabled = !state->m_desktop_alerts_enabled;
  if (!g_settings_set_boolean(state->m_settings, "desktop-alerts", enabled)) {
    setStatus(state, "Could not save the desktop alert preference.");

    return;
  }

  state->m_desktop_alerts_enabled = enabled;
  g_simple_action_set_state(action, g_variant_new_boolean(enabled));
  if (!enabled) {
    withdrawDesktopAlerts(state);
  }

  setStatus(state, enabled ? "Generic desktop alerts enabled."
                           : "Desktop alerts disabled.");
}

/**
 * @brief Show or present the application's About window.
 *
 * The standard libadwaita window separates details, credits, and legal
 * information while keeping displayed links tied to application metadata.
 */
void aboutAction(GSimpleAction *, GVariant *, gpointer userData) {
  auto *app = GTK_APPLICATION(userData);
  for (auto *window = gtk_application_get_windows(app); window != nullptr;
       window = window->next) {
    if (ADW_IS_ABOUT_WINDOW(window->data)) {
      gtk_window_present(GTK_WINDOW(window->data));

      return;
    }
  }

  const char *developers[] = {"Chee Bin Hoh https://github.com/cheebinhoh",
                              nullptr};
  auto *parent = gtk_application_get_active_window(app);
  auto *about = ADW_ABOUT_WINDOW(adw_about_window_new());
  adw_about_window_set_application_name(about, "Dmn Bluetooth");
  adw_about_window_set_application_icon(about, kApplicationId);
  adw_about_window_set_developer_name(about, "Chee Bin Hoh");
  adw_about_window_set_version(about, DMN_VERSION);
  adw_about_window_set_comments(
      about, "Bluetooth device discovery and selection for Dmn.");
  adw_about_window_set_copyright(about, "© 2026 Chee Bin Hoh");
  adw_about_window_set_developers(about, developers);
  adw_about_window_set_license_type(about, GTK_LICENSE_GPL_3_0_ONLY);
  adw_about_window_add_link(about, "Repository",
                            "https://github.com/cheebinhoh/Dmn");
  adw_about_window_add_link(about, "Issues",
                            "https://github.com/cheebinhoh/Dmn/issues");
  adw_about_window_add_link(about, "Email", "mailto:CheeBin.Hoh@gmail.com");
  adw_about_window_add_link(
      about, "GPL-3.0-only",
      "https://github.com/cheebinhoh/Dmn?tab=GPL-3.0-1-ov-file");

  g_message("Showing application About window");
  gtk_window_set_application(GTK_WINDOW(about), app);
  if (parent != nullptr) {
    gtk_window_set_transient_for(GTK_WINDOW(about), parent);
  }

  gtk_window_present(GTK_WINDOW(about));
}

void addAction(GtkApplication *app, const char *name, GCallback callback,
               gpointer userData) {
  auto *action = g_simple_action_new(name, nullptr);
  g_signal_connect(action, "activate", callback, userData);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  g_object_unref(action);
}

void addDesktopAlertsAction(GtkApplication *app, AppState *state) {
  state->m_desktop_alerts_enabled =
      state->m_settings != nullptr &&
      g_settings_get_boolean(state->m_settings, "desktop-alerts");
  auto *action = g_simple_action_new_stateful(
      "desktop-alerts", nullptr,
      g_variant_new_boolean(state->m_desktop_alerts_enabled));
  g_signal_connect(action, "activate", G_CALLBACK(desktopAlertsAction), state);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  g_object_unref(action);
}

auto makeToolbarPage(GtkWidget *content, const char *title, AppState *state,
                     const bool isDevicePage) -> GtkWidget * {
  auto *toolbar = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *titleLabel = gtk_label_new(title);
  gtk_widget_add_css_class(titleLabel, "title");
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), titleLabel);

  if (isDevicePage) {
    auto *backButton = gtk_button_new_from_icon_name("go-previous-symbolic");
    state->m_back_button = backButton;
    gtk_widget_set_tooltip_text(backButton, "Back to devices");
    gtk_accessible_update_property(GTK_ACCESSIBLE(backButton),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   "Back to devices", -1);
    g_signal_connect(backButton, "clicked", G_CALLBACK(backToDevicesClicked),
                     state);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), backButton);
  }

  auto *menu = g_menu_new();
  auto *quitSection = g_menu_new();
  g_menu_append(menu, "_About Dmn Bluetooth", "app.about");
  g_menu_append(menu, "Desktop alerts", "app.desktop-alerts");
  g_menu_append(quitSection, "_Quit", "app.quit");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(quitSection));
  g_object_unref(quitSection);

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
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);

  return toolbar;
}

auto makeSidebar(AppState *state) -> GtkWidget * {
  auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(root, 12);
  gtk_widget_set_margin_bottom(root, 12);
  gtk_widget_set_margin_start(root, 12);
  gtk_widget_set_margin_end(root, 12);

  auto *explanation = gtk_label_new(
      "Choose a phone to monitor. Selection is saved separately from pairing "
      "or connecting.");
  gtk_label_set_wrap(GTK_LABEL(explanation), TRUE);
  gtk_label_set_xalign(GTK_LABEL(explanation), 0.0F);
  gtk_box_append(GTK_BOX(root), explanation);

  state->m_adapter_names = gtk_string_list_new(nullptr);
  state->m_adapter_dropdown = gtk_drop_down_new(
      G_LIST_MODEL(g_object_ref(state->m_adapter_names)), nullptr);
  gtk_widget_set_tooltip_text(
      state->m_adapter_dropdown,
      "Bluetooth adapter used for an explicit device scan");
  gtk_accessible_update_property(
      GTK_ACCESSIBLE(state->m_adapter_dropdown), GTK_ACCESSIBLE_PROPERTY_LABEL,
      "Bluetooth adapter used for an explicit device scan", -1);
  g_signal_connect(state->m_adapter_dropdown, "notify::selected",
                   G_CALLBACK(adapterChanged), state);
  gtk_box_append(GTK_BOX(root), state->m_adapter_dropdown);

  state->m_scan_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  state->m_refresh_button = gtk_button_new_with_label("Refresh devices");
  state->m_scan_button = gtk_button_new_with_label("Scan for devices");
  state->m_stop_scan_button = gtk_button_new_with_label("Stop scan");
  g_signal_connect(state->m_refresh_button, "clicked",
                   G_CALLBACK(refreshClicked), state);
  g_signal_connect(state->m_scan_button, "clicked", G_CALLBACK(scanClicked),
                   state);
  g_signal_connect(state->m_stop_scan_button, "clicked",
                   G_CALLBACK(stopScanClicked), state);
  gtk_box_append(GTK_BOX(state->m_scan_actions), state->m_refresh_button);
  gtk_box_append(GTK_BOX(state->m_scan_actions), state->m_scan_button);
  gtk_box_append(GTK_BOX(state->m_scan_actions), state->m_stop_scan_button);
  gtk_box_append(GTK_BOX(root), state->m_scan_actions);

  auto *sortHeaders = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  state->m_name_sort_button = gtk_button_new_with_label("Name ↑");
  state->m_status_sort_button = gtk_button_new_with_label("Status");
  gtk_widget_add_css_class(state->m_name_sort_button, "flat");
  gtk_widget_add_css_class(state->m_name_sort_button, "heading");
  gtk_widget_add_css_class(state->m_status_sort_button, "flat");
  gtk_widget_add_css_class(state->m_status_sort_button, "heading");
  gtk_widget_set_hexpand(state->m_name_sort_button, TRUE);
  gtk_widget_set_halign(state->m_name_sort_button, GTK_ALIGN_START);
  gtk_widget_set_halign(state->m_status_sort_button, GTK_ALIGN_END);
  g_signal_connect(state->m_name_sort_button, "clicked",
                   G_CALLBACK(nameSortClicked), state);
  g_signal_connect(state->m_status_sort_button, "clicked",
                   G_CALLBACK(statusSortClicked), state);
  gtk_box_append(GTK_BOX(sortHeaders), state->m_name_sort_button);
  gtk_box_append(GTK_BOX(sortHeaders), state->m_status_sort_button);
  gtk_box_append(GTK_BOX(root), sortHeaders);

  auto *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  state->m_device_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->m_device_list),
                                  GTK_SELECTION_NONE);
  gtk_list_box_set_show_separators(GTK_LIST_BOX(state->m_device_list), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                state->m_device_list);
  gtk_box_append(GTK_BOX(root), scroll);

  state->m_selection_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  auto *selectionButtons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  state->m_save_button = gtk_button_new_with_label("Save selection");
  state->m_discard_button = gtk_button_new_with_label("Discard changes");
  g_signal_connect(state->m_save_button, "clicked",
                   G_CALLBACK(selectionClicked), state);
  g_signal_connect(state->m_discard_button, "clicked",
                   G_CALLBACK(discardClicked), state);
  state->m_selected_only_filter =
      gtk_check_button_new_with_label("Selected only");
  if (state->m_settings != nullptr) {
    gtk_check_button_set_active(
        GTK_CHECK_BUTTON(state->m_selected_only_filter),
        g_settings_get_boolean(state->m_settings, "selected-only"));
  }

  gtk_widget_set_tooltip_text(
      state->m_selected_only_filter,
      "Show devices selected in the current selection draft");
  g_signal_connect(state->m_selected_only_filter, "toggled",
                   G_CALLBACK(selectedOnlyFilterToggled), state);
  auto *actionsSpacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(actionsSpacer, TRUE);
  gtk_box_append(GTK_BOX(selectionButtons), state->m_save_button);
  gtk_box_append(GTK_BOX(selectionButtons), state->m_discard_button);
  gtk_box_append(GTK_BOX(state->m_selection_actions), selectionButtons);
  gtk_box_append(GTK_BOX(state->m_selection_actions), actionsSpacer);
  gtk_box_append(GTK_BOX(state->m_selection_actions),
                 state->m_selected_only_filter);
  gtk_widget_set_halign(state->m_selected_only_filter, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(root), state->m_selection_actions);

  state->m_status_label = gtk_label_new("Loading Bluetooth devices…");
  gtk_label_set_wrap(GTK_LABEL(state->m_status_label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(state->m_status_label), 0.0F);
  gtk_box_append(GTK_BOX(root), state->m_status_label);

  return root;
}

auto makeDetails(AppState *state) -> GtkWidget * {
  auto *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_top(content, 24);
  gtk_widget_set_margin_bottom(content, 24);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);

  state->m_details_page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(state->m_details_page),
                                "bluetooth-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                            "Choose a Bluetooth device");
  adw_status_page_set_description(
      ADW_STATUS_PAGE(state->m_details_page),
      "Save a device selection to prepare it for monitoring. Selecting or "
      "saving a device never pairs or connects it.");
  gtk_widget_set_vexpand(state->m_details_page, FALSE);
  gtk_box_append(GTK_BOX(content), state->m_details_page);

  state->m_feed_heading = gtk_label_new("Notifications");
  gtk_label_set_xalign(GTK_LABEL(state->m_feed_heading), 0.0F);
  gtk_widget_add_css_class(state->m_feed_heading, "title-3");
  gtk_widget_set_visible(state->m_feed_heading, FALSE);
  gtk_box_append(GTK_BOX(content), state->m_feed_heading);

  createEventFeedModel(state);
  state->m_feed_controls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  auto *searchRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  state->m_feed_search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(state->m_feed_search),
                                        "Search notifications");
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->m_feed_search),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Search notifications", -1);
  gtk_widget_set_hexpand(state->m_feed_search, TRUE);
  state->m_feed_all_devices_filter =
      gtk_check_button_new_with_label("All devices");
  gtk_widget_set_tooltip_text(
      state->m_feed_all_devices_filter,
      "Include events received from every device during this app session");
  gtk_box_append(GTK_BOX(searchRow), state->m_feed_search);
  gtk_box_append(GTK_BOX(searchRow), state->m_feed_all_devices_filter);
  gtk_box_append(GTK_BOX(state->m_feed_controls), searchRow);

  auto *filterRow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  static constexpr const char *kEventKinds[]{"Any event",
                                             "Added",
                                             "Updated",
                                             "Removed",
                                             "Attributes unavailable",
                                             "Monitor state",
                                             "Monitor error",
                                             nullptr};
  static constexpr const char *kEventCategories[]{"Any category",
                                                  "Other",
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
                                                  "Entertainment",
                                                  nullptr};
  state->m_feed_kind_filter = gtk_drop_down_new(
      G_LIST_MODEL(gtk_string_list_new(kEventKinds)), nullptr);
  state->m_feed_category_filter = gtk_drop_down_new(
      G_LIST_MODEL(gtk_string_list_new(kEventCategories)), nullptr);
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->m_feed_kind_filter),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Filter by event kind", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(state->m_feed_category_filter),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 "Filter by notification category", -1);
  gtk_widget_set_hexpand(state->m_feed_kind_filter, TRUE);
  gtk_widget_set_hexpand(state->m_feed_category_filter, TRUE);
  gtk_widget_set_tooltip_text(state->m_feed_kind_filter,
                              "Filter by event kind");
  gtk_widget_set_tooltip_text(state->m_feed_category_filter,
                              "Filter by notification category");
  state->m_feed_clear_filters = gtk_button_new_with_label("Clear filters");
  state->m_feed_clear_notifications =
      gtk_button_new_with_label("Clear notifications");
  gtk_widget_set_tooltip_text(state->m_feed_clear_notifications,
                              "Clear in-memory events for this app session");
  gtk_box_append(GTK_BOX(filterRow), state->m_feed_kind_filter);
  gtk_box_append(GTK_BOX(filterRow), state->m_feed_category_filter);
  gtk_box_append(GTK_BOX(filterRow), state->m_feed_clear_filters);
  gtk_box_append(GTK_BOX(filterRow), state->m_feed_clear_notifications);
  gtk_box_append(GTK_BOX(state->m_feed_controls), filterRow);
  gtk_widget_set_visible(state->m_feed_controls, FALSE);
  gtk_box_append(GTK_BOX(content), state->m_feed_controls);

  g_signal_connect(state->m_feed_search, "search-changed",
                   G_CALLBACK(eventSearchChanged), state);
  g_signal_connect(state->m_feed_kind_filter, "notify::selected",
                   G_CALLBACK(eventFilterChanged), state);
  g_signal_connect(state->m_feed_category_filter, "notify::selected",
                   G_CALLBACK(eventFilterChanged), state);
  g_signal_connect(state->m_feed_all_devices_filter, "toggled",
                   G_CALLBACK(eventFilterToggled), state);
  g_signal_connect(state->m_feed_clear_filters, "clicked",
                   G_CALLBACK(clearEventFiltersClicked), state);
  g_signal_connect(state->m_feed_clear_notifications, "clicked",
                   G_CALLBACK(clearNotificationsClicked), state);

  state->m_feed_notice = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(state->m_feed_notice), 0.0F);
  gtk_label_set_wrap(GTK_LABEL(state->m_feed_notice), TRUE);
  gtk_widget_add_css_class(state->m_feed_notice, "dim-label");
  gtk_widget_set_visible(state->m_feed_notice, FALSE);
  gtk_box_append(GTK_BOX(content), state->m_feed_notice);

  state->m_feed_empty = gtk_label_new(nullptr);
  gtk_label_set_wrap(GTK_LABEL(state->m_feed_empty), TRUE);
  gtk_label_set_xalign(GTK_LABEL(state->m_feed_empty), 0.0F);
  gtk_widget_set_margin_top(state->m_feed_empty, 12);
  gtk_widget_set_margin_bottom(state->m_feed_empty, 12);
  gtk_widget_set_visible(state->m_feed_empty, FALSE);
  gtk_box_append(GTK_BOX(content), state->m_feed_empty);

  state->m_feed_scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(state->m_feed_scroll, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(state->m_feed_scroll),
                                state->m_feed_view);
  gtk_widget_set_visible(state->m_feed_scroll, FALSE);
  gtk_box_append(GTK_BOX(content), state->m_feed_scroll);

  auto *page = makeToolbarPage(content, "Notifications", state, true);

  return page;
}

auto createSettings() -> GSettings * { return settingsForApp(); }

auto loadPaneFraction(GSettings *settings) -> double {
  auto *storedFraction = g_settings_get_user_value(settings, "pane-fraction");
  if (storedFraction != nullptr) {
    const auto fraction =
        std::clamp(g_variant_get_double(storedFraction), 0.1, 0.9);
    g_variant_unref(storedFraction);

    return fraction;
  }

  auto *storedPosition = g_settings_get_user_value(settings, "pane-position");
  if (storedPosition == nullptr) {
    return std::clamp(g_settings_get_double(settings, "pane-fraction"), 0.1,
                      0.9);
  }

  const auto fraction =
      std::clamp(static_cast<double>(g_variant_get_int32(storedPosition)) /
                     kDefaultWindowWidth,
                 0.1, 0.9);
  g_variant_unref(storedPosition);
  if (!g_settings_set_double(settings, "pane-fraction", fraction)) {
    g_warning("Could not migrate the saved device pane width");
  }

  return fraction;
}

void activate(GtkApplication *app, gpointer) {
  auto *state = stateFor(G_OBJECT(app));
  if (state == nullptr) {
    g_message("Starting Dmn Bluetooth GTK application");
    state = new AppState{app};
    g_object_set_data_full(
        G_OBJECT(app), "dmn-bluetooth-state", state,
        [](gpointer data) { delete static_cast<AppState *>(data); });
    state->m_settings = createSettings();
    if (state->m_settings != nullptr) {
      state->m_pane_fraction = loadPaneFraction(state->m_settings);
    }

    loadSavedSelection(state);
    loadMonitoringIntents(state);

    state->m_window = adw_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(state->m_window), "Dmn Bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(state->m_window),
                                kDefaultWindowWidth, 680);
    gtk_widget_set_size_request(state->m_window, 360, 420);
    g_signal_connect(state->m_window, "close-request",
                     G_CALLBACK(windowCloseRequested), state);

    addAction(app, "about", G_CALLBACK(aboutAction), app);
    addAction(app, "quit", G_CALLBACK(quitAction), state);
    addAction(app, "open", G_CALLBACK(openAction), app);
    addDesktopAlertsAction(app, state);

    state->m_sidebar_page =
        makeToolbarPage(makeSidebar(state), "Devices", state, false);
    state->m_device_page = makeDetails(state);
    auto *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    state->m_paned = paned;
    gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_start_child(GTK_PANED(paned), state->m_sidebar_page);
    gtk_paned_set_end_child(GTK_PANED(paned), state->m_device_page);
    gtk_paned_set_position(GTK_PANED(paned), 480);
    g_signal_connect(paned, "notify::position", G_CALLBACK(panePositionChanged),
                     state);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(state->m_window),
                                       paned);

    auto *condition = adw_breakpoint_condition_parse("max-width: 600sp");
    auto *breakpoint = adw_breakpoint_new(condition);
    adw_application_window_add_breakpoint(
        ADW_APPLICATION_WINDOW(state->m_window), breakpoint);
    g_signal_connect(state->m_window, "notify::current-breakpoint",
                     G_CALLBACK(windowBreakpointChanged), state);
    windowBreakpointChanged(G_OBJECT(state->m_window), nullptr, state);

    static const char *quitAccelerators[] = {"<Primary>q", nullptr};
    static const char *aboutAccelerators[] = {"<Primary>a", nullptr};
    gtk_application_set_accels_for_action(app, "app.quit", quitAccelerators);
    gtk_application_set_accels_for_action(app, "app.about", aboutAccelerators);

    refreshView(state);
    if (state->m_settings == nullptr) {
      setStatus(state, "The application settings schema is unavailable.");
    }

    requestOperation(state, Dmn_Operation::kInventory);
    updateMonitorPolling(state);
    gtk_window_maximize(GTK_WINDOW(state->m_window));
    state->m_pane_restore_source =
        g_timeout_add(10, restorePanePosition, state);
  }

  gtk_window_present(GTK_WINDOW(state->m_window));
}

} // namespace

auto main(int argc, char **argv) -> int {
  adw_init();
  auto *app = adw_application_new(kApplicationId, G_APPLICATION_DEFAULT_FLAGS);
  g_application_set_version(G_APPLICATION(app), DMN_VERSION);
  g_signal_connect(app, "activate", G_CALLBACK(activate), nullptr);
  const int result = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return result;
}
