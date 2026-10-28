/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-engine-backend.hpp
 * @brief Bluetooth transport boundary used by the GUI-independent engine.
 *
 * Overview
 * --------
 * The engine never talks to BlueZ or ANCS directly. It drives this interface,
 * which has a real implementation over Dmn_BluezClient and
 * Dmn_AncsBluezMonitor, a deterministic demo implementation for UI development
 * and hardware-free runs, and fakes in unit tests.
 *
 * Implementations
 * ---------------
 * - makeBluezBackend(): production transport over BlueZ and ANCS; see
 *   Dmn_BluezBackendOptions for its managed (GUI) and direct (CLI) modes.
 * - makeDemoBackend(): synthetic, hardware-free transport for development,
 *   CI and demo runs; it simulates connect, monitor and notifications.
 * The engine (dmn-engine.hpp) documents how they fit under the frontends.
 * This header and the engine library have no GTK or GLib dependency.
 *
 * Thread-Safety
 * -------------
 * Blocking calls are made from engine worker threads, possibly concurrently for
 * different operations. Implementations must tolerate that. Handlers installed
 * with setHandlers() may be invoked from any thread and must not block.
 */

#ifndef DMN_BLUETOOTH_ENGINE_BACKEND_HPP_
#define DMN_BLUETOOTH_ENGINE_BACKEND_HPP_

#include "app/gui/core/dmn-device-selection-model.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dmn::gui {

struct Dmn_BackendAdapter {
  std::string m_path;
  std::string m_address;
  std::string m_name;
  bool m_powered{};
};

struct Dmn_BackendInventory {
  std::vector<Dmn_BackendAdapter> m_adapters;
  std::vector<Dmn_DeviceEntry> m_devices;
};

struct Dmn_BackendError {
  explicit Dmn_BackendError(std::string message, bool cancelled = false,
                            std::string code = {}, std::string detail = {},
                            std::string remoteName = {})
      : m_message{std::move(message)}, m_cancelled{cancelled},
        m_code{std::move(code)}, m_detail{std::move(detail)},
        m_remote_name{std::move(remoteName)} {}

  std::string m_message;

  /**
   * @brief True when the operation ended because the caller cancelled it.
   */
  bool m_cancelled{};

  /**
   * @brief Stable machine-readable failure class (empty when unclassified).
   *
   * One of "bluez_unavailable", "invalid_argument", "not_ready", "inventory",
   * "discovery", "device_not_found", "pairing", "connection",
   * "ancs_unsupported", "ancs_startup", "cancelled" or "cleanup". Frontends
   * such as the CLI map it to exit statuses and wording.
   */
  std::string m_code;

  /**
   * @brief Underlying message without the stage prefix of m_message.
   */
  std::string m_detail;

  /**
   * @brief Remote device name reported with the failure, when known.
   */
  std::string m_remote_name;
};

using Dmn_BackendResult = std::expected<void, Dmn_BackendError>;

struct Dmn_BackendNotification {
  /**
   * @brief One of "added", "updated", "removed" or "attributes_unavailable".
   */
  std::string m_kind;

  std::uint32_t m_uid{};
  std::uint8_t m_category{};
  std::uint8_t m_flags{};
  std::uint8_t m_category_count{};
  std::optional<std::string> m_app_identifier;
  std::optional<std::string> m_title;
  std::optional<std::string> m_subtitle;
  std::optional<std::string> m_message;
  std::optional<std::string> m_date;

  /**
   * @brief The ANCS event that carried the notification: "added", "updated",
   *        "removed" or "attributes_unavailable". For the last one, m_kind
   *        still names the notification event whose attributes were missing.
   */
  std::string m_event;
};

struct Dmn_BackendMonitorEvent {
  enum class Kind {
    kError,        /**< A transport or protocol error occurred. */
    kNotification, /**< A phone notification arrived or changed. */
    kState         /**< The ANCS session changed state. */
  };

  Kind m_kind{Kind::kError};

  /**
   * @brief Session state name for kState events.
   */
  std::string m_state;

  /**
   * @brief True when the session ended and must be restarted to continue.
   */
  bool m_terminal{};

  std::optional<Dmn_BackendNotification> m_notification;

  /**
   * @brief Error text for kError events.
   */
  std::string m_error;
};

class Dmn_EngineBackend {
public:
  using InventoryChangedHandler = std::function<void()>;
  using MonitorEventHandler = std::function<void(
      const Dmn_DeviceIdentity &, const Dmn_BackendMonitorEvent &)>;
  using ProgressHandler = std::function<void(const std::string &)>;

  Dmn_EngineBackend() = default;
  virtual ~Dmn_EngineBackend() = default;

  Dmn_EngineBackend(const Dmn_EngineBackend &) = delete;
  auto operator=(const Dmn_EngineBackend &) -> Dmn_EngineBackend & = delete;
  Dmn_EngineBackend(Dmn_EngineBackend &&) = delete;
  auto operator=(Dmn_EngineBackend &&) -> Dmn_EngineBackend & = delete;

  [[nodiscard]] virtual auto name() const -> std::string = 0;

  /**
   * @brief Installs the engine callbacks before any other call.
   */
  virtual void setHandlers(InventoryChangedHandler inventoryChanged,
                           MonitorEventHandler monitorEvent) = 0;

  [[nodiscard]] virtual auto
  snapshot() -> std::expected<Dmn_BackendInventory, Dmn_BackendError> = 0;

  virtual auto setScanning(bool enabled, const std::string &adapterPath)
      -> Dmn_BackendResult = 0;

  virtual auto
  connect(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult = 0;

  virtual auto
  disconnect(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult = 0;

  /**
   * @brief Aborts an in-flight connect(); safe to call from another thread.
   */
  virtual void cancelConnect(const Dmn_DeviceIdentity &identity) = 0;

  /**
   * @brief Starts ANCS monitoring, blocking until ready, failed or cancelled.
   *
   * Implementations poll `cancel` and return an error with m_cancelled set when
   * it becomes true.
   */
  virtual auto
  startMonitor(const Dmn_DeviceIdentity &identity,
               const std::atomic<bool> &cancel,
               const ProgressHandler &progress) -> Dmn_BackendResult = 0;

  virtual auto
  stopMonitor(const Dmn_DeviceIdentity &identity) -> Dmn_BackendResult = 0;
};

/**
 * @brief Tunables of the BlueZ backend.
 *
 * The defaults give the "managed" behavior used by the GUIs: a monitor starts
 * only for a device that is already connected, and only notifications newer
 * than the backend's creation are reported. `m_direct_monitor` selects the
 * behavior of the CLI instead.
 */
struct Dmn_BluezBackendOptions {
  /**
   * @brief When true, startMonitor() resolves the device by address or exact
   *        name and discovers, pairs and connects it as needed. In this mode
   *        the progress handler receives stable keys ("inventory",
   *        "discovery", "pairing", "connection", "ancs_startup" and
   *        "remaining_profiles:<device name>") instead of display text.
   */
  bool m_direct_monitor{};

  /**
   * @brief Direct mode only: when set, notifications dated before it are
   *        dropped. Managed mode always uses the backend creation time.
   */
  std::optional<std::chrono::system_clock::time_point> m_notification_cutoff;

  /**
   * @brief Direct mode only: do not cap queued ANCS notifications.
   */
  bool m_unlimited_notifications{};

  std::chrono::seconds m_discovery_timeout{30};
  std::chrono::seconds m_pairing_timeout{120};
  std::chrono::seconds m_connection_timeout{30};
  std::chrono::seconds m_services_timeout{30};

  /**
   * @brief Private D-Bus address for isolated integration tests.
   */
  std::string m_test_bus_address;
};

/**
 * @brief Backend over BlueZ and ANCS; requires a system D-Bus with BlueZ.
 */
[[nodiscard]] auto makeBluezBackend(const Dmn_BluezBackendOptions &options = {})
    -> std::unique_ptr<Dmn_EngineBackend>;

/**
 * @brief Deterministic synthetic backend that needs no Bluetooth hardware.
 *
 * @param notificationInterval Delay between synthetic notifications while a
 *        demo monitor is running; an initial notification is sent immediately.
 */
[[nodiscard]] auto makeDemoBackend(
    std::chrono::milliseconds notificationInterval = std::chrono::seconds{
        4}) -> std::unique_ptr<Dmn_EngineBackend>;

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_ENGINE_BACKEND_HPP_
