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

#include "app/gui/core/device-selection-model.hpp"

#include "dmn-ancs-client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dmn::gui {

/**
 * @brief One owned event received for a device during this process session.
 */
struct Dmn_DeviceMonitorEvent {
  /**
   * @brief Stable adapter/device identity associated with the event.
   */
  Dmn_DeviceIdentity m_identity;

  /**
   * @brief Monotonic arrival sequence assigned by the callback or store.
   *
   * The sequence, rather than wall-clock time or dispatch order, defines event
   * order.
   */
  std::uint64_t m_sequence{};

  /**
   * @brief Host wall-clock time when the monitor callback received it.
   */
  std::chrono::system_clock::time_point m_received;

  /**
   * @brief Owned ANCS event payload; retained in memory only.
   */
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

  /**
   * @brief Append an owned event and evict the oldest record if needed.
   */
  void append(Dmn_DeviceMonitorEvent event);

  /**
   * @brief Return retained events for one stable device identity.
   *
   * @param identity Adapter/device identity to select.
   * @return Pointers into this store, ordered from oldest to newest and valid
   * until the next append, clear, or store destruction.
   */
  [[nodiscard]] auto eventsFor(const Dmn_DeviceIdentity &identity) const
      -> std::vector<const Dmn_DeviceMonitorEvent *>;

  /**
   * @brief Return all retained events in arrival order.
   */
  [[nodiscard]] auto
  events() const -> std::vector<const Dmn_DeviceMonitorEvent *>;

  /**
   * @brief Find an event by its stable arrival sequence.
   *
   * @param sequence Sequence assigned by append().
   * @return The retained event, or nullptr if it was cleared or evicted. The
   * pointer is invalidated by a later append, clear, or store destruction.
   */
  [[nodiscard]] auto eventForSequence(std::uint64_t sequence) const
      -> const Dmn_DeviceMonitorEvent *;

  /**
   * @brief Remove all retained events and return the number removed.
   */
  auto clear() -> std::size_t;

  /**
   * @brief Return the current number of retained events.
   */
  [[nodiscard]] auto size() const -> std::size_t;

  /**
   * @brief Return the total count evicted due to the retention limit.
   */
  [[nodiscard]] auto evictedCount() const -> std::size_t;

private:
  std::size_t m_maximum_events;
  std::uint64_t m_next_sequence{1};
  std::size_t m_evicted_events{};
  std::vector<Dmn_DeviceMonitorEvent> m_events;
};

/**
 * @brief Composable in-memory filters for the event feed.
 */
struct Dmn_DeviceMonitorEventFilter {
  /**
   * @brief Optional device identity; absent includes every device.
   */
  std::optional<Dmn_DeviceIdentity> m_identity;

  /**
   * @brief Optional exact ANCS event kind.
   */
  std::optional<Dmn_AncsEvent::Kind> m_kind;

  /**
   * @brief Optional raw ANCS category value.
   */
  std::optional<std::uint8_t> m_category;

  /**
   * @brief Case-insensitive text query; event payloads are never persisted.
   */
  std::string m_search;
};

/**
 * @brief Test an event against all enabled filter dimensions.
 *
 * @param event Event to evaluate.
 * @param filter Active feed filters.
 * @param deviceName Optional display name included in text search.
 * @return true if every active filter matches.
 */
[[nodiscard]] auto
matchesEventFilter(const Dmn_DeviceMonitorEvent &event,
                   const Dmn_DeviceMonitorEventFilter &filter,
                   std::string_view deviceName = {}) -> bool;

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_GUI_DEVICE_MONITOR_EVENT_STORE_HPP_
