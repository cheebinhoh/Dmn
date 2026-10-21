/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file desktop-alert-policy.hpp
 * @brief Privacy-safe aggregation policy for optional desktop alerts.
 */

#ifndef DMN_BLUETOOTH_GUI_DESKTOP_ALERT_POLICY_HPP_
#define DMN_BLUETOOTH_GUI_DESKTOP_ALERT_POLICY_HPP_

#include "app/gui/core/device-selection-model.hpp"

#include "dmn-ancs-client.hpp"

#include <cstddef>
#include <string>

namespace dmn::gui {

/** @brief Generic notification payload without device or ANCS content. */
struct Dmn_DesktopAlert {
  /** @brief Stable replacement ID scoped to one adapter/device pair. */
  std::string m_id;

  /** @brief Generic alert summary. */
  std::string m_title;

  /** @brief Generic count and app-opening instruction. */
  std::string m_body;
};

/** @brief Return whether an ANCS event represents a newly added notification.
 */
[[nodiscard]] auto shouldSendDesktopAlert(const Dmn_AncsEvent &event) -> bool;

/**
 * @brief Build a generic per-device replacement alert.
 *
 * Notification text, device name, and Bluetooth address are not copied into
 * the user-visible title or body.
 *
 * @param identity Stable device identity used only for the replacement ID.
 * @param count Number of added notifications aggregated for this device.
 * @return Generic alert suitable for GNotification.
 */
[[nodiscard]] auto makeDesktopAlert(const Dmn_DeviceIdentity &identity,
                                    std::size_t count) -> Dmn_DesktopAlert;

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_GUI_DESKTOP_ALERT_POLICY_HPP_
