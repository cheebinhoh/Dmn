/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-ancs-client.hpp
 * @brief Public ANCS values and backend-neutral protocol parsing primitives.
 *
 * Overview
 * --------
 * This header defines the owned value/error vocabulary used by the ANCS
 * client and the bounded protocol parsers shared by that client. It depends
 * only on the backend-neutral GATT contract and standard C++ types; no BlueZ,
 * D-Bus, iOS SDK, or user-interface type crosses this boundary.
 *
 * The notification-source parser decodes one complete eight-byte ANCS event.
 * The data-source reassembler accepts arbitrary byte fragments for one
 * serialized Get Notification Attributes response. It reports completion
 * only after every requested attribute tuple has been fully received and
 * validated.
 *
 * Design constraints
 * ------------------
 * - Notification UIDs and parser instances are scoped to one live ANCS
 *   session; neither is a durable notification identity.
 * - Data Source responses have no independent request identifier. The caller
 *   therefore serializes requests and creates one reassembler per active UID.
 * - Every parser input and accumulated response has a finite byte bound.
 *   Invalid framing is a typed error rather than an unchecked read or
 *   success-shaped partial result.
 */

#ifndef DMN_ANCS_CLIENT_HPP_
#define DMN_ANCS_CLIENT_HPP_

#include "dmn-gatt-client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dmn {

/**
 * @brief ANCS-level failure category, independent of the GATT backend.
 */
enum class Dmn_AncsErrorKind {
  InvalidArgument,
  NotReady,
  Timeout,
  Unsupported,
  Authorization,
  Remote,
  Protocol,
  ResourceLimit,
  Backend,
  Cleanup
};

/**
 * @brief Structured ANCS failure; messages must never contain payload data.
 */
struct Dmn_AncsError {
  Dmn_AncsErrorKind kind{Dmn_AncsErrorKind::Protocol};
  std::string message{};
  std::string remote_name{};
  bool fatal{true};
};

/**
 * @brief Success or typed ANCS failure.
 */
using Dmn_AncsResult = std::expected<void, Dmn_AncsError>;

/**
 * @brief Value result or typed ANCS failure.
 */
template <typename T>
using Dmn_AncsValueResult = std::expected<T, Dmn_AncsError>;

/**
 * @brief Lifecycle states of one ANCS client session.
 */
enum class Dmn_AncsState {
  Stopped,
  Starting,
  ResolvingServices,
  DiscoveringANCS,
  SubscribingDataSource,
  SubscribingNotificationSource,
  Ready,
  Stopping,
  Failed
};

/**
 * @brief Semantic kind of a normalized ANCS notification value.
 */
enum class Dmn_AncsNotificationKind {
  Added,
  Updated,
  Removed,
  AttributesUnavailable
};

/**
 * @brief Owned notification metadata and the requested optional attributes.
 *
 * The category, flags, and category count preserve the raw byte values from
 * Notification Source. Attribute fields are populated only after a complete
 * Data Source response has been received and validated.
 */
struct Dmn_AncsNotification {
  Dmn_AncsNotificationKind kind{Dmn_AncsNotificationKind::Added};
  std::uint32_t uid{};
  std::uint8_t category{};
  std::uint8_t category_count{};
  std::uint8_t flags{};
  std::optional<std::string> app_identifier{};
  std::optional<std::string> title{};
  std::optional<std::string> subtitle{};
  std::optional<std::string> message{};
  std::optional<std::string> date{};
};

/**
 * @brief Finite operation deadlines for an ANCS session.
 */
struct Dmn_AncsConfig {
  std::chrono::milliseconds service_resolution_timeout{30000};
  std::chrono::milliseconds gatt_operation_timeout{10000};
  std::chrono::milliseconds attribute_response_timeout{10000};

  /**
   * @brief Optional cutoff for suppressing older ANCS notifications.
   *
   * ANCS provides a notification date but no arrival timestamp. When set,
   * notifications with an older, missing, or unrecognized date are not
   * delivered to the event handler. A frontend can capture this timestamp at
   * process startup and pass it here when starting the ANCS session.
   */
  std::optional<std::chrono::system_clock::time_point>
      notification_date_cutoff{};

  /**
   * @brief Maximum queued notification-attribute requests.
   *
   * When the limit is reached, the oldest pending request is discarded so
   * newer notifications can be processed. Zero disables this limit.
   */
  std::size_t maximum_pending_notifications{255};
};

/**
 * @brief ANCS event delivered to an application callback.
 *
 * StateChanged carries `state`; notification events carry `notification`;
 * Error carries `error`. AttributesUnavailable carries both the affected
 * notification and its request-scoped error when one is available.
 */
struct Dmn_AncsEvent {
  /**
   * @brief Event categories emitted by one ANCS session.
   */
  enum class Kind {
    StateChanged,
    NotificationAdded,
    NotificationUpdated,
    NotificationRemoved,
    AttributesUnavailable,
    Error
  };

  /**
   * @brief Event category.
   */
  Kind kind{Kind::Error};

  /**
   * @brief New state for StateChanged events.
   */
  std::optional<Dmn_AncsState> state{};

  /**
   * @brief Owned notification value for notification events.
   */
  std::optional<Dmn_AncsNotification> notification{};

  /**
   * @brief Typed error for Error and AttributesUnavailable events.
   */
  std::optional<Dmn_AncsError> error{};
};

/**
 * @brief Reusable, backend-neutral ANCS session over one borrowed GATT client.
 *
 * The client never pairs or connects the selected device. Its caller must
 * arrange link setup first. GATT callbacks are copied to ANCS-owned queues;
 * application events are serialized on an ANCS worker and never delivered on
 * the backend's callback thread. Control Point writes use a separate worker
 * so a slow acknowledged write cannot block notification intake.
 */
class Dmn_AncsClient {
public:
  /**
   * @brief Callback receiving owned state, notification, and error events.
   */
  using EventHandler = std::function<void(const Dmn_AncsEvent &)>;

  /**
   * @brief Create a client borrowing one backend-neutral GATT client.
   *
   * @param gatt GATT backend that must outlive this client.
   */
  explicit Dmn_AncsClient(Dmn_GattClient &gatt);

  /**
   * @brief Best-effort session cleanup; never throws.
   */
  virtual ~Dmn_AncsClient() noexcept;

  /**
   * @brief Clients cannot be copied because they own worker state.
   */
  Dmn_AncsClient(const Dmn_AncsClient &) = delete;

  /**
   * @brief Clients cannot be copy-assigned.
   */
  auto operator=(const Dmn_AncsClient &) -> Dmn_AncsClient & = delete;

  /**
   * @brief Clients cannot be moved while callbacks retain their state.
   */
  Dmn_AncsClient(Dmn_AncsClient &&) = delete;

  /**
   * @brief Clients cannot be move-assigned.
   */
  auto operator=(Dmn_AncsClient &&) -> Dmn_AncsClient & = delete;

  /**
   * @brief Install or clear the event callback while the session is stopped.
   *
   * @param handler Callback invoked serially on the ANCS worker. It must
   * return promptly, must not throw, and must not call start(), stop(), or
   * destroy this client.
   * @return Success or NotReady if the client is not stopped.
   */
  [[nodiscard]] auto setEventHandler(EventHandler handler) -> Dmn_AncsResult;

  /**
   * @brief Resolve ANCS on a ready GATT link and subscribe to its sources.
   *
   * This bounded synchronous operation waits for service resolution, locates
   * the three required ANCS characteristics, subscribes to Data Source first,
   * then Notification Source, and returns only after both subscriptions are
   * ready. It does not pair or connect the device.
   *
   * @param deviceId Opaque identifier accepted by the borrowed GATT backend.
   * @param config Finite per-operation and response deadlines.
   * @return Success or a typed validation, GATT, protocol, or cleanup error.
   */
  [[nodiscard]] auto start(const std::string &deviceId,
                           const Dmn_AncsConfig &config = {}) -> Dmn_AncsResult;

  /**
   * @brief Stop the session and explicitly release acquired subscriptions.
   *
   * The operation is idempotent. It prevents new event intake before cleanup,
   * attempts both subscription stops, and reports cleanup failures.
   *
   * @return Success or the first typed cleanup error.
   */
  [[nodiscard]] auto stop() -> Dmn_AncsResult;

  /**
   * @brief Return a thread-safe snapshot of the current session state.
   */
  [[nodiscard]] auto state() const -> Dmn_AncsState;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

namespace detail {

/**
 * @brief Parse one ANCS Notification Source notification value.
 *
 * A valid value is exactly eight bytes. Unknown category and flag bits are
 * retained verbatim; an unknown event ID is returned as a nonfatal
 * Unsupported error so a session can report and ignore it.
 */
[[nodiscard]] auto
parseAncsNotificationSource(std::span<const std::uint8_t> value)
    -> Dmn_AncsValueResult<Dmn_AncsNotification>;

/**
 * @brief Requested textual fields from one complete Data Source response.
 *
 * An absent optional means that ANCS supplied a zero-length value. An empty
 * string is therefore a present attribute with an empty value, not absence.
 */
struct Dmn_AncsAttributeSet {
  std::optional<std::string> app_identifier{};
  std::optional<std::string> title{};
  std::optional<std::string> subtitle{};
  std::optional<std::string> message{};
  std::optional<std::string> date{};
};

/**
 * @brief Bounded reassembler for one serialized Get Notification Attributes
 * response.
 *
 * The caller must submit fragments in wire order and must not overlap this
 * reassembler with another Control Point request. A successful result
 * contains an attribute set; an empty optional means more fragments are
 * required. On completion or failure the instance is terminal and must not be
 * reused.
 */
class Dmn_AncsDataSourceReassembler {
public:
  /**
   * @brief Create a parser bound to the expected session-scoped UID.
   */
  explicit Dmn_AncsDataSourceReassembler(std::uint32_t expectedUid);

  /**
   * @brief Append and parse one bounded Data Source fragment.
   *
   * @param fragment The next notification value fragment in wire order.
   * @return An empty optional while incomplete, completed owned attributes,
   * or a typed framing/resource/UTF-8 error.
   */
  [[nodiscard]] auto consume(std::span<const std::uint8_t> fragment)
      -> Dmn_AncsValueResult<std::optional<Dmn_AncsAttributeSet>>;

private:
  std::uint32_t m_expected_uid{};
  std::size_t m_total_bytes{};
  std::size_t m_expected_attribute_index{};
  bool m_header_seen{};
  bool m_terminal{};
  std::vector<std::uint8_t> m_buffer{};
  Dmn_AncsAttributeSet m_attributes{};
};

} // namespace detail
} // namespace dmn

#endif // DMN_ANCS_CLIENT_HPP_
