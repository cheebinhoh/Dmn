/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-engine.hpp
 * @brief GUI-independent Bluetooth monitor engine driven by JSON messages.
 *
 * Overview
 * --------
 * Dmn_Engine owns the device selection model, monitoring intents, per-device
 * monitor sessions and retry policy that every frontend needs, and exposes them
 * through a JSON message protocol (version 1). A frontend sends commands with
 * send() and drains snapshots, events and persistence requests with
 * nextMessage(). The engine never touches disk, never pairs, and connects only
 * on an explicit command.
 *
 * Architecture
 * ------------
 * The engine is a frontend-neutral layer shared by every Bluetooth frontend:
 * the GTK app (linked directly), the COSMIC app (through the C ABI in
 * src/app/gui/cabi) and the dmn-bluetooth-cli command. It contains no GTK,
 * GLib or other UI code and is built whenever any of those frontends is
 * enabled (ENABLE_APP_BLUETOOTH_CLI, _GTK or _COSMIC), as the static library
 * dmn-bluetooth-engine in src/app/engine.
 *
 *   GTK app          COSMIC app (Rust)        dmn-bluetooth-cli
 *      |                  |  C ABI                  |
 *      +------------------+-------------+-----------+
 *                                       | JSON commands / messages
 *                              +--------v--------+
 *                              |   Dmn_Engine    |  one control thread, worker
 *                              | (this class)    |  threads for blocking calls
 *                              +--------+--------+
 *                                       | Dmn_EngineBackend (transport seam)
 *                     +-----------------+-----------------+
 *                     |                                   |
 *            Bluez backend (real)                  Demo backend (synthetic)
 *   Dmn_BluezClient + Dmn_AncsBluezMonitor     in-memory phones and a timer
 *
 * Layers it builds on: Dmn_DeviceSelectionModel and Dmn_DeviceMonitorController
 * (dmn-bluetooth-device-core, GLib-free) for selection, action availability and
 * retry back-off; Dmn_JsonValue for the wire format.
 *
 * Backends
 * --------
 * - Bluez backend (makeBluezBackend): talks to the real BlueZ system service
 *   and ANCS. It needs D-Bus, BlueZ and a paired iPhone.
 * Dmn_BluezBackendOptions selects two behaviors. Managed mode (default, used by
 * the GUIs) only starts a monitor for a device that is already saved, paired
 * and connected, reports notifications newer than backend creation, and
 * inventory comes from snapshots. Direct mode (used by the CLI) resolves a
 * device by address or name and discovers, pairs and connects it as part of the
 * "monitor_direct" command, with configurable timeouts.
 * - Demo backend (makeDemoBackend): deterministic fake phones and
 *   notifications with no Bluetooth hardware, D-Bus or BlueZ. It exists for
 *   UI development, screenshots, CI and "--demo" runs, never touches saved
 *   devices, and supports only the managed command flow.
 * Both implement the same Dmn_EngineBackend, so frontends and the engine are
 * identical in either mode; only the transport differs.
 *
 * Protocol additions for the CLI
 * ------------------------------
 * "monitor_direct" (adapter?, device) starts a one-device monitor outside the
 * saved-selection flow and "stop_direct" ends it. The engine answers with
 * "direct_monitor" messages whose "state" is progress, ready, failed (with
 * code, detail, remote_name, cancelled) or stopped. Monitor events carry a
 * "terminal" flag. Set Dmn_EngineOptions::m_inventory_enabled to false when no
 * device list is needed.
 *
 * Thread-Safety
 * -------------
 * send() and nextMessage() may be called from any thread. All engine state is
 * owned by one control thread; blocking transport work runs on short-lived
 * worker threads that report back through that thread. Destruction cancels
 * pending monitor starts, stops active monitors and joins every thread.
 */

#ifndef DMN_BLUETOOTH_ENGINE_ENGINE_HPP_
#define DMN_BLUETOOTH_ENGINE_ENGINE_HPP_

#include "app/engine/dmn-engine-backend.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dmn::gui {

/**
 * @brief Current JSON protocol major version.
 */
inline constexpr int kEngineProtocolVersion{1};

struct Dmn_EngineOptions {
  /**
   * @brief How often the control loop reconciles monitors and retries.
   */
  std::chrono::milliseconds m_tick_interval{250};

  /**
   * @brief Background inventory refresh period while no operation is running.
   */
  std::chrono::milliseconds m_inventory_refresh_interval{5000};

  /**
   * @brief When false the engine never reads the device inventory (no
   *        snapshot at startup, on a timer or on a backend change). The CLI
   *        sets this because it only runs one direct monitor.
   */
  bool m_inventory_enabled{true};
};

class Dmn_Engine {
public:
  explicit Dmn_Engine(std::unique_ptr<Dmn_EngineBackend> backend,
                      const Dmn_EngineOptions &options = {});
  ~Dmn_Engine();

  Dmn_Engine(const Dmn_Engine &) = delete;
  auto operator=(const Dmn_Engine &) -> Dmn_Engine & = delete;
  Dmn_Engine(Dmn_Engine &&) = delete;
  auto operator=(Dmn_Engine &&) -> Dmn_Engine & = delete;

  /**
   * @brief Parses and enqueues one command.
   * @return false for malformed JSON, a non-object, a missing type, or when the
   *         engine has already shut down.
   */
  [[nodiscard]] auto send(std::string_view commandJson) -> bool;

  /**
   * @brief Waits up to `timeout` for the next outgoing message.
   * @return A JSON document, or nullopt on timeout or after the closed engine's
   *         queue has drained.
   */
  [[nodiscard]] auto
  nextMessage(std::chrono::milliseconds timeout) -> std::optional<std::string>;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn::gui

#endif // DMN_BLUETOOTH_ENGINE_ENGINE_HPP_
