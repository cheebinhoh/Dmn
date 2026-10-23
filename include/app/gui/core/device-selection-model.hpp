/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file device-selection-model.hpp
 * @brief Public GTK-independent device inventory and selection state.
 *
 * The model separates durable selection rules from GTK widgets and BlueZ
 * transport. The UI supplies owned inventory snapshots, edits a draft, and
 * saves or discards it independently of device availability.
 */

#ifndef DMN_BLUETOOTH_GUI_DEVICE_SELECTION_MODEL_HPP_
#define DMN_BLUETOOTH_GUI_DEVICE_SELECTION_MODEL_HPP_

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace dmn::gui {

/**
 * @brief Stable identity for a Bluetooth device on a specific adapter.
 *
 * Device object paths are intentionally excluded because BlueZ may change
 * them when the device is rediscovered.
 */
struct Dmn_DeviceIdentity {
  /**
   * @brief Compare this identity with another.
   *
   * @param other Identity to compare against.
   * @return `true` if both Bluetooth addresses match.
   */
  auto operator==(const Dmn_DeviceIdentity &other) const -> bool = default;

  /**
   * @brief Bluetooth address of the adapter that owns the device.
   */
  std::string m_adapter_address;

  /**
   * @brief Bluetooth address identifying the device on that adapter.
   */
  std::string m_device_address;
};

/**
 * @brief Display and availability state for one known Bluetooth device.
 */
struct Dmn_DeviceEntry {
  /**
   * @brief Stable device identity used to reconcile snapshots and settings.
   */
  Dmn_DeviceIdentity m_identity;

  /**
   * @brief Preferred human-readable name, possibly empty.
   */
  std::string m_display_name;

  /**
   * @brief Pairing state; absent means unknown or unavailable.
   */
  std::optional<bool> m_paired;

  /**
   * @brief Connection state; absent means unknown or unavailable.
   */
  std::optional<bool> m_connected;

  /**
   * @brief Whether the device is present in the latest BlueZ snapshot.
   */
  bool m_present{};

  /**
   * @brief Whether BlueZ observed this device during discovery.
   */
  bool m_seen_during_discovery{};
};

/**
 * @brief Inputs used to decide which actions a device row may expose.
 */
struct Dmn_DeviceActionContext {
  /**
   * @brief Whether this identity is in the saved selection.
   */
  bool m_saved{};

  /**
   * @brief Whether the editable draft differs from saved settings.
   */
  bool m_selection_dirty{};

  /**
   * @brief Whether a monitor object is active for this device.
   */
  bool m_monitor_active{};

  /**
   * @brief Whether this device has saved monitoring intent.
   */
  bool m_monitor_requested{};

  /**
   * @brief Whether this device's monitor is starting or stopping.
   */
  bool m_monitor_busy{};

  /**
   * @brief Whether this device's monitor startup may be canceled.
   */
  bool m_monitor_start_pending{};

  /**
   * @brief Whether cancellation of this startup was already requested.
   */
  bool m_monitor_cancel_requested{};

  /**
   * @brief Whether this device has a pending connection attempt.
   */
  bool m_connection_pending{};

  /**
   * @brief Whether a disconnect is queued for this device.
   */
  bool m_disconnect_pending{};

  /**
   * @brief Whether a connect is queued until this device's monitor stops.
   */
  bool m_connect_after_monitor_stop{};

  /**
   * @brief Whether a shared Bluetooth operation is in progress.
   */
  bool m_operation_busy{};

  /**
   * @brief Whether application shutdown has been requested.
   */
  bool m_close_requested{};
};

/**
 * @brief Availability of the four per-device row actions.
 */
struct Dmn_DeviceActionAvailability {
  /**
   * @brief Whether the device may be connected.
   */
  bool m_can_connect{};

  /**
   * @brief Whether the device may be disconnected.
   */
  bool m_can_disconnect{};

  /**
   * @brief Whether ANCS monitoring may be started.
   */
  bool m_can_start_monitoring{};

  /**
   * @brief Whether monitoring intent or pending startup may be stopped.
   */
  bool m_can_stop_monitoring{};
};

/**
 * @brief Device property used to order the inventory for display.
 */
enum class Dmn_DeviceSortColumn {
  kName,   /**< Sort by the displayed device name. */
  kStatus, /**< Sort by the displayed connection/pairing status. */
};

/**
 * @brief Return the visible device name, falling back to its address.
 *
 * @param device Device whose display name is requested.
 * @return The preferred name or stable Bluetooth address.
 */
[[nodiscard]] auto
deviceDisplayName(const Dmn_DeviceEntry &device) -> std::string;

/**
 * @brief Return the concise status text shown beside a device name.
 *
 * @param device Device whose current availability and status are described.
 * @return Human-readable status text consistent with the device list.
 */
[[nodiscard]] auto
deviceDisplayStatus(const Dmn_DeviceEntry &device) -> std::string;

/**
 * @brief Derive row actions from saved selection and live device state.
 *
 * Connection actions remain independent of monitor activity and backend busy
 * state. Monitoring actions follow saved user intent and confirmed connection
 * state rather than transient ANCS state; Stop remains available while the
 * selection is dirty so an active intent can still be cleared.
 *
 * @param device Current device facts.
 * @param context Saved/draft and asynchronous operation state.
 * @return Action sensitivity for the device row.
 */
[[nodiscard]] auto deviceActionAvailability(
    const Dmn_DeviceEntry &device,
    const Dmn_DeviceActionContext &context) -> Dmn_DeviceActionAvailability;

/**
 * @brief GTK-independent inventory and saved/draft device selection.
 *
 * Saved devices remain represented when absent from the latest inventory, but
 * cannot be newly selected or used to start monitoring while unavailable.
 */
class Dmn_DeviceSelectionModel {
public:
  /**
   * @brief Construct a model with a maximum number of selected devices.
   *
   * @param selectionLimit Maximum number of devices the user may select.
   * Defaults to no selection-count limit.
   * @throws std::invalid_argument If @p selectionLimit is zero.
   */
  explicit Dmn_DeviceSelectionModel(
      std::size_t selectionLimit = std::numeric_limits<std::size_t>::max());

  /**
   * @brief Replace the current owned inventory snapshot.
   *
   * Addresses are normalized to uppercase. Absent devices are retained only
   * when they are part of the saved selection.
   *
   * @param inventory The latest BlueZ device inventory.
   */
  void replaceInventory(std::vector<Dmn_DeviceEntry> inventory);

  /**
   * @brief Load and normalize a persisted selection as both saved and draft.
   *
   * Empty identities and duplicates are ignored; values beyond the configured
   * selection limit are discarded.
   *
   * @param selection Persisted adapter/device address pairs.
   */
  void loadSavedSelection(std::vector<Dmn_DeviceIdentity> selection);

  /**
   * @brief Add or remove an available device from the editable draft.
   *
   * @param identity Stable adapter/device identity.
   * @param selected Whether the identity should be selected.
   * @return `false` if adding the device would exceed the limit or the device
   * is not present. An already-selected, unavailable device may still be
   * removed from the draft.
   */
  [[nodiscard]] auto setDraftSelected(const Dmn_DeviceIdentity &identity,
                                      bool selected) -> bool;

  /**
   * @brief Make the current draft the saved selection.
   */
  void saveSelection();

  /**
   * @brief Restore the draft from the saved selection.
   */
  void discardChanges();

  /**
   * @brief Return the current devices in display order.
   */
  [[nodiscard]] auto devices() const -> const std::vector<Dmn_DeviceEntry> &;

  /**
   * @brief Order the device inventory by the selected column and direction.
   *
   * Equal values retain a deterministic name/address tie-break order.
   *
   * @param column Device property used as the primary sort key.
   * @param ascending Whether primary values are ordered ascending.
   */
  void sortDevices(Dmn_DeviceSortColumn column, bool ascending);

  /**
   * @brief Return the column currently used to order the inventory.
   */
  [[nodiscard]] auto sortColumn() const -> Dmn_DeviceSortColumn;

  /**
   * @brief Return whether the current sort is ascending.
   */
  [[nodiscard]] auto sortAscending() const -> bool;

  /**
   * @brief Return the persisted selection.
   */
  [[nodiscard]] auto
  savedSelection() const -> const std::vector<Dmn_DeviceIdentity> &;

  /**
   * @brief Return the current editable selection.
   */
  [[nodiscard]] auto
  draftSelection() const -> const std::vector<Dmn_DeviceIdentity> &;

  /**
   * @brief Return whether the draft differs from the saved selection.
   */
  [[nodiscard]] auto hasUnsavedChanges() const -> bool;

  /**
   * @brief Return whether saved selection is current and all devices exist.
   *
   * @return `true` only when there are no edits, selection is non-empty, and
   * every saved device is present in the latest inventory.
   */
  [[nodiscard]] auto canStartMonitoring() const -> bool;

  /**
   * @brief Return the configured maximum selection size.
   */
  [[nodiscard]] auto selectionLimit() const -> std::size_t;

private:
  void mergeSavedDevices();
  void applySort();

  std::size_t m_selection_limit;
  std::vector<Dmn_DeviceEntry> m_devices;
  std::vector<Dmn_DeviceIdentity> m_saved_selection;
  std::vector<Dmn_DeviceIdentity> m_draft_selection;
  Dmn_DeviceSortColumn m_sort_column{Dmn_DeviceSortColumn::kName};
  bool m_sort_ascending{true};
};

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_GUI_DEVICE_SELECTION_MODEL_HPP_
