/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file main.cpp
 * @brief GTK application shell and asynchronous BlueZ device inventory.
 *
 * The GTK main context owns all widgets and selection state. BlueZ calls run
 * on a GTask worker through a serialized backend; completed owned snapshots
 * return to the main context before the view is updated. The first UI slice
 * deliberately stops at inventory, explicit discovery, and saved selection:
 * selecting a device does not pair, connect, or start monitoring it.
 */

#define G_LOG_DOMAIN "dmn-bluetooth-gtk"

#include "device-selection-model.hpp"

#include "dmn-bluez-client.hpp"

#include <adwaita.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr auto kApplicationId = "com.cheebinhoh.DmnBluetooth";
constexpr auto kSettingsSchemaId = "com.cheebinhoh.DmnBluetooth";
constexpr auto kDiscoveryDuration = std::chrono::seconds{30};

enum class Dmn_Operation { kInventory, kStartDiscovery, kStopDiscovery };

auto operationName(const Dmn_Operation operation) -> const char * {
  switch (operation) {
  case Dmn_Operation::kInventory:
    return "inventory refresh";
  case Dmn_Operation::kStartDiscovery:
    return "discovery start";
  case Dmn_Operation::kStopDiscovery:
    return "discovery stop";
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
};

/** @brief Serialize every BlueZ call on the application's worker thread. */
class BluezInventoryBackend {
public:
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
  auto perform(Dmn_Operation operation,
               const std::string &adapterPath) -> BackendReply {
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

/** @brief GTK-main-context-owned widgets, settings, and selection state. */
struct AppState {
  explicit AppState(GtkApplication *application)
      : m_app{application},
        m_backend{std::make_shared<BluezInventoryBackend>()}, m_selection{1} {}

  ~AppState() {
    if (m_scan_poll_source != 0) {
      g_source_remove(m_scan_poll_source);
    }

    if (m_settings != nullptr) {
      g_object_unref(m_settings);
    }

    if (m_adapter_names != nullptr) {
      g_object_unref(m_adapter_names);
    }
  }

  GtkApplication *m_app{};
  GtkWidget *m_window{};
  GtkWidget *m_split_view{};
  GtkWidget *m_device_list{};
  GtkWidget *m_adapter_dropdown{};
  GtkWidget *m_status_label{};
  GtkWidget *m_name_sort_button{};
  GtkWidget *m_status_sort_button{};
  GtkWidget *m_refresh_button{};
  GtkWidget *m_scan_button{};
  GtkWidget *m_stop_scan_button{};
  GtkWidget *m_save_button{};
  GtkWidget *m_discard_button{};
  GtkWidget *m_details_page{};
  GtkStringList *m_adapter_names{};
  GSettings *m_settings{};
  std::shared_ptr<BluezInventoryBackend> m_backend;
  dmn::gui::Dmn_DeviceSelectionModel m_selection;
  std::vector<AdapterOption> m_adapters;
  std::string m_selected_adapter_address;
  bool m_busy{};
  bool m_scanning{};
  bool m_rendering{};
  bool m_close_requested{};
  bool m_inventory_loaded{};
  std::string m_inventory_error;
  guint m_scan_poll_source{};
  std::chrono::steady_clock::time_point m_scan_deadline{};
};

/** @brief Owned arguments copied into a GTask worker invocation. */
struct TaskData {
  std::shared_ptr<BluezInventoryBackend> m_backend;
  Dmn_Operation m_operation{Dmn_Operation::kInventory};
  std::string m_adapter_path;
};

/** @brief Callback context identifying one selection row and its owner. */
struct RowContext {
  AppState *m_state{};
  dmn::gui::Dmn_DeviceIdentity m_identity;
};

auto stateFor(GObject *object) -> AppState * {
  return static_cast<AppState *>(
      g_object_get_data(object, "dmn-bluetooth-state"));
}

void refreshView(AppState *state, bool rebuildDevices = true);
void requestOperation(AppState *state, Dmn_Operation operation,
                      std::string adapterPath = {});
void requestClose(AppState *state);

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
                           !state->m_adapters.empty() && !state->m_busy);
  gtk_widget_set_sensitive(state->m_scan_button, !state->m_adapters.empty() &&
                                                     !state->m_busy &&
                                                     !state->m_scanning);
}

void setStatus(AppState *state, const std::string &message) {
  gtk_label_set_text(GTK_LABEL(state->m_status_label), message.c_str());
  if (gtk_widget_get_mapped(state->m_status_label)) {
    gtk_accessible_announce(GTK_ACCESSIBLE(state->m_status_label),
                            message.c_str(),
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  }
}

void updateButtonState(AppState *state) {
  gtk_widget_set_sensitive(state->m_refresh_button, !state->m_busy);
  gtk_widget_set_sensitive(state->m_save_button,
                           !state->m_busy &&
                               state->m_selection.hasUnsavedChanges());
  gtk_widget_set_sensitive(state->m_discard_button,
                           !state->m_busy &&
                               state->m_selection.hasUnsavedChanges());
  gtk_widget_set_sensitive(state->m_scan_button,
                           !state->m_busy && !state->m_scanning &&
                               !state->m_adapters.empty());
  gtk_widget_set_sensitive(state->m_stop_scan_button,
                           !state->m_busy && state->m_scanning);
  gtk_widget_set_sensitive(state->m_adapter_dropdown,
                           !state->m_busy && !state->m_adapters.empty());
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
    setStatus(state,
              "Only one monitoring device is supported in this release.");

    return;
  }

  if (selected) {
    adw_navigation_split_view_set_show_content(
        ADW_NAVIGATION_SPLIT_VIEW(state->m_split_view), TRUE);
  }

  refreshView(state, false);
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

auto isSelected(const std::vector<dmn::gui::Dmn_DeviceIdentity> &selection,
                const dmn::gui::Dmn_DeviceIdentity &identity) -> bool {
  return std::find(selection.begin(), selection.end(), identity) !=
         selection.end();
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

  for (const auto &device : state->m_selection.devices()) {
    auto *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(row, 10);
    gtk_widget_set_margin_bottom(row, 10);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 12);

    const auto title = dmn::gui::deviceDisplayName(device);
    auto *check = gtk_check_button_new_with_label(title.c_str());
    const bool selected =
        isSelected(state->m_selection.draftSelection(), device.m_identity);
    const bool saved =
        isSelected(state->m_selection.savedSelection(), device.m_identity);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), selected);
    gtk_widget_set_sensitive(check, device.m_present || selected || saved);
    gtk_widget_set_hexpand(check, TRUE);

    const auto facts = dmn::gui::deviceDisplayStatus(device);
    auto *detail = gtk_label_new(facts.c_str());
    gtk_widget_set_halign(detail, GTK_ALIGN_END);
    gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
    gtk_label_set_single_line_mode(GTK_LABEL(detail), TRUE);
    gtk_widget_set_size_request(detail, 120, -1);
    gtk_widget_add_css_class(detail, "dim-label");

    gtk_box_append(GTK_BOX(row), check);
    gtk_box_append(GTK_BOX(row), detail);
    gtk_list_box_append(GTK_LIST_BOX(state->m_device_list), row);

    auto *context = new RowContext{state, device.m_identity};
    g_signal_connect_data(check, "toggled", G_CALLBACK(rowToggled), context,
                          rowContextDestroy, G_CONNECT_DEFAULT);
  }

  state->m_rendering = false;
}

void synchronizeDeviceListSelection(AppState *state) {
  const auto &devices = state->m_selection.devices();
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
    auto *check = gtk_widget_get_first_child(content);
    const auto &device = devices[index];
    const bool selected =
        isSelected(state->m_selection.draftSelection(), device.m_identity);
    const bool saved =
        isSelected(state->m_selection.savedSelection(), device.m_identity);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), selected);
    gtk_widget_set_sensitive(GTK_WIDGET(check),
                             device.m_present || selected || saved);
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

    return;
  }

  if (selected.empty()) {
    adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                              "Choose a Bluetooth device");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(state->m_details_page),
        "Save a device selection to prepare it for monitoring. Selecting or "
        "saving a device never pairs or connects it.");

    return;
  }

  adw_status_page_set_title(ADW_STATUS_PAGE(state->m_details_page),
                            "Device selection saved");
  adw_status_page_set_description(
      ADW_STATUS_PAGE(state->m_details_page),
      "This inventory-and-selection build does not start pairing, connection, "
      "or monitoring.");
}

void loadSavedSelection(AppState *state) {
  if (state->m_settings == nullptr) {
    return;
  }

  auto *savedValue = g_settings_get_value(state->m_settings, "selected-device");
  if (savedValue == nullptr) {
    g_warning("Could not read the saved Bluetooth device selection");

    return;
  }

  const char *adapterAddress = nullptr;
  const char *deviceAddress = nullptr;
  g_variant_get(savedValue, "(&s&s)", &adapterAddress, &deviceAddress);
  const std::string adapter{adapterAddress};
  const std::string device{deviceAddress};
  g_variant_unref(savedValue);
  if (!adapter.empty() && !device.empty()) {
    state->m_selection.loadSavedSelection({{adapter, device}});
  }
}

void saveSelection(AppState *state) {
  if (state->m_settings == nullptr) {
    setStatus(state,
              "Settings are unavailable; device selection was not saved.");

    return;
  }

  const auto &selection = state->m_selection.draftSelection();
  const char *adapter =
      selection.empty() ? "" : selection.front().m_adapter_address.c_str();
  const char *device =
      selection.empty() ? "" : selection.front().m_device_address.c_str();
  auto *value = g_variant_new("(ss)", adapter, device);
  if (!g_settings_set_value(state->m_settings, "selected-device", value)) {
    g_warning("Could not persist the Bluetooth device selection");
    setStatus(state, "The device selection could not be saved to settings.");

    return;
  }

  state->m_selection.saveSelection();
  setStatus(state, selection.empty() ? "Device selection cleared."
                                     : "Device selection saved.");
  refreshView(state, false);
}

auto settingsForApp() -> GSettings * {
  GError *error = nullptr;
  auto *source = g_settings_schema_source_new_from_directory(
      DMN_BLUETOOTH_GTK_SCHEMA_DIR, g_settings_schema_source_get_default(),
      FALSE, &error);
  if (source == nullptr) {
    if (error != nullptr) {
      g_warning("Could not load the build-time settings schema: %s",
                error->message);
    }

    g_clear_error(&error);
    source = g_settings_schema_source_get_default();
    if (source == nullptr) {
      g_warning("No GSettings schema source is available");

      return nullptr;
    }

    auto *schema =
        g_settings_schema_source_lookup(source, kSettingsSchemaId, TRUE);
    if (schema == nullptr) {
      g_warning("GSettings schema '%s' was not found", kSettingsSchemaId);

      return nullptr;
    }

    auto *settings = g_settings_new_full(schema, nullptr, nullptr);
    g_settings_schema_unref(schema);

    return settings;
  }

  auto *schema =
      g_settings_schema_source_lookup(source, kSettingsSchemaId, TRUE);
  g_settings_schema_source_unref(source);
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
      data->m_backend->perform(data->m_operation, data->m_adapter_path);
  g_task_return_pointer(
      task, new BackendReply{std::move(reply)},
      [](gpointer result) { delete static_cast<BackendReply *>(result); });
}

void applyInventory(AppState *state, const dmn::Dmn_BluezInventory &inventory) {
  g_debug("Received Bluetooth inventory: %zu adapter(s), %zu device(s)",
          inventory.adapters.size(), inventory.devices.size());
  updateAdapters(state, inventory);
  state->m_selection.replaceInventory(toGuiInventory(inventory));
  state->m_inventory_loaded = true;
  state->m_inventory_error.clear();
  refreshView(state);
  setStatus(state, state->m_scanning ? "Nearby Bluetooth devices updated."
                                     : "Bluetooth devices updated.");
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
    requestOperation(state, Dmn_Operation::kInventory);
  }

  return G_SOURCE_CONTINUE;
}

void operationFinished(GObject *sourceObject, GAsyncResult *result, gpointer) {
  auto *state = stateFor(sourceObject);
  GError *error = nullptr;
  auto *reply = static_cast<BackendReply *>(
      g_task_propagate_pointer(G_TASK(result), &error));
  const bool stopFailed = reply != nullptr &&
                          reply->m_operation == Dmn_Operation::kStopDiscovery &&
                          !reply->m_error.empty();

  state->m_busy = false;
  if (error != nullptr) {
    g_warning("Bluetooth background task failed: %s", error->message);
    setStatus(state, error->message);
    g_error_free(error);
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

  if (state->m_close_requested && stopFailed) {
    state->m_close_requested = false;
    updateButtonState(state);

    return;
  }

  if (state->m_close_requested) {
    if (state->m_scanning) {
      requestOperation(state, Dmn_Operation::kStopDiscovery);

      return;
    }

    g_application_quit(G_APPLICATION(state->m_app));

    return;
  }

  updateButtonState(state);
}

void requestOperation(AppState *state, const Dmn_Operation operation,
                      std::string adapterPath) {
  if (state->m_busy) {
    return;
  }

  if (operation == Dmn_Operation::kStartDiscovery && adapterPath.empty()) {
    setStatus(state, "Choose a powered Bluetooth adapter before scanning.");

    return;
  }

  state->m_busy = true;
  g_debug("Starting Bluetooth %s", operationName(operation));
  if (operation == Dmn_Operation::kInventory && state->m_scanning) {
    setStatus(state, "Updating nearby Bluetooth devices…");
  } else if (operation == Dmn_Operation::kInventory) {
    setStatus(state, "Refreshing Bluetooth devices…");
  } else if (operation == Dmn_Operation::kStartDiscovery) {
    setStatus(state, "Starting Bluetooth scan…");
  } else {
    setStatus(state, "Stopping Bluetooth scan…");
  }

  updateButtonState(state);

  auto *task =
      g_task_new(G_OBJECT(state->m_app), nullptr, operationFinished, nullptr);
  auto *data =
      new TaskData{state->m_backend, operation, std::move(adapterPath)};
  g_task_set_task_data(task, data, [](gpointer taskData) {
    delete static_cast<TaskData *>(taskData);
  });
  g_task_run_in_thread(task, taskWorker);
  g_object_unref(task);
}

void refreshClicked(GtkButton *, gpointer userData) {
  requestOperation(static_cast<AppState *>(userData),
                   Dmn_Operation::kInventory);
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
                   state->m_adapters[selected].m_path);
}

void stopScanClicked(GtkButton *, gpointer userData) {
  requestOperation(static_cast<AppState *>(userData),
                   Dmn_Operation::kStopDiscovery);
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
  if (state->m_busy) {
    setStatus(state, "Waiting for the current Bluetooth operation to finish "
                     "before quitting.");

    return;
  }

  if (state->m_scanning) {
    requestOperation(state, Dmn_Operation::kStopDiscovery);

    return;
  }

  g_application_quit(G_APPLICATION(state->m_app));
}

void quitAction(GSimpleAction *, GVariant *, gpointer userData) {
  requestClose(static_cast<AppState *>(userData));
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
  adw_about_window_set_application_icon(about, "bluetooth-symbolic");
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

auto makeToolbarPage(GtkWidget *content, const char *title) -> GtkWidget * {
  auto *toolbar = adw_toolbar_view_new();
  auto *header = adw_header_bar_new();
  auto *menu = g_menu_new();
  auto *quitSection = g_menu_new();
  g_menu_append(menu, "_About Dmn Bluetooth", "app.about");
  g_menu_append(quitSection, "_Quit", "app.quit");
  g_menu_append_section(menu, nullptr, G_MENU_MODEL(quitSection));
  g_object_unref(quitSection);

  auto *menuButton = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menuButton),
                                "open-menu-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menuButton),
                                 G_MENU_MODEL(menu));
  gtk_widget_set_tooltip_text(menuButton, "Main Menu");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), menuButton);

  g_object_unref(menu);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);

  return GTK_WIDGET(adw_navigation_page_new(toolbar, title));
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
  g_signal_connect(state->m_adapter_dropdown, "notify::selected",
                   G_CALLBACK(adapterChanged), state);
  gtk_box_append(GTK_BOX(root), state->m_adapter_dropdown);

  auto *scan_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  state->m_refresh_button = gtk_button_new_with_label("Refresh devices");
  state->m_scan_button = gtk_button_new_with_label("Scan for devices");
  state->m_stop_scan_button = gtk_button_new_with_label("Stop scan");
  g_signal_connect(state->m_refresh_button, "clicked",
                   G_CALLBACK(refreshClicked), state);
  g_signal_connect(state->m_scan_button, "clicked", G_CALLBACK(scanClicked),
                   state);
  g_signal_connect(state->m_stop_scan_button, "clicked",
                   G_CALLBACK(stopScanClicked), state);
  gtk_box_append(GTK_BOX(scan_actions), state->m_refresh_button);
  gtk_box_append(GTK_BOX(scan_actions), state->m_scan_button);
  gtk_box_append(GTK_BOX(scan_actions), state->m_stop_scan_button);
  gtk_box_append(GTK_BOX(root), scan_actions);

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

  auto *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  state->m_save_button = gtk_button_new_with_label("Save selection");
  state->m_discard_button = gtk_button_new_with_label("Discard changes");
  g_signal_connect(state->m_save_button, "clicked",
                   G_CALLBACK(selectionClicked), state);
  g_signal_connect(state->m_discard_button, "clicked",
                   G_CALLBACK(discardClicked), state);
  gtk_box_append(GTK_BOX(actions), state->m_save_button);
  gtk_box_append(GTK_BOX(actions), state->m_discard_button);
  gtk_box_append(GTK_BOX(root), actions);

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
  gtk_widget_set_vexpand(state->m_details_page, TRUE);
  gtk_box_append(GTK_BOX(content), state->m_details_page);

  auto *page = makeToolbarPage(content, "Device");

  return page;
}

auto createSettings() -> GSettings * { return settingsForApp(); }

void activate(GtkApplication *app, gpointer) {
  auto *state = stateFor(G_OBJECT(app));
  if (state == nullptr) {
    g_message("Starting Dmn Bluetooth GTK application");
    state = new AppState{app};
    g_object_set_data_full(
        G_OBJECT(app), "dmn-bluetooth-state", state,
        [](gpointer data) { delete static_cast<AppState *>(data); });
    state->m_settings = createSettings();
    loadSavedSelection(state);

    state->m_window = adw_application_window_new(GTK_APPLICATION(app));
    gtk_window_set_title(GTK_WINDOW(state->m_window), "Dmn Bluetooth");
    gtk_window_set_default_size(GTK_WINDOW(state->m_window), 960, 680);
    gtk_widget_set_size_request(state->m_window, 360, 420);
    g_signal_connect(state->m_window, "close-request",
                     G_CALLBACK(windowCloseRequested), state);

    addAction(app, "about", G_CALLBACK(aboutAction), app);
    addAction(app, "quit", G_CALLBACK(quitAction), state);

    auto *sidebar = makeToolbarPage(makeSidebar(state), "Devices");
    auto *details = makeDetails(state);
    auto *split = adw_navigation_split_view_new();
    state->m_split_view = split;
    adw_navigation_split_view_set_sidebar(ADW_NAVIGATION_SPLIT_VIEW(split),
                                          ADW_NAVIGATION_PAGE(sidebar));
    adw_navigation_split_view_set_content(ADW_NAVIGATION_SPLIT_VIEW(split),
                                          ADW_NAVIGATION_PAGE(details));
    adw_navigation_split_view_set_sidebar_width_fraction(
        ADW_NAVIGATION_SPLIT_VIEW(split), 0.35);
    adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(split),
                                               FALSE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(state->m_window),
                                       split);

    auto *condition = adw_breakpoint_condition_parse("max-width: 600sp");
    auto *breakpoint = adw_breakpoint_new(condition);
    adw_breakpoint_add_setters(breakpoint, G_OBJECT(split), "collapsed", TRUE,
                               nullptr);
    adw_application_window_add_breakpoint(
        ADW_APPLICATION_WINDOW(state->m_window), breakpoint);

    static const char *quitAccelerators[] = {"<Primary>q", nullptr};
    static const char *aboutAccelerators[] = {"<Primary>a", nullptr};
    gtk_application_set_accels_for_action(app, "app.quit", quitAccelerators);
    gtk_application_set_accels_for_action(app, "app.about", aboutAccelerators);

    refreshView(state);
    if (state->m_settings == nullptr) {
      setStatus(state, "The application settings schema is unavailable.");
    }

    requestOperation(state, Dmn_Operation::kInventory);
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
