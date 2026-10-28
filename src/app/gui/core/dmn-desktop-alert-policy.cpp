/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-desktop-alert-policy.cpp
 * @brief Privacy-safe aggregation policy for optional desktop alerts.
 */

#include "app/gui/core/dmn-desktop-alert-policy.hpp"

namespace dmn::gui {

auto shouldSendDesktopAlert(const Dmn_AncsEvent &event) -> bool {
  return event.kind == Dmn_AncsEvent::Kind::NotificationAdded;
}

auto makeDesktopAlert(const Dmn_DeviceIdentity &identity,
                      const std::size_t count) -> Dmn_DesktopAlert {
  const auto id =
      "device:" + identity.m_adapter_address + "|" + identity.m_device_address;
  const auto body =
      std::to_string(count) +
      (count == 1 ? " new notification arrived. Open Dmn Bluetooth to view it."
                  : " new notifications arrived. Open Dmn Bluetooth to view "
                    "them.");

  return {id, "New iPhone notification", body};
}

} // namespace dmn::gui
