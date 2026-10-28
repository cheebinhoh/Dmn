/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-device-monitor-controller.hpp
 * @brief GTK-independent monitor lifecycle decisions and retry policy.
 *
 * The frontend supplies current device and operation state; this controller
 * returns start/stop commands and retains per-device retry state without
 * owning UI widgets, settings, or Bluetooth transport objects.
 */

#ifndef DMN_BLUETOOTH_GUI_DEVICE_MONITOR_CONTROLLER_HPP_
#define DMN_BLUETOOTH_GUI_DEVICE_MONITOR_CONTROLLER_HPP_

#include "app/gui/core/dmn-device-selection-model.hpp"

#include <chrono>
#include <vector>

namespace dmn::gui {

/**
 * @brief Live state needed to reconcile one device's monitor lifecycle.
 */
struct Dmn_MonitorControllerDeviceState {
  /**
   * @brief Stable identity for the target device.
   */
  Dmn_DeviceIdentity m_identity;

  /**
   * @brief Whether the identity remains in the saved device selection.
   */
  bool m_saved{};

  /**
   * @brief Whether the user has retained explicit monitoring intent.
   */
  bool m_monitoring_requested{};

  /**
   * @brief Whether the device is present in the current BlueZ inventory.
   */
  bool m_present{};

  /**
   * @brief Whether BlueZ currently reports the device as paired.
   */
  bool m_paired{};

  /**
   * @brief Whether BlueZ currently confirms the device is connected.
   */
  bool m_connected{};

  /**
   * @brief Whether an ANCS monitor session is active for this device.
   */
  bool m_monitor_active{};

  /**
   * @brief Whether an active session stopped unexpectedly and needs cleanup.
   */
  bool m_monitor_restart_required{};

  /**
   * @brief Whether a monitor start or stop operation is in progress.
   */
  bool m_monitor_busy{};
};

/**
 * @brief Monitor command selected by lifecycle reconciliation.
 */
struct Dmn_MonitorControllerCommand {
  enum class Action {
    kStart, /**< Start monitoring a connected device with saved intent. */
    kStop,  /**< Stop a monitor that is no longer valid for its device. */
  };

  /**
   * @brief Stable identity for the target device.
   */
  Dmn_DeviceIdentity m_identity;

  /**
   * @brief Start or stop action to execute in the frontend.
   */
  Action m_action{Action::kStart};
};

/**
 * @brief Decide monitor lifecycle transitions independently of any GUI.
 *
 * Frontends provide observed state and execute the returned commands on
 * their own asynchronous facilities. Failed starts and unexpected session
 * stops retain monitoring intent and are retried using jittered exponential
 * backoff.
 */
class Dmn_DeviceMonitorController {
public:
  using Clock = std::chrono::steady_clock;

  /**
   * @brief Reconcile current devices with saved intent and monitor sessions.
   *
   * Active sessions are stopped when their device is no longer saved,
   * requested, or connected, or needs restart cleanup. Starts are returned
   * only when @p allowStarts is true and the retry delay has elapsed.
   *
   * @param devices Current state for saved devices and active monitor sessions.
   * @param allowStarts Whether new start commands may currently be issued.
   * @param now Current monotonic time, supplied by the frontend or tests.
   * @return Required monitor transitions in input order.
   */
  [[nodiscard]] auto
  reconcile(const std::vector<Dmn_MonitorControllerDeviceState> &devices,
            bool allowStarts,
            Clock::time_point now) -> std::vector<Dmn_MonitorControllerCommand>;

  /**
   * @brief Record a failed start or unexpected session stop and retry later.
   *
   * @param identity Stable identity of the device whose monitor needs retry.
   * @param now Current monotonic time.
   * @param jitterPercent Retry-delay percentage in the inclusive range 80 to
   * 100.
   * @return The scheduled delay.
   * @throws std::invalid_argument If @p jitterPercent is outside 80 to 100.
   */
  auto
  recordStartFailure(const Dmn_DeviceIdentity &identity, Clock::time_point now,
                     unsigned int jitterPercent) -> std::chrono::milliseconds;

  /**
   * @brief Clear retry state after a successful monitor start.
   */
  void recordStartSuccess(const Dmn_DeviceIdentity &identity);

  /**
   * @brief Clear retry state after an explicit intent change.
   */
  void resetRetries(const Dmn_DeviceIdentity &identity);

private:
  struct RetryState {
    Dmn_DeviceIdentity m_identity;
    unsigned int m_attempts{};
    Clock::time_point m_next_attempt{};
  };

  [[nodiscard]] auto
  retryStateFor(const Dmn_DeviceIdentity &identity) -> RetryState &;

  std::vector<RetryState> m_retry_states;
};

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_GUI_DEVICE_MONITOR_CONTROLLER_HPP_
