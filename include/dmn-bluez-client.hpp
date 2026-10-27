/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-client.hpp
 * @brief Optional BlueZ adapter, device, and discovery API.
 *
 * Architecture
 * ------------
 * @ref Dmn_BluezClient is the typed BlueZ domain layer for adapter/device
 * inventory and explicit discovery, pairing, and connection operations. It
 * translates BlueZ's ObjectManager objects, properties, signals, and method
 * results into owned Dmn_Bluez values and typed errors:
 *
 * @verbatim
 * Application / Dmn_AncsBluezMonitor
 *          |
 *          v
 * Dmn_BluezClient                 BlueZ inventory and device operations
 *          |
 *          v
 * Dmn_DbusClient + Dmn_DbusValue  generic method/property/signal API
 *          |
 *          v
 * libdbus -> system bus -> org.bluez
 * @endverbatim
 *
 * This class depends on the generic D-Bus client, not on libdbus directly.
 * It does not expose arbitrary D-Bus calls and does not implement GATT
 * characteristic operations; those belong to @ref Dmn_BluezGattClient. In
 * the ANCS monitor architecture, this client handles device inventory and
 * link setup while the GATT backend handles the ANCS service tree and
 * characteristics. See `docs/specs/dbus-client-spec.md` for the detailed
 * generic-client and BlueZ boundary.
 */

#ifndef DMN_BLUEZ_CLIENT_HPP_
#define DMN_BLUEZ_CLIENT_HPP_

#include "dmn-dbus-client.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dmn {

/**
 * @brief Owned adapter state from the current BlueZ inventory.
 */
struct Dmn_BluezAdapter {
  /**
   * @brief Current BlueZ object path; it is not a stable adapter identity.
   */
  std::string object_path;

  /**
   * @brief Adapter Bluetooth address, empty when unavailable.
   */
  std::string address;

  /**
   * @brief Display name selected from alias, reported name, then address.
   */
  std::string name;

  /**
   * @brief BlueZ `Alias`, when available.
   */
  std::optional<std::string> alias;

  /**
   * @brief BlueZ `Name`, when available.
   */
  std::optional<std::string> reported_name;

  /**
   * @brief BlueZ `Powered` state; absent means unknown or unavailable.
   */
  std::optional<bool> powered;
};

/**
 * @brief Owned device state from the current BlueZ inventory.
 */
struct Dmn_BluezDevice {
  /**
   * @brief Current BlueZ object path; it may change after rediscovery.
   */
  std::string object_path;

  /**
   * @brief Object path of the parent adapter.
   */
  std::string adapter_path;

  /**
   * @brief Bluetooth device address used with adapter identity for updates.
   */
  std::string address;

  /**
   * @brief Display name selected from alias, reported name, then address.
   */
  std::string name;

  /**
   * @brief BlueZ `Alias`, when available.
   */
  std::optional<std::string> alias;

  /**
   * @brief BlueZ `Name`, when available.
   */
  std::optional<std::string> reported_name;

  /**
   * @brief BlueZ signal strength in dBm, when available.
   */
  std::optional<std::int16_t> rssi;

  /**
   * @brief BlueZ `Paired` state; absent means unknown or unavailable.
   */
  std::optional<bool> paired;

  /**
   * @brief BlueZ `Connected` state; absent means unknown or unavailable.
   */
  std::optional<bool> connected;

  /**
   * @brief BlueZ `ServicesResolved` state, when available.
   */
  std::optional<bool> services_resolved;

  /**
   * @brief Whether this device was observed during this client's scan.
   */
  bool seen_during_discovery{};
};

/**
 * @brief Reconciled adapter and known-device snapshot.
 */
struct Dmn_BluezInventory {
  /**
   * @brief Adapters in the reconciled ObjectManager snapshot.
   */
  std::vector<Dmn_BluezAdapter> adapters;

  /**
   * @brief Known devices, including devices not currently connected.
   */
  std::vector<Dmn_BluezDevice> devices;
};

/**
 * @brief Inventory or service-state transition reported by BlueZ.
 */
enum class Dmn_BluezEventKind {
  /**
   * @brief An adapter appeared in ObjectManager inventory.
   */
  AdapterAdded,

  /**
   * @brief An adapter property changed.
   */
  AdapterChanged,

  /**
   * @brief An adapter disappeared from ObjectManager inventory.
   */
  AdapterRemoved,

  /**
   * @brief A known device appeared in inventory.
   */
  DeviceAdded,

  /**
   * @brief A known device property or path changed.
   */
  DeviceChanged,

  /**
   * @brief A known device disappeared from inventory.
   */
  DeviceRemoved,

  /**
   * @brief An inventory, signal-decoding, or synchronization error occurred.
   */
  Error,

  /**
   * @brief BlueZ is available and its inventory is ready, including after
   *        resynchronization.
   */
  ServiceAvailable,

  /**
   * @brief The BlueZ service owner disappeared.
   */
  ServiceUnavailable
};

/**
 * @brief Owned inventory or service-state update delivered to a handler.
 */
struct Dmn_BluezEvent {
  /**
   * @brief Kind of inventory or service-state transition.
   */
  Dmn_BluezEventKind kind{Dmn_BluezEventKind::Error};

  /**
   * @brief Adapter associated with an adapter event, when applicable.
   */
  std::optional<Dmn_BluezAdapter> adapter;

  /**
   * @brief Device associated with a device event, when applicable.
   */
  std::optional<Dmn_BluezDevice> device;

  /**
   * @brief Human-readable detail for service-state and error events.
   */
  std::string message;
};

/**
 * @brief Connection and resource-limit configuration for a BlueZ client.
 */
struct Dmn_BluezClientConfig {
  /**
   * @brief Generic D-Bus limits used by the underlying client.
   */
  Dmn_DbusClientLimits limits;

  /**
   * @brief Explicit private-bus address for isolated integration tests.
   */
  std::string test_bus_address;
};

/**
 * @brief BlueZ operation result carrying success or a typed D-Bus error.
 */
using Dmn_BluezResult = std::expected<void, Dmn_DbusError>;

/**
 * @brief BlueZ operation result carrying a value or typed D-Bus error.
 */
template <typename T>
using Dmn_BluezValueResult = std::expected<T, Dmn_DbusError>;

/**
 * @brief Manage BlueZ inventory, explicit device actions, and one scan session.
 *
 * Production instances connect only to the system bus. Event callbacks are
 * serialized on a client-owned worker, outside the libdbus dispatch thread.
 * Callbacks must not throw or destroy this client.
 */
class Dmn_BluezClient {
public:
  /**
   * @brief Callback receiving serialized, owned BlueZ inventory events.
   */
  using EventHandler = std::function<void(const Dmn_BluezEvent &)>;

  /**
   * @brief Connect to BlueZ and subscribe to inventory signals.
   *
   * Production clients use the system bus. `test_bus_address` is only for
   * isolated fake-BlueZ tests and never falls back to another bus.
   *
   * @param config Connection limits and optional private test-bus address.
   * @throws std::runtime_error If the selected D-Bus connection or required
   * signal subscriptions cannot be established.
   */
  explicit Dmn_BluezClient(const Dmn_BluezClientConfig &config = {});

  /**
   * @brief Stop an owned discovery session and release worker resources.
   */
  virtual ~Dmn_BluezClient();

  /**
   * @brief Clients cannot be copied because they own a bus connection.
   */
  Dmn_BluezClient(const Dmn_BluezClient &) = delete;

  /**
   * @brief Clients cannot be copy-assigned.
   */
  auto operator=(const Dmn_BluezClient &) -> Dmn_BluezClient & = delete;

  /**
   * @brief Clients cannot be moved because workers retain client state.
   */
  Dmn_BluezClient(Dmn_BluezClient &&) = delete;

  /**
   * @brief Clients cannot be move-assigned.
   */
  auto operator=(Dmn_BluezClient &&) -> Dmn_BluezClient & = delete;

  /**
   * @brief Install the serialized inventory and service-event handler.
   *
   * @param handler Callback receiving owned event values on the client worker.
   * An empty handler disables callback delivery. Callbacks must not throw,
   * block on this client's D-Bus calls, or destroy the client.
   */
  void setEventHandler(EventHandler handler);

  /**
   * @brief Fetch and reconcile a fresh ObjectManager inventory snapshot.
   *
   * Signals received during the request are folded into the returned
   * inventory. A BlueZ owner change, malformed reply, transport error, or
   * bounded-queue overflow is returned as a typed D-Bus error.
   *
   * @param timeout Finite request timeout; zero selects the configured
   * default.
   * @return Owned adapter and known-device values in deterministic order.
   */
  [[nodiscard]] auto snapshot(
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_BluezValueResult<Dmn_BluezInventory>;

  /**
   * @brief Return the current owned device view after snapshot or live updates.
   *
   * @return A copy of the current known-device values.
   */
  [[nodiscard]] auto devices() const -> std::vector<Dmn_BluezDevice>;

  /**
   * @brief Request connection to one explicitly selected known device.
   *
   * The selector may be a case-insensitive device address or an exact current
   * device object path. When multiple adapters contain the address, provide
   * their address or object path in @p adapterSelector. This operation does not
   * invoke `Device1.Pair` or install a pairing agent; BlueZ may still initiate
   * its normal system pairing/authorization flow when the device requires it.
   * An already-connected device is an idempotent success with no D-Bus call.
   *
   * @param deviceSelector Bluetooth address or current object path in
   * inventory.
   * @param adapterSelector Optional adapter address or current object path.
   * @param timeout Finite method timeout; zero selects the configured default.
   * @return Success or the typed local/remote BlueZ error.
   */
  [[nodiscard]] auto connectDevice(
      const std::string &deviceSelector,
      const std::optional<std::string> &adapterSelector = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_BluezResult;

  /**
   * @brief Ask BlueZ to connect any remaining auto-connectable profiles.
   *
   * Unlike connectDevice(), this always calls `Device1.Connect`, even when
   * `Connected` is already true. BlueZ may use this to connect a remaining
   * profile or the disconnected bearer on a dual-mode device. It does not
   * select a specific bearer or guarantee that GATT services will resolve.
   * BlueZ's `AlreadyConnected` and `InProgress` responses, and its
   * `Failed` / "Operation already in progress" response, are treated as an
   * already-issued request; callers must separately verify the state they
   * need.
   *
   * @param deviceSelector Bluetooth address or current object path in
   * inventory.
   * @param adapterSelector Optional adapter address or current object path.
   * @param timeout Finite method timeout; zero selects the configured default.
   * @return Success or the typed local/remote BlueZ error.
   */
  [[nodiscard]] auto connectAvailableProfiles(
      const std::string &deviceSelector,
      const std::optional<std::string> &adapterSelector = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_BluezResult;

  /**
   * @brief Explicitly pair one selected device using BlueZ's default agent.
   *
   * This operation calls `Device1.Pair` unless the current inventory already
   * reports the device paired. Dmn does not register a pairing agent,
   * request trust, or start discovery; BlueZ's configured default agent handles
   * any required user interaction. Pairing may connect the device and resolve
   * its services as part of BlueZ's documented operation.
   *
   * @param deviceSelector Bluetooth address or current object path in
   * inventory.
   * @param adapterSelector Optional adapter address or current object path.
   * @param timeout Finite method timeout; zero selects the configured default.
   * @return Success or the typed local/remote BlueZ error. A missing default
   * agent or user cancellation is returned as a remote error.
   */
  [[nodiscard]] auto
  pairDevice(const std::string &deviceSelector,
             const std::optional<std::string> &adapterSelector = std::nullopt,
             std::chrono::milliseconds timeout =
                 std::chrono::milliseconds::zero()) -> Dmn_BluezResult;

  /**
   * @brief Request disconnection of one explicitly selected known device.
   *
   * Disconnecting does not remove pairing and may interrupt all active BlueZ
   * profiles for that device. An already-disconnected device is an idempotent
   * success with no D-Bus call.
   *
   * @param deviceSelector Bluetooth address or current object path in
   * inventory.
   * @param adapterSelector Optional adapter address or current object path.
   * @param timeout Finite method timeout; zero selects the configured default.
   * @return Success or the typed local/remote BlueZ error.
   */
  [[nodiscard]] auto disconnectDevice(
      const std::string &deviceSelector,
      const std::optional<std::string> &adapterSelector = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_BluezResult;

  /**
   * @brief Send a disconnect request even if Connected is still false.
   *
   * Intended to interrupt a pending Device1.Connect request from a separate
   * client. Unlike disconnectDevice(), this does not treat a currently
   * disconnected device as an idempotent no-op.
   *
   * @param deviceSelector Bluetooth address or current object path in
   * inventory.
   * @param adapterSelector Optional adapter address or current object path.
   * @param timeout Finite method timeout; zero selects the configured default.
   * @return Success or the typed local/remote BlueZ error.
   */
  [[nodiscard]] auto cancelConnectionAttempt(
      const std::string &deviceSelector,
      const std::optional<std::string> &adapterSelector = std::nullopt,
      std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_BluezResult;

  /**
   * @brief Set the default auto-transport filter and start one adapter scan.
   *
   * This operation does not power the adapter, pair, or connect to devices.
   * The client records ownership only after `StartDiscovery` succeeds.
   *
   * @param adapterPath Object path of a powered adapter in the current
   * inventory.
   * @return Success or the typed local/remote D-Bus error.
   */
  [[nodiscard]] auto
  startDiscovery(const std::string &adapterPath) -> Dmn_BluezResult;

  /**
   * @brief Stop only a discovery session successfully started by this client.
   *
   * Repeated calls are successful no-ops after the owned session has stopped.
   * A stop failure is returned and ownership is retained so the caller may
   * retry.
   *
   * @return Success or the typed local/remote D-Bus error.
   */
  [[nodiscard]] auto stopDiscovery() -> Dmn_BluezResult;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_BLUEZ_CLIENT_HPP_
