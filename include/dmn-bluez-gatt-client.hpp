/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-bluez-gatt-client.hpp
 * @brief BlueZ D-Bus implementation of the remote GATT client contract.
 */

#ifndef DMN_BLUEZ_GATT_CLIENT_HPP_
#define DMN_BLUEZ_GATT_CLIENT_HPP_

#include "dmn-bluez-client.hpp"
#include "dmn-gatt-client.hpp"

#include <memory>

namespace dmn {

/**
 * @brief Access one selected device's remote GATT tree through BlueZ.
 *
 * This optional Linux backend uses the generic Dmn_DbusClient and BlueZ's
 * system-D-Bus GATT API. It neither connects nor pairs devices implicitly.
 * Callers must explicitly connect using Dmn_BluezClient, wait for service
 * resolution, and then inspect or operate on the current GATT tree.
 *
 * Service and characteristic identifiers are current BlueZ object paths
 * carried as opaque strings. They must be discarded after disconnect,
 * service change, or BlueZ owner loss. Callback delivery is serialized by the
 * D-Bus client's callback worker.
 */
class Dmn_BluezGattClient : public Dmn_GattClient {
public:
  /**
   * @brief Connect to the system bus or an explicitly configured test bus.
   *
   * @param config BlueZ D-Bus connection and resource limits.
   */
  explicit Dmn_BluezGattClient(const Dmn_BluezClientConfig &config = {});

  /**
   * @brief Stop backend work and release the client-owned D-Bus connection.
   */
  ~Dmn_BluezGattClient() override;

  /**
   * @brief BlueZ clients cannot be copied because they own bus resources.
   */
  Dmn_BluezGattClient(const Dmn_BluezGattClient &) = delete;

  /**
   * @brief BlueZ clients cannot be copy-assigned.
   */
  auto operator=(const Dmn_BluezGattClient &) -> Dmn_BluezGattClient & = delete;

  /**
   * @brief BlueZ clients cannot be moved while operations own their state.
   */
  Dmn_BluezGattClient(Dmn_BluezGattClient &&) = delete;

  /**
   * @brief BlueZ clients cannot be move-assigned.
   */
  auto operator=(Dmn_BluezGattClient &&) -> Dmn_BluezGattClient & = delete;

  /** @copydoc Dmn_GattClient::waitForServicesResolved */
  [[nodiscard]] auto waitForServicesResolved(const std::string &deviceId,
                                             std::chrono::milliseconds timeout)
      -> Dmn_GattResult override;

  /** @copydoc Dmn_GattClient::discover */
  [[nodiscard]] auto discover(const std::string &deviceId,
                              std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<std::vector<Dmn_GattService>> override;

  /** @copydoc Dmn_GattClient::startNotify */
  [[nodiscard]] auto
  startNotify(const std::string &deviceId, const std::string &characteristicId,
              NotificationHandler handler, std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<Dmn_GattNotification> override;

  /** @copydoc Dmn_GattClient::writeWithResponse */
  [[nodiscard]] auto writeWithResponse(
      const std::string &deviceId, const std::string &characteristicId,
      const std::vector<std::uint8_t> &value,
      std::chrono::milliseconds timeout) -> Dmn_GattResult override;

private:
  class Impl;
  std::shared_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_BLUEZ_GATT_CLIENT_HPP_
