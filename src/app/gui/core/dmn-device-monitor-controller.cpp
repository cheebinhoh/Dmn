/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-device-monitor-controller.cpp
 * @brief GTK-independent monitor lifecycle decisions and retry policy.
 */

#include "app/gui/core/dmn-device-monitor-controller.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace dmn::gui {
namespace {

constexpr auto kInitialRetryDelay = std::chrono::seconds{5};
constexpr auto kMaximumRetryDelay = std::chrono::seconds{60};
constexpr unsigned int kRetryJitterMinimumPercent = 80;
constexpr unsigned int kRetryJitterMaximumPercent = 100;
constexpr unsigned int kMaximumBackoffAttempt = 5;

} // namespace

auto Dmn_DeviceMonitorController::reconcile(
    const std::vector<Dmn_MonitorControllerDeviceState> &devices,
    const bool allowStarts,
    const Clock::time_point now) -> std::vector<Dmn_MonitorControllerCommand> {
  std::vector<Dmn_MonitorControllerCommand> commands;
  for (const auto &device : devices) {
    if (!device.m_saved || !device.m_monitoring_requested ||
        !device.m_present || !device.m_paired || !device.m_connected) {
      resetRetries(device.m_identity);
    }

    if (device.m_monitor_busy) {
      continue;
    }

    const bool connected =
        device.m_present && device.m_paired && device.m_connected;
    if (device.m_monitor_active &&
        (!device.m_saved || !device.m_monitoring_requested || !connected ||
         device.m_monitor_restart_required)) {
      commands.push_back(
          {device.m_identity, Dmn_MonitorControllerCommand::Action::kStop});

      continue;
    }

    if (!allowStarts || !device.m_saved || !device.m_monitoring_requested ||
        !connected || device.m_monitor_active) {
      continue;
    }

    auto &retryState = retryStateFor(device.m_identity);
    if (now < retryState.m_next_attempt) {
      continue;
    }

    commands.push_back(
        {device.m_identity, Dmn_MonitorControllerCommand::Action::kStart});
  }

  return commands;
}

auto Dmn_DeviceMonitorController::recordStartFailure(
    const Dmn_DeviceIdentity &identity, const Clock::time_point now,
    const unsigned int jitterPercent) -> std::chrono::milliseconds {
  if (jitterPercent < kRetryJitterMinimumPercent ||
      jitterPercent > kRetryJitterMaximumPercent) {
    throw std::invalid_argument{
        "monitor retry jitter must be between 80 and 100"};
  }

  auto &retryState = retryStateFor(identity);
  if (retryState.m_attempts < kMaximumBackoffAttempt) {
    ++retryState.m_attempts;
  }

  auto delay = kInitialRetryDelay;
  for (unsigned int attempt = 1;
       attempt < retryState.m_attempts && delay < kMaximumRetryDelay;
       ++attempt) {
    delay = std::min(delay * 2, kMaximumRetryDelay);
  }

  const auto delayMilliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(delay).count();
  const auto jitteredDelay = std::chrono::milliseconds{
      delayMilliseconds * static_cast<std::int64_t>(jitterPercent) / 100};
  retryState.m_next_attempt = now + jitteredDelay;

  return jitteredDelay;
}

void Dmn_DeviceMonitorController::recordStartSuccess(
    const Dmn_DeviceIdentity &identity) {
  resetRetries(identity);
}

void Dmn_DeviceMonitorController::resetRetries(
    const Dmn_DeviceIdentity &identity) {
  std::erase_if(m_retry_states, [&identity](const auto &retryState) {
    return retryState.m_identity == identity;
  });
}

auto Dmn_DeviceMonitorController::retryStateFor(
    const Dmn_DeviceIdentity &identity) -> RetryState & {
  const auto found = std::find_if(m_retry_states.begin(), m_retry_states.end(),
                                  [&identity](const auto &retryState) {
                                    return retryState.m_identity == identity;
                                  });
  if (found != m_retry_states.end()) {
    return *found;
  }

  m_retry_states.push_back({identity, 0, {}});

  return m_retry_states.back();
}

} // namespace dmn::gui
