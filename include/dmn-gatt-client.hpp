/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-gatt-client.hpp
 * @brief Backend-neutral remote GATT client contract.
 */

#ifndef DMN_GATT_CLIENT_HPP_
#define DMN_GATT_CLIENT_HPP_

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dmn {

/** @brief Backend-neutral category for a failed GATT operation. */
enum class Dmn_GattErrorKind {
  /** @brief The supplied device or GATT identifier is invalid. */
  InvalidArgument,
  /** @brief The device is disconnected or its GATT tree is not ready. */
  NotReady,
  /** @brief The finite operation deadline expired. */
  Timeout,
  /** @brief BlueZ or another backend rejected the operation. */
  Remote,
  /** @brief A transport or local backend failure occurred. */
  Backend,
  /** @brief A D-Bus value did not match the GATT object contract. */
  Decode
};

/** @brief Backend-neutral typed error for GATT operations. */
struct Dmn_GattError {
  /** @brief Local error category. */
  Dmn_GattErrorKind kind{Dmn_GattErrorKind::Backend};
  /** @brief Human-readable failure detail. */
  std::string message;
  /** @brief Original remote error name, when reported by the backend. */
  std::string remote_name;
};

/** @brief A GATT operation result carrying either a value or typed error. */
template <typename T>
using Dmn_GattValueResult = std::expected<T, Dmn_GattError>;
/** @brief A GATT operation result carrying success or a typed error. */
using Dmn_GattResult = std::expected<void, Dmn_GattError>;

/**
 * @brief A remote GATT characteristic and its advertised capabilities.
 */
struct Dmn_GattCharacteristic {
  /** @brief Backend-owned opaque characteristic identifier. */
  std::string id;
  /** @brief Canonical uppercase UUID reported by the backend. */
  std::string uuid;
  /** @brief Backend-neutral capability names, such as `notify` or `write`. */
  std::vector<std::string> flags;
};

/**
 * @brief A remote GATT service and its currently discovered characteristics.
 */
struct Dmn_GattService {
  /** @brief Backend-owned opaque service identifier. */
  std::string id;
  /** @brief Canonical uppercase UUID reported by the backend. */
  std::string uuid;
  /** @brief Whether the service is primary. */
  bool primary{};
  /** @brief Characteristics belonging to this service. */
  std::vector<Dmn_GattCharacteristic> characteristics;
};

/**
 * @brief One notification or invalidation from a GATT subscription.
 */
struct Dmn_GattEvent {
  /** @brief Category of notification callback event. */
  enum class Kind {
    /** @brief Characteristic value changed. */
    ValueChanged,
    /** @brief The device, service, or backend session became invalid. */
    Invalidated,
    /** @brief A typed backend error occurred. */
    Error
  };

  /** @brief Event kind. */
  Kind kind{Kind::Invalidated};
  /** @brief Owned characteristic bytes for ValueChanged. */
  std::vector<std::uint8_t> value;
  /** @brief Human-readable detail for invalidation or error events. */
  std::string message;
};

/**
 * @brief Move-constructible owner for one enabled characteristic notification.
 *
 * Call stop() to observe a StopNotify failure. Destruction also attempts
 * cleanup, but cannot report a remote cleanup error. The token retains the
 * backend resources needed to stop the notification even if its client wrapper
 * is destroyed first. Move assignment is disabled because it cannot report a
 * failed cleanup of the currently owned notification.
 */
class Dmn_GattNotification {
public:
  /** @brief Construct an empty notification token. */
  Dmn_GattNotification() = default;
  /**
   * @brief Construct a token with backend cleanup ownership.
   *
   * @param stop Callback that disables the notification.
   */
  explicit Dmn_GattNotification(std::function<Dmn_GattResult()> stop)
      : m_stop{std::move(stop)} {}
  /**
   * @brief Stop the owned notification when this token is destroyed.
   *
   * Cleanup failures cannot be reported during destruction.
   */
  virtual ~Dmn_GattNotification() noexcept { (void)stop(); }

  /** @brief Notification cleanup ownership cannot be copied. */
  Dmn_GattNotification(const Dmn_GattNotification &) = delete;

  /** @brief Notification cleanup ownership cannot be copy-assigned. */
  auto
  operator=(const Dmn_GattNotification &) -> Dmn_GattNotification & = delete;

  /** @brief Transfer notification cleanup ownership. */
  Dmn_GattNotification(Dmn_GattNotification &&other) noexcept
      : m_stop{std::exchange(other.m_stop, {})} {}

  /**
   * @brief Notification cleanup ownership cannot be move-assigned.
   *
   * A failed stop must remain retryable; move assignment cannot report that
   * failure without discarding or obscuring cleanup ownership.
   */
  auto operator=(Dmn_GattNotification &&) -> Dmn_GattNotification & = delete;

  /**
   * @brief Stop the notification and return any backend failure.
   *
   * @return Success or a typed backend error. Exceptions thrown by the cleanup
   * callback are converted to `Dmn_GattErrorKind::Backend`.
   */
  [[nodiscard]] auto stop() -> Dmn_GattResult {
    if (!m_stop) {
      return {};
    }

    auto stop = std::move(m_stop);
    try {
      auto result = stop();
      if (!result) {
        m_stop = std::move(stop);
      }

      return result;
    } catch (const std::exception &exception) {
      m_stop = std::move(stop);

      return std::unexpected(
          Dmn_GattError{Dmn_GattErrorKind::Backend, exception.what(), {}});
    } catch (...) {
      m_stop = std::move(stop);

      return std::unexpected(Dmn_GattError{
          Dmn_GattErrorKind::Backend,
          "notification cleanup callback failed with a non-standard "
          "exception",
          {}});
    }
  }

private:
  std::function<Dmn_GattResult()> m_stop;
};

/**
 * @brief Backend-neutral, bounded GATT operations for one selected device.
 *
 * Device, service, and characteristic identifiers are opaque to callers and
 * are valid only for the current connection/service-tree lifetime. Blocking
 * operations are synchronous and have finite deadlines; notification events
 * are delivered asynchronously on the backend's documented serialized
 * worker. Callers must keep blocking operations off UI threads and notification
 * callbacks prompt.
 */
class Dmn_GattClient {
public:
  /** @brief Callback invoked for each notification or invalidation event. */
  using NotificationHandler = std::function<void(const Dmn_GattEvent &)>;

  /** @brief Destroy the GATT backend interface polymorphically. */
  virtual ~Dmn_GattClient() noexcept = default;

  /** @brief GATT backend instances cannot be copied. */
  Dmn_GattClient(const Dmn_GattClient &) = delete;

  /** @brief GATT backend instances cannot be copy-assigned. */
  auto operator=(const Dmn_GattClient &) -> Dmn_GattClient & = delete;

  /** @brief GATT backend instances cannot be moved. */
  Dmn_GattClient(Dmn_GattClient &&) = delete;

  /** @brief GATT backend instances cannot be move-assigned. */
  auto operator=(Dmn_GattClient &&) -> Dmn_GattClient & = delete;

protected:
  /** @brief Initialize the backend-neutral contract for a derived backend. */
  Dmn_GattClient() = default;

public:
  /**
   * @brief Wait until the selected device's GATT service tree is resolved.
   *
   * @param deviceId Opaque identifier for the selected device.
   * @param timeout Positive finite deadline for the wait.
   * @return Success when connected services are resolved, otherwise a typed
   * error.
   */
  [[nodiscard]] virtual auto waitForServicesResolved(
      const std::string &deviceId,
      std::chrono::milliseconds timeout) -> Dmn_GattResult = 0;

  /**
   * @brief Discover the currently resolved services and characteristics.
   *
   * @param deviceId Opaque identifier for the selected device.
   * @param timeout Positive finite deadline for discovery.
   * @return Owned service and characteristic snapshots or a typed error.
   */
  [[nodiscard]] virtual auto discover(const std::string &deviceId,
                                      std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<std::vector<Dmn_GattService>> = 0;

  /**
   * @brief Enable notifications and receive subsequent value changes.
   *
   * @param deviceId Opaque identifier for the selected device.
   * @param characteristicId Opaque identifier from the current GATT snapshot.
   * @param handler Callback invoked asynchronously for characteristic values
   * and invalidation events.
   * @param timeout Positive finite deadline for subscription setup.
   * @return A move-only notification token or a typed setup error.
   */
  [[nodiscard]] virtual auto
  startNotify(const std::string &deviceId, const std::string &characteristicId,
              NotificationHandler handler, std::chrono::milliseconds timeout)
      -> Dmn_GattValueResult<Dmn_GattNotification> = 0;

  /**
   * @brief Write bytes using the backend's acknowledged/write-request mode.
   *
   * @param deviceId Opaque identifier for the selected device.
   * @param characteristicId Opaque identifier from the current GATT snapshot.
   * @param value Bytes to write.
   * @param timeout Positive finite deadline for the write.
   * @return Success after the backend acknowledges the write, otherwise a
   * typed error.
   */
  [[nodiscard]] virtual auto
  writeWithResponse(const std::string &deviceId,
                    const std::string &characteristicId,
                    const std::vector<std::uint8_t> &value,
                    std::chrono::milliseconds timeout) -> Dmn_GattResult = 0;
};

} // namespace dmn

#endif // DMN_GATT_CLIENT_HPP_
