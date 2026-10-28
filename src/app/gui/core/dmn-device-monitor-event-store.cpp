/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-device-monitor-event-store.cpp
 * @brief Bounded, process-local ANCS events associated with device identities.
 */

#include "app/gui/core/dmn-device-monitor-event-store.hpp"

#include <glib.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace dmn::gui {

namespace {

auto caseFold(const std::string_view text) -> std::string {
  if (text.empty()) {
    return {};
  }

  auto *valid =
      g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
  auto *folded = g_utf8_casefold(valid, -1);
  std::string result{folded};
  g_free(folded);
  g_free(valid);

  return result;
}

} // namespace

Dmn_DeviceMonitorEventStore::Dmn_DeviceMonitorEventStore(
    const std::size_t maximumEvents)
    : m_maximum_events{maximumEvents} {
  if (m_maximum_events == 0) {
    throw std::invalid_argument{"monitor event limit must be positive"};
  }
}

void Dmn_DeviceMonitorEventStore::append(Dmn_DeviceMonitorEvent event) {
  if (event.m_sequence == 0) {
    event.m_sequence = m_next_sequence;
  }

  m_next_sequence = std::max(m_next_sequence, event.m_sequence + 1);
  const auto position =
      std::upper_bound(m_events.begin(), m_events.end(), event.m_sequence,
                       [](const auto sequence, const auto &candidate) {
                         return sequence < candidate.m_sequence;
                       });
  m_events.insert(position, std::move(event));
  if (m_events.size() > m_maximum_events) {
    m_events.erase(m_events.begin());
    ++m_evicted_events;
  }
}

auto Dmn_DeviceMonitorEventStore::eventsFor(const Dmn_DeviceIdentity &identity)
    const -> std::vector<const Dmn_DeviceMonitorEvent *> {
  std::vector<const Dmn_DeviceMonitorEvent *> result;
  for (const auto &event : m_events) {
    if (event.m_identity == identity) {
      result.push_back(&event);
    }
  }

  return result;
}

auto Dmn_DeviceMonitorEventStore::events() const
    -> std::vector<const Dmn_DeviceMonitorEvent *> {
  std::vector<const Dmn_DeviceMonitorEvent *> result;
  result.reserve(m_events.size());
  for (const auto &event : m_events) {
    result.push_back(&event);
  }

  return result;
}

auto Dmn_DeviceMonitorEventStore::eventForSequence(
    const std::uint64_t sequence) const -> const Dmn_DeviceMonitorEvent * {
  const auto event = std::find_if(m_events.begin(), m_events.end(),
                                  [sequence](const auto &candidate) {
                                    return candidate.m_sequence == sequence;
                                  });

  return event == m_events.end() ? nullptr : &*event;
}

auto Dmn_DeviceMonitorEventStore::clear() -> std::size_t {
  const auto removed = m_events.size();
  m_events.clear();
  m_evicted_events = 0;

  return removed;
}

auto Dmn_DeviceMonitorEventStore::size() const -> std::size_t {
  return m_events.size();
}

auto Dmn_DeviceMonitorEventStore::evictedCount() const -> std::size_t {
  return m_evicted_events;
}

auto matchesEventFilter(const Dmn_DeviceMonitorEvent &event,
                        const Dmn_DeviceMonitorEventFilter &filter,
                        const std::string_view deviceName) -> bool {
  if (filter.m_identity && event.m_identity != *filter.m_identity) {
    return false;
  }

  if (filter.m_kind && event.m_event.kind != *filter.m_kind) {
    return false;
  }

  if (filter.m_category &&
      (!event.m_event.notification ||
       event.m_event.notification->category != *filter.m_category)) {
    return false;
  }

  if (filter.m_search.empty()) {
    return true;
  }

  const auto query = caseFold(filter.m_search);
  const auto containsQuery = [&query](const std::string_view value) {
    return caseFold(value).find(query) != std::string::npos;
  };
  if (containsQuery(deviceName) ||
      containsQuery(event.m_identity.m_adapter_address) ||
      containsQuery(event.m_identity.m_device_address)) {
    return true;
  }

  if (event.m_event.notification) {
    const auto &notification = *event.m_event.notification;
    if ((notification.app_identifier &&
         containsQuery(*notification.app_identifier)) ||
        (notification.title && containsQuery(*notification.title)) ||
        (notification.subtitle && containsQuery(*notification.subtitle)) ||
        (notification.message && containsQuery(*notification.message)) ||
        (notification.date && containsQuery(*notification.date))) {
      return true;
    }
  }

  if (event.m_event.error && containsQuery(event.m_event.error->message)) {
    return true;
  }

  return false;
}

} // namespace dmn::gui
