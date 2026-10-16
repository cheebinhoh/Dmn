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
 * keeping those transport objects private. Each instance is a complete,
 * independent Dmn_DMesgNet participant: its output broadcasts DMesg messages
 * and its input receives matching broadcasts from other participants through
 * the selected bus daemon. Applications link this library and create their
 * own instances; the facade is an application participant, not a D-Bus
 * service, a client proxy for a shared Dmn service, or a bus daemon, and it
 * does not start or manage a daemon. The default configuration uses the
 * session bus. A caller may instead set an explicit address for the system
 * bus, an explicitly addressed session bus, or a separately started private
 * bus instance. All participants must use the same bus and signal tuple; they
 * communicate through the bus daemon rather than through direct peer-to-peer
 * D-Bus connections. The output endpoint serializes the node's outgoing
 * payload into a byte-array signal; the daemon routes matching signals to
 * input endpoints, which queue the received bytes for the node. This transport
 * does not create a shared Dmn publisher or acknowledge delivery to a peer.
 */
class Dmn_DMesgDbus final {
public:
  /** Callback aliases forwarded from the wrapped DMesg node. */
  using AsyncProcessTask = Dmn_DMesg::AsyncProcessTask;
  using FilterTask = Dmn_DMesg::FilterTask;

  /** Ordered handler-event contract aliases forwarded from Dmn_DMesg. */
  using HandlerEvent = Dmn_DMesg::HandlerEvent;
  using HandlerEventCallbackTask = Dmn_DMesg::HandlerEventCallbackTask;
  using HandlerEventType = Dmn_DMesg::HandlerEventType;

  /** Handler configuration, factory, specification, and proxy aliases. */
  using HandlerConfig = Dmn_DMesg::HandlerConfig;
  using HandlerFactory = Dmn_DMesg::HandlerFactory;
  using HandlerSpec = Dmn_DMesg::HandlerSpec;
  using HandlerType = Dmn_DMesg::HandlerType;

  /**
   * @brief Construct an independent DMesg participant on the configured bus.
   *
   * @param node_id DMesg node identifier; must be unique among participants
   *                that share the same DMesg network.
   * @param config Bus selection, D-Bus signal routing, and queue limits.
   *               Empty bus_address selects the session bus; a non-empty
   *               address selects that exact bus daemon (the system bus, an
   *               explicitly addressed session bus, or a separately started
   *               private bus instance). Other
   *               participants must use the same bus and signal tuple to
   *               exchange messages.
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
   * The full spec, including its optional handler-event observer, is forwarded
   * to the wrapped DMesg node.
   *
   * @param spec Handler name/topic, callbacks, and handler configuration.
   * @return Proxy to the registered handler.
   */
  auto openHandler(const HandlerSpec &spec) -> HandlerType;

  /**
   * @brief Open a handler using a custom derived-handler factory.
   *
   * The spec and factory are forwarded unchanged to the wrapped DMesg node.
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
