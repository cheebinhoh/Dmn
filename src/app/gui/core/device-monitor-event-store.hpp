/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file device-monitor-event-store.hpp
 * @brief Bounded, process-local ANCS events associated with device identities.
 *
 * The store keeps event payloads in memory only. Its identity filter lets the
 * GTK event pane display one device's stream without mixing notifications
 * from simultaneously monitored devices.
 */

#ifndef DMN_BLUETOOTH_GUI_DEVICE_MONITOR_EVENT_STORE_HPP_
#define DMN_BLUETOOTH_GUI_DEVICE_MONITOR_EVENT_STORE_HPP_

#include "device-selection-model.hpp"

#include "dmn-ancs-client.hpp"

#include <chrono>
#include <cstddef>
#include <vector>

namespace dmn::gui {

/** @brief One owned event received for a device during this process session. */
struct Dmn_DeviceMonitorEvent {
  /** @brief Stable adapter/device identity associated with the event. */
  Dmn_DeviceIdentity m_identity;

  /** @brief Host wall-clock time when the monitor callback received it. */
  std::chrono::system_clock::time_point m_received;

  /** @brief Owned ANCS event payload; retained in memory only. */
  Dmn_AncsEvent m_event;
};

/**
 * @brief Bounded event history for the current GUI process.
 *
 * Events remain in arrival order. Callers may request a device-specific view;
 * no event data is persisted to settings or disk.
 */
class Dmn_DeviceMonitorEventStore {
public:
  /**
   * @brief Create a process-local store with a finite event limit.
   *
   * @param maximumEvents Maximum records retained across all devices.
   * @throws std::invalid_argument If @p maximumEvents is zero.
   */
  explicit Dmn_DeviceMonitorEventStore(std::size_t maximumEvents = 1000);

  /** @brief Append an owned event and evict the oldest record if needed. */
  void append(Dmn_DeviceMonitorEvent event);

  /**
   * @brief Return retained events for one stable device identity.
   *
   * @param identity Adapter/device identity to select.
   * @return Pointers into this store, ordered from oldest to newest and valid
   * until the next append or store destruction.
   */
  [[nodiscard]] auto eventsFor(const Dmn_DeviceIdentity &identity) const
      -> std::vector<const Dmn_DeviceMonitorEvent *>;

  /** @brief Return the current number of retained events. */
  [[nodiscard]] auto size() const -> std::size_t;

private:
  std::size_t m_maximum_events;
  std::vector<Dmn_DeviceMonitorEvent> m_events;
};

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_GUI_DEVICE_MONITOR_EVENT_STORE_HPP_
