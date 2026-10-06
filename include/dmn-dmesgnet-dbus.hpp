/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet-dbus.hpp
 * @brief DMesg application facade over the optional local D-Bus transport.
 */

#ifndef DMN_DMESGNET_DBUS_HPP_
#define DMN_DMESGNET_DBUS_HPP_

#include "dmn-dbus-config.hpp"
#include "dmn-dmesg.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace dmn {

/**
 * @brief Snapshot of both D-Bus endpoints owned by a DMesg facade.
 *
 * Endpoint counters report local activity and do not imply peer delivery.
 */
struct Dmn_DMesgDbusStatus {
  Dmn_DbusIoStatus input{};
  Dmn_DbusIoStatus output{};
};

/**
 * @brief DMesg handler facade that owns its D-Bus transport and network node.
 *
 * The class composes an internal Dmn_DMesgNet with separate D-Bus input and
 * output endpoints. It forwards the selected handler/topic operations while
 * keeping those transport objects private. The default config selects the
 * Dmn_DMesgNet signal tuple on the session bus.
 */
class Dmn_DMesgDbus final {
public:
  using AsyncProcessTask = Dmn_DMesg::AsyncProcessTask;
  using FilterTask = Dmn_DMesg::FilterTask;
  using HandlerEvent = Dmn_DMesg::HandlerEvent;
  using HandlerEventCallbackTask = Dmn_DMesg::HandlerEventCallbackTask;
  using HandlerEventType = Dmn_DMesg::HandlerEventType;
  using HandlerConfig = Dmn_DMesg::HandlerConfig;
  using HandlerFactory = Dmn_DMesg::HandlerFactory;
  using HandlerSpec = Dmn_DMesg::HandlerSpec;
  using HandlerType = Dmn_DMesg::HandlerType;

  /**
   * @brief Construct a DMesg node with private D-Bus input/output endpoints.
   *
   * @param node_id DMesg node identifier; must be unique on the bus.
   * @param config Shared D-Bus routing and queue configuration.
   * @throws std::invalid_argument if the endpoint configuration is invalid.
   * @throws std::system_error if either endpoint cannot connect or initialize.
   */
  explicit Dmn_DMesgDbus(std::string_view node_id,
                         const Dmn_DbusConfig &config = {});
  ~Dmn_DMesgDbus() noexcept;

  Dmn_DMesgDbus(const Dmn_DMesgDbus &) = delete;
  auto operator=(const Dmn_DMesgDbus &) -> Dmn_DMesgDbus & = delete;
  Dmn_DMesgDbus(Dmn_DMesgDbus &&) = delete;
  auto operator=(Dmn_DMesgDbus &&) -> Dmn_DMesgDbus & = delete;

  /**
   * @brief Open a standard handler using normalized constructor arguments.
   *
   * @param spec Handler name/topic, callbacks, and handler configuration.
   * @return Proxy to the registered handler.
   */
  auto openHandler(const HandlerSpec &spec) -> HandlerType;

  /**
   * @brief Open a handler using a custom derived-handler factory.
   *
   * @param spec Normalized handler construction arguments.
   * @param factory Factory that creates a handler from @p spec.
   * @return Proxy to the registered handler.
   */
  auto openHandlerWithFactory(const HandlerSpec &spec,
                              const HandlerFactory &factory) -> HandlerType;

  /** @brief Close the handler and invalidate its proxy. */
  void closeHandler(HandlerType &handler);

  /**
   * @brief Return the last published message for a topic, if present.
   */
  auto getTopicLastMessage(std::string_view topic) -> std::optional<DMesgPb>;

  /** @brief Republish the last known topic message to resolve conflicts. */
  void resetConflictStateWithLastTopicMessage(std::string_view topic);

  /** @brief Return thread-safe input and output endpoint status snapshots. */
  [[nodiscard]] auto status() const -> Dmn_DMesgDbusStatus;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_DMESGNET_DBUS_HPP_
