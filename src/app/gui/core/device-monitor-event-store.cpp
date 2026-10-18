/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file device-monitor-event-store.cpp
 * @brief Bounded, process-local ANCS events associated with device identities.
 */

#include "device-monitor-event-store.hpp"

#include <stdexcept>
#include <utility>

namespace dmn::gui {

Dmn_DeviceMonitorEventStore::Dmn_DeviceMonitorEventStore(
    const std::size_t maximumEvents)
    : m_maximum_events{maximumEvents} {
  if (m_maximum_events == 0) {
    throw std::invalid_argument{"monitor event limit must be positive"};
  }
}

void Dmn_DeviceMonitorEventStore::append(Dmn_DeviceMonitorEvent event) {
  m_events.push_back(std::move(event));
  if (m_events.size() > m_maximum_events) {
    m_events.erase(m_events.begin());
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

auto Dmn_DeviceMonitorEventStore::size() const -> std::size_t {
  return m_events.size();
}

} // namespace dmn::gui
