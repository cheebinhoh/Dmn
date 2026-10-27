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
 * Thread-Safety
 * -------------
 * send() and nextMessage() may be called from any thread. All engine state is
 * owned by one control thread; blocking transport work runs on short-lived
 * worker threads that report back through that thread. Destruction cancels
 * pending monitor starts, stops active monitors and joins every thread.
 */

#ifndef DMN_BLUETOOTH_GUI_ENGINE_ENGINE_HPP_
#define DMN_BLUETOOTH_GUI_ENGINE_ENGINE_HPP_

#include "app/gui/engine/dmn-engine-backend.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dmn::gui {

/**
 * @brief Current JSON protocol major version.
 */
inline constexpr int kEngineProtocolVersion = 1;

struct Dmn_EngineOptions {
  /**
   * @brief How often the control loop reconciles monitors and retries.
   */
  std::chrono::milliseconds m_tick_interval{250};

  /**
   * @brief Background inventory refresh period while no operation is running.
   */
  std::chrono::milliseconds m_inventory_refresh_interval{5000};
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

#endif // DMN_BLUETOOTH_GUI_ENGINE_ENGINE_HPP_
