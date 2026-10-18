/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-ancs-bluez-monitor.hpp
 * @brief Reusable BlueZ device setup and ANCS monitor orchestration.
 *
 * Overview
 * --------
 * This facade owns the BlueZ inventory client, BlueZ GATT backend, and ANCS
 * session client. It moves device selection, optional discovery, pairing,
 * connection, service readiness, and ANCS startup out of terminal frontends.
 * Applications receive owned ANCS events through the configured callback and
 * can present them in a terminal, GTK interface, or another frontend.
 *
 * The synchronous start operation performs bounded BlueZ and GATT calls.
 * GUI applications must invoke it away from their UI thread and marshal event
 * callbacks onto their UI context. Setup progress is reported synchronously
 * on the thread calling start. Calls must not race object destruction;
 * stop serializes with startup and does not cancel an in-progress setup. The
 * facade does not disconnect or unpair the selected device during stop.
 */

#ifndef DMN_ANCS_BLUEZ_MONITOR_HPP_
#define DMN_ANCS_BLUEZ_MONITOR_HPP_

#include "dmn-ancs-client.hpp"
#include "dmn-bluez-client.hpp"

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace dmn {

/** @brief Stage at which reusable monitor startup or cleanup failed. */
enum class Dmn_AncsMonitorStage {
  InvalidArgument, /**< Invalid selector, timeout, or monitor configuration. */
  NotReady,  /**< The monitor is already active or otherwise unavailable. */
  Inventory, /**< BlueZ adapter or device inventory failure. */
  Discovery, /**< Discovery start, stop, or adapter selection failure. */
  DeviceNotFound, /**< No unique matching device was found. */
  Pairing,        /**< Pairing failed or paired state was not confirmed. */
  Connection,  /**< Connection failed or connected state was not confirmed. */
  AncsStartup, /**< ANCS service discovery or subscription failed. */
  Cleanup      /**< ANCS startup rollback or stop cleanup failed. */
};

/** @brief Typed monitor orchestration failure with optional BlueZ detail. */
struct Dmn_AncsMonitorError {
  /** @brief Startup or cleanup stage associated with the failure. */
  Dmn_AncsMonitorStage stage{Dmn_AncsMonitorStage::Inventory};

  /** @brief Human-readable detail without notification payload data. */
  std::string message{};

  /** @brief Remote BlueZ error name when the backend supplied one. */
  std::string remote_name{};

  /** @brief ANCS error category for an ANCS startup failure. */
  std::optional<Dmn_AncsErrorKind> ancs_error_kind{};
};

/** @brief Successful ANCS monitor startup result. */
struct Dmn_AncsMonitorSession {
  /** @brief Current BlueZ adapter selected for this session. */
  Dmn_BluezAdapter adapter;

  /** @brief Current BlueZ device selected for this session. */
  Dmn_BluezDevice device;

  /** @brief Whether startup requested connection of remaining profiles. */
  bool requested_remaining_profiles{};
};

/** @brief Successful startup result or typed orchestration failure. */
using Dmn_AncsMonitorResult =
    std::expected<Dmn_AncsMonitorSession, Dmn_AncsMonitorError>;

/** @brief Setup phase reported by a monitor's optional progress callback. */
enum class Dmn_AncsMonitorProgress {
  Inventory,  /**< Resolving the target in the current BlueZ inventory. */
  Discovery,  /**< Scanning for a target absent from the inventory. */
  Pairing,    /**< Pairing an unpaired target. */
  Connection, /**< Connecting the target or its remaining profiles. */
  AncsStartup /**< Resolving and subscribing to ANCS characteristics. */
};

/** @brief Callback for synchronous selected-device setup progress. */
using Dmn_AncsMonitorProgressHandler =
    std::function<void(Dmn_AncsMonitorProgress)>;

/** @brief Configuration for selecting, connecting, and monitoring one device.
 */
struct Dmn_AncsMonitorConfig {
  /** @brief Optional adapter address or current BlueZ object path. */
  std::optional<std::string> adapter_selector{};

  /**
   * @brief Require the device to already be present, paired, and connected.
   *
   * When true, startup does not discover, pair, or establish a device
   * connection. Remaining profiles are requested only when
   * `request_remaining_profiles_when_connected` is enabled.
   */
  bool require_existing_connection{};

  /**
   * @brief Request remaining BlueZ profiles for an already-connected target.
   *
   * This is intended for frontends that require a pre-existing link but need
   * the CLI-equivalent profile completion before ANCS startup. It has no
   * effect unless the selected device is already connected.
   */
  bool request_remaining_profiles_when_connected{};

  /** @brief Maximum time to discover a target not already in inventory. */
  std::chrono::milliseconds discovery_timeout{30000};

  /** @brief Maximum time for pairing the selected device. */
  std::chrono::milliseconds pairing_timeout{120000};

  /** @brief Maximum time for establishing the selected device connection. */
  std::chrono::milliseconds connection_timeout{30000};

  /**
   * @brief Optional callback invoked on the thread calling start().
   *
   * It must return promptly, must not throw, and must not call start(), stop(),
   * or destroy this monitor. The application may marshal these progress
   * values to its own UI context.
   */
  Dmn_AncsMonitorProgressHandler progress_handler{};

  /** @brief ANCS operation deadlines, pending-request policy, and date cutoff.
   */
  Dmn_AncsConfig ancs{};
};

/**
 * @brief Reusable selected-device setup and ANCS event client.
 *
 * Startup resolves an exact device name or Bluetooth address in the current
 * BlueZ inventory, discovers it on one selected powered adapter when absent,
 * pairs it when needed, connects it when needed, and starts the ANCS session.
 * Device names are matched exactly with ASCII case-insensitivity. Startup
 * addresses are matched case-insensitively. Startup progress can be observed
 * using the configuration callback.
 * The callback is installed before startup and receives serialized ANCS events
 * according to Dmn_AncsClient's callback contract.
 */
class Dmn_AncsBluezMonitor {
public:
  /**
   * @brief Create a monitor with the BlueZ/GATT connection configuration.
   *
   * @param bluezConfig System-bus configuration, or an explicit private-bus
   * address for tests.
   * @throws std::runtime_error If BlueZ/D-Bus connections cannot be created.
   */
  explicit Dmn_AncsBluezMonitor(const Dmn_BluezClientConfig &bluezConfig = {});

  /** @brief Stop ANCS subscriptions and release owned backend resources. */
  ~Dmn_AncsBluezMonitor() noexcept;

  /** @brief Monitors cannot be copied because they own workers and bus state.
   */
  Dmn_AncsBluezMonitor(const Dmn_AncsBluezMonitor &) = delete;

  /** @brief Monitors cannot be copy-assigned. */
  auto
  operator=(const Dmn_AncsBluezMonitor &) -> Dmn_AncsBluezMonitor & = delete;

  /** @brief Monitors cannot be moved while callbacks retain their state. */
  Dmn_AncsBluezMonitor(Dmn_AncsBluezMonitor &&) = delete;

  /** @brief Monitors cannot be move-assigned. */
  auto operator=(Dmn_AncsBluezMonitor &&) -> Dmn_AncsBluezMonitor & = delete;

  /**
   * @brief Install or clear the handler before starting the monitor.
   *
   * @param handler Callback receiving owned ANCS events on the ANCS worker.
   * It must return promptly, must not throw, and must not call start(),
   * stop(), or destroy this monitor.
   * @return Success or NotReady when changing the handler during a session.
   */
  [[nodiscard]] auto
  setEventHandler(Dmn_AncsClient::EventHandler handler) -> Dmn_AncsResult;

  /**
   * @brief Select and start monitoring one exact device.
   *
   * This operation is synchronous and bounded by @p config. By default it
   * discovers, pairs, and connects as needed; if already connected, BlueZ is
   * asked to connect remaining auto-connectable profiles. When
   * `config.require_existing_connection` is true, it instead requires an
   * already-present, paired, connected device and performs no discovery,
   * pairing, or initial link connection. A connected target gets a BlueZ
   * Connect request to complete remaining profiles in the normal
   * setup-capable mode, or when
   * `config.request_remaining_profiles_when_connected` is enabled. Pairing
   * and link state are retained after the monitor stops. stop() serializes
   * with this operation and does not cancel an in-progress setup.
   *
   * @param device_selector Exact display name or Bluetooth address.
   * @param config Adapter, setup deadlines, and ANCS behavior.
   * @return The selected adapter/device after ANCS startup, or a typed error.
   */
  [[nodiscard]] auto
  start(const std::string &device_selector,
        const Dmn_AncsMonitorConfig &config = {}) -> Dmn_AncsMonitorResult;

  /**
   * @brief Stop ANCS and retry any required subscription cleanup.
   *
   * If startup is in progress, this call waits until startup completes before
   * stopping ANCS; it does not cancel device discovery, pairing, or connection.
   *
   * @return Success or a typed cleanup error.
   */
  [[nodiscard]] auto stop() -> Dmn_AncsResult;

  /**
   * @brief Return the current ANCS session state.
   *
   * This reports ANCS lifecycle only; it remains Stopped during device setup.
   */
  [[nodiscard]] auto state() const -> Dmn_AncsState;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_ANCS_BLUEZ_MONITOR_HPP_
