/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-client.hpp
 * @brief Optional generic D-Bus client API.
 */

#ifndef DMN_DBUS_CLIENT_HPP_
#define DMN_DBUS_CLIENT_HPP_

#include "dmn-dbus-value.hpp"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dmn {

/**
 * @brief Selects the D-Bus bus to which this client connects.
 *
 * Session and System select the standard buses by kind. Address selects the
 * exact caller-supplied address of a D-Bus bus daemon; that address can refer
 * to either standard bus or to a separately started private bus instance.
 * Every option creates a dedicated client connection. "Private connection"
 * means a connection owned by this client, not a separate bus daemon. Direct
 * peer-to-peer connections to a service process are not supported.
 */
enum class Dmn_DbusBusKind {
  /**
   * @brief Open a dedicated client connection to the current user's session
   * bus.
   *
   * This uses the shared session bus; it does not start a separate bus.
   */
  Session,

  /**
   * @brief Open a dedicated client connection to the machine's system bus.
   *
   * This uses the shared system bus; it does not start a separate bus.
   */
  System,

  /**
   * @brief Connect to the bus daemon at the exact caller-supplied address.
   *
   * Use this for an explicitly addressed system/session bus or a separately
   * started private bus instance. The address must identify a bus daemon, not
   * a service process accepting a direct peer-to-peer connection. No
   * session/system fallback is attempted.
   */
  Address
};

/** @brief Local error category for a failed D-Bus operation. */
enum class Dmn_DbusErrorKind {
  /** @brief Input or configuration did not satisfy the D-Bus API contract. */
  InvalidArgument,

  /** @brief No usable connection exists for the requested operation. */
  NotConnected,

  /** @brief A local transport or connection failure occurred. */
  Transport,

  /** @brief The finite method-call deadline expired. */
  Timeout,

  /** @brief The caller cancelled the operation. */
  Cancelled,

  /** @brief The client is shutting down or has already shut down. */
  Shutdown,

  /** @brief A configured resource bound rejected the operation. */
  ResourceLimit,

  /** @brief The remote service returned a named D-Bus error. */
  Remote,

  /** @brief A D-Bus value could not be encoded or decoded. */
  Codec
};

/**
 * @brief Structured failure returned by a D-Bus client operation.
 *
 * Local failures are identified by kind. For a remote D-Bus error, kind is
 * Remote and remote_name retains the service's D-Bus error name.
 */
struct Dmn_DbusError {
  /** @brief Local category describing the failed D-Bus operation. */
  Dmn_DbusErrorKind kind{Dmn_DbusErrorKind::Transport};

  /** @brief Human-readable local or remote error message. */
  std::string message;

  /** @brief Remote D-Bus error name, empty for local failures. */
  std::string remote_name;
};

/** @brief Success value or structured D-Bus operation failure. */
template <typename T> using Dmn_DbusResult = std::expected<T, Dmn_DbusError>;

/**
 * @brief Finite resource bounds applied to one generic D-Bus client.
 *
 * Configuration values must be positive and within the libdbus/platform
 * limits checked during client construction. Calls beyond the effective
 * in-flight cap are rejected. Signal deliveries beyond either callback-queue
 * cap are dropped and counted in Dmn_DbusClientStatus.
 */
struct Dmn_DbusClientLimits {
  /** @brief Default finite timeout for method calls. */
  std::chrono::milliseconds method_timeout{5000};

  /** @brief Maximum admitted method calls awaiting completion. */
  std::size_t max_in_flight_calls{128};

  /**
   * @brief Additional cap on in-flight calls and active subscriptions.
   *
   * There is no separate local command FIFO in this implementation. The
   * in-flight call cap is the smaller of this value and
   * max_in_flight_calls.
   */
  std::size_t max_queued_commands{256};

  /**
   * @brief Maximum queued callback deliveries across all signal matches.
   *
   * One incoming signal matching multiple subscriptions occupies one queue
   * entry per subscription.
   */
  std::size_t max_signal_messages{256};

  /**
   * @brief Maximum serialized signal bytes retained by queued deliveries.
   *
   * Accounting includes D-Bus message framing and charges once per callback
   * delivery, so a signal matching multiple subscriptions is charged multiple
   * times.
   */
  std::size_t max_signal_bytes{16U * 1024U * 1024U};

  /**
   * @brief Maximum serialized method-call, reply, or signal message size.
   *
   * The limit includes D-Bus message framing, not only the argument body.
   */
  std::size_t max_message_bytes{8U * 1024U * 1024U};

  /** @brief Maximum recursive array/dictionary/struct nesting depth. */
  std::size_t max_container_depth{32};

  /** @brief Maximum values or entries accepted in one message. */
  std::size_t max_values{65536};
};

/** @brief Bus selection and resource limits for a Dmn_DbusClient. */
struct Dmn_DbusClientConfig {
  /**
   * @brief Selects the session bus, system bus, or explicit address.
   *
   * All choices create a dedicated private connection owned by
   * Dmn_DbusClient. The connection is private to this client; the selected
   * system, session, or separately started private bus may be shared with
   * other processes. Defaults to Session.
   */
  Dmn_DbusBusKind bus_kind{Dmn_DbusBusKind::Session};

  /**
   * @brief Exact bus-daemon address, required only for
   * `Dmn_DbusBusKind::Address`.
   *
   * The address can identify a standard system/session bus or a separately
   * started private bus instance. It must identify a bus daemon, not a
   * service process listening for a direct peer-to-peer connection.
   */
  std::string bus_address;

  /** @brief Finite resource limits for calls and signals. */
  Dmn_DbusClientLimits limits;
};

/**
 * @brief D-Bus match criteria used to install a signal subscription.
 *
 * Interface and member are required. Sender is optional; at most one of path
 * or path_namespace may be supplied. The same criteria are applied by the
 * daemon and checked locally before callback delivery.
 */
struct Dmn_DbusSignalMatch {
  /** @brief Optional well-known or unique sender name. */
  std::string sender;

  /** @brief Required signal interface. */
  std::string interface;

  /** @brief Required signal member. */
  std::string member;

  /** @brief Optional exact object path. */
  std::string path;

  /** @brief Optional object-path namespace prefix. */
  std::string path_namespace;
};

/**
 * @brief Point-in-time connection, work-queue, and failure snapshot.
 *
 * Signal-drop and shutdown-discard counts are cumulative for the client
 * lifetime. Queue sizes and pending-call counts describe the snapshot moment;
 * no field indicates that a remote service applied an operation.
 */
struct Dmn_DbusClientStatus {
  /** @brief Whether this client's dedicated connection is currently usable. */
  bool connected{};

  /** @brief Whether shutdown has stopped new operation admission. */
  bool shutting_down{};

  /** @brief Number of pending method calls. */
  std::size_t pending_calls{};

  /**
   * @brief Number of callback deliveries waiting in the signal queue.
   *
   * A single signal matching multiple subscriptions contributes multiple
   * queued deliveries.
   */
  std::size_t queued_signal_messages{};

  /**
   * @brief Bytes charged to queued callback deliveries, including framing.
   *
   * A signal matched to multiple subscriptions is charged once per queued
   * callback.
   */
  std::size_t queued_signal_bytes{};

  /**
   * @brief Incoming signals dropped because a callback-queue cap was reached.
   *
   * Each incoming signal is counted once even if multiple subscriptions
   * matched it.
   */
  std::size_t dropped_signal_messages{};

  /**
   * @brief Serialized bytes of incoming signals dropped at the queue bound.
   *
   * Each incoming signal is counted once even when it matched multiple
   * subscriptions.
   */
  std::size_t dropped_signal_bytes{};

  /** @brief Queued callback deliveries discarded during shutdown. */
  std::size_t shutdown_discarded_signal_messages{};

  /**
   * @brief Bytes charged to queued callback deliveries discarded at shutdown.
   */
  std::size_t shutdown_discarded_signal_bytes{};

  /**
   * @brief Human-readable terminal client, codec, or callback error, if any.
   */
  std::string terminal_error;
};

/**
 * @brief Move-only RAII token that owns one signal subscription.
 *
 * Destroying or explicitly cancelling the token removes its match and
 * suppresses future callback admission. A default-constructed token is empty.
 * Cancellation is idempotent and does not wait for a callback already running
 * on the client's callback worker to finish. The token may outlive its client;
 * cancellation then has no effect because shutdown has already removed the
 * subscription.
 */
class Dmn_DbusSubscription final {
public:
  /**
   * @brief Construct an empty subscription token.
   *
   * An empty token has no cancellation action.
   */
  Dmn_DbusSubscription() = default;

  /** @brief Construct a subscription token with its cancellation action. */
  explicit Dmn_DbusSubscription(std::function<void()> cancel);

  /** @brief Cancel the subscription when this token is destroyed. */
  ~Dmn_DbusSubscription();

  /** @brief Subscriptions cannot be copied. */
  Dmn_DbusSubscription(const Dmn_DbusSubscription &) = delete;

  /** @brief Subscriptions cannot be copy-assigned. */
  auto
  operator=(const Dmn_DbusSubscription &) -> Dmn_DbusSubscription & = delete;

  /** @brief Transfer cancellation ownership from another token. */
  Dmn_DbusSubscription(Dmn_DbusSubscription &&other) noexcept;

  /** @brief Transfer cancellation ownership from another token. */
  auto
  operator=(Dmn_DbusSubscription &&other) noexcept -> Dmn_DbusSubscription &;

  /** @brief Cancel this subscription; cancellation is idempotent. */
  void cancel() noexcept;

private:
  std::function<void()> m_cancel;
};

/**
 * @brief Move-only handle for one asynchronous method-call result.
 *
 * The handle owns the future and an explicit cancellation action. Destroying
 * or replacing it does not cancel the operation; use cancel() when required.
 * get() consumes the result once, and waitFor() observes readiness without
 * consuming it.
 */
class Dmn_DbusCall final {
public:
  /** @brief Owned sequence of method-reply arguments. */
  using Arguments = std::vector<Dmn_DbusValue>;

  /** @brief Method reply arguments or the corresponding typed failure. */
  using Result = Dmn_DbusResult<Arguments>;

  /**
   * @brief Own an asynchronous result future and its cancellation action.
   *
   * Destroying or replacing this handle does not cancel the submitted call;
   * call cancel() explicitly when cancellation is required.
   */
  Dmn_DbusCall(std::future<Result> future, std::function<void()> cancel);

  /** @brief Transfer ownership of an asynchronous call. */
  Dmn_DbusCall(Dmn_DbusCall &&) noexcept = default;

  /** @brief Transfer ownership of an asynchronous call. */
  auto operator=(Dmn_DbusCall &&) noexcept -> Dmn_DbusCall & = default;

  /** @brief Asynchronous calls cannot be copied. */
  Dmn_DbusCall(const Dmn_DbusCall &) = delete;

  /** @brief Asynchronous calls cannot be copy-assigned. */
  auto operator=(const Dmn_DbusCall &) -> Dmn_DbusCall & = delete;

  /** @brief Wait for a result without consuming it. */
  template <typename Rep, typename Period>
  auto waitFor(const std::chrono::duration<Rep, Period> &duration) const
      -> std::future_status {
    return m_future.wait_for(duration);
  }

  /** @brief Wait for and retrieve the single asynchronous call result. */
  [[nodiscard]] auto get() -> Result;

  /** @brief Cancel the call; cancellation is idempotent. */
  void cancel() noexcept;

private:
  std::future<Result> m_future;
  std::function<void()> m_cancel;
};

/**
 * @brief Generic, typed client for calling services on a D-Bus daemon.
 *
 * Architecture
 * ------------
 * This library is a D-Bus client, not a bus daemon or a D-Bus service. The
 * selected bus daemon must already be running. A service process connects to
 * that same bus, owns the destination name, and exports the requested methods
 * and objects. A client sends a method call through its dedicated private
 * connection; the bus daemon routes it to the named service and routes the
 * reply back. Signal subscriptions install match rules on the bus daemon.
 * A method call receives either a method-return message or an error message;
 * a signal is a one-way announcement without a method reply. This client uses
 * one bidirectional connection: caller threads submit pending calls, while
 * its dispatch worker reads and dispatches incoming replies. Completion
 * decodes the result and fulfills the call future; call() waits on the caller
 * thread, while callAsync() exposes a Dmn_DbusCall handle. The four D-Bus
 * message types and full request/reply flow are described in
 * docs/specs/dbus-client-spec.md.
 *
 * All supported connection choices use a bus daemon. In Address mode, the
 * supplied address is opened and registered as a bus connection. This client
 * does not support direct peer-to-peer D-Bus connections, where a client
 * connects directly to a service process without bus routing.
 *
 * Dmn_DbusValue is the owned, type-preserving message-body model. The private
 * codec translates values to and from libdbus messages, so callers do not
 * manage DBusMessage, DBusMessageIter, or borrowed message memory. This layer
 * provides generic D-Bus operations; service-specific meaning belongs in a
 * higher-level client such as Dmn_BluezClient.
 *
 * Concurrency and lifetime
 * ------------------------
 * Each Dmn_DbusClient owns one connection, one I/O/dispatch worker, and one
 * serial signal-callback worker. Method replies are completed by the dispatch
 * worker. Signal callbacks receive owned values on the callback worker, not
 * on the dispatch worker. Signal callback deliveries and serialized message
 * sizes are bounded by Dmn_DbusClientLimits; overload is reflected in
 * status().
 *
 * The synchronous call() helper waits on its calling thread and must not be
 * called from the dispatch worker. Do not call it from a signal callback
 * either: that would block the serial callback worker and delay all later
 * callbacks. User callbacks must return promptly and must not destroy the
 * client; concurrent destruction with public calls is unsupported.
 * shutdown() stops new admission, resolves outstanding calls with a shutdown
 * result, joins the workers, and releases the connection.
 * The client does not automatically reconnect after a terminal disconnect.
 *
 * Bus selection and security
 * --------------------------
 * Dmn_DbusClientConfig selects the standard session bus, the standard system
 * bus, or an explicit bus-daemon address. The explicit address may identify
 * either standard bus or a separately started private bus instance. In every
 * case this client owns a dedicated private connection to that bus; it does
 * not start or manage the daemon. A private bus is a separate daemon instance,
 * not another name for the client's private connection. This client does not
 * support direct peer-to-peer connections and never falls back to another bus.
 * Successful connection does not imply permission to call every service:
 * the bus daemon's policy and the target service's authorization still apply.
 *
 * Typical use:
 * @code
 * Dmn_DbusClient client;
 * auto reply = client.call("org.example.Service", "/org/example/Object",
 *                          "org.example.Interface", "GetValue");
 * if (reply) {
 *   // Inspect the owned Dmn_DbusValue reply arguments.
 * }
 * @endcode
 */
class Dmn_DbusClient {
public:
  /** @brief Owned argument list used for method replies and signal bodies. */
  using Arguments = std::vector<Dmn_DbusValue>;

  /** @brief Signal callback receiving owned decoded arguments. */
  using SignalCallback = std::function<void(Arguments)>;

  /** @brief Signal callback receiving its object path and owned arguments. */
  using SignalCallbackWithPath = std::function<void(std::string, Arguments)>;

  /**
   * @brief Connect to the explicitly selected bus without fallback.
   *
   * @param config Bus selection and finite resource limits.
   * @throws std::invalid_argument if configuration is invalid.
   * @throws std::system_error if the selected bus cannot be opened or
   * registered.
   */
  explicit Dmn_DbusClient(const Dmn_DbusClientConfig &config = {});

  /**
   * @brief Stop the client and release its dedicated bus connection.
   *
   * Equivalent to shutdown(). Destruction from a client-owned callback or
   * dispatch worker is unsupported because shutdown joins those workers.
   */
  virtual ~Dmn_DbusClient();

  /**
   * @brief Stop admission, complete outstanding calls, and release resources.
   *
   * Safe to call repeatedly from an external thread. Pending calls complete
   * with a Shutdown result; queued signal callbacks may be discarded and are
   * counted in status(). Do not call from a client-owned worker.
   */
  void shutdown() noexcept;

  /** @brief Clients cannot be copied because they own a connection and workers.
   */
  Dmn_DbusClient(const Dmn_DbusClient &) = delete;

  /** @brief Clients cannot be copy-assigned. */
  auto operator=(const Dmn_DbusClient &) -> Dmn_DbusClient & = delete;

  /** @brief Clients cannot be moved because their workers retain client state.
   */
  Dmn_DbusClient(Dmn_DbusClient &&) = delete;

  /** @brief Clients cannot be move-assigned. */
  auto operator=(Dmn_DbusClient &&) -> Dmn_DbusClient & = delete;

  /**
   * @brief Submit a bounded asynchronous method call with a finite timeout.
   *
   * A zero timeout selects the configured default. The returned move-only
   * handle owns the result future and can cancel the operation.
   *
   * @param destination Well-known or unique destination name on the selected
   *                    bus.
   * @param path Target object path.
   * @param interface Target interface.
   * @param member Method name.
   * @param arguments Owned method arguments.
   * @param timeout Per-call timeout, or zero for the configured default.
   * @return Handle for awaiting or cancelling this call.
   */
  [[nodiscard]] auto
  callAsync(const std::string &destination, const std::string &path,
            const std::string &interface, const std::string &member,
            Arguments arguments = {},
            std::chrono::milliseconds timeout =
                std::chrono::milliseconds::zero()) -> Dmn_DbusCall;

  /**
   * @brief Call a method and wait on the calling thread for its result.
   *
   * Do not call this from the client's signal-callback worker.
   *
   * @param destination Well-known or unique destination name on the selected
   *                    bus.
   * @param path Target object path.
   * @param interface Target interface.
   * @param member Method name.
   * @param arguments Owned method arguments.
   * @param timeout Per-call timeout, or zero for the configured default.
   * @return Owned reply arguments or a typed failure.
   */
  [[nodiscard]] auto
  call(const std::string &destination, const std::string &path,
       const std::string &interface, const std::string &member,
       const Arguments &arguments = {},
       std::chrono::milliseconds timeout = std::chrono::milliseconds::zero())
      -> Dmn_DbusResult<Arguments>;

  /**
   * @brief Make a finite-timeout Properties.Get request.
   *
   * @param destination Well-known or unique destination name on the selected
   *                    bus.
   * @param path Object path hosting the property.
   * @param interface Interface that declares the property.
   * @param property Property name.
   * @return The owned property value or a typed call/decode error.
   */
  [[nodiscard]] auto
  getProperty(const std::string &destination, const std::string &path,
              const std::string &interface,
              const std::string &property) -> Dmn_DbusResult<Dmn_DbusValue>;

  /**
   * @brief Set a remote property using the Properties.Set method.
   *
   * @param destination Well-known or unique destination name on the selected
   *                    bus.
   * @param path Object path hosting the property.
   * @param interface Interface that declares the property.
   * @param property Property name.
   * @param value Owned value to assign.
   * @return Success or a typed call/encode error.
   */
  [[nodiscard]] auto setProperty(const std::string &destination,
                                 const std::string &path,
                                 const std::string &interface,
                                 const std::string &property,
                                 Dmn_DbusValue value) -> Dmn_DbusResult<void>;

  /**
   * @brief Subscribe to matching signals with owned arguments.
   *
   * Both interface and member are required in `match`. The bus daemon applies
   * the match to signals on the selected bus; this is not a subscription over
   * a direct peer-to-peer connection. The callback runs serially on the
   * client-owned callback worker, not the dispatch thread.
   *
   * @param match Constrained sender/interface/member/path match.
   * @param callback Receives owned decoded arguments.
   * @return A move-only cancellation token or a typed setup failure.
   */
  [[nodiscard]] auto
  subscribe(const Dmn_DbusSignalMatch &match,
            SignalCallback callback) -> Dmn_DbusResult<Dmn_DbusSubscription>;

  /**
   * @brief Subscribe while retaining the emitting signal's object path.
   *
   * @param match Constrained sender/interface/member/path match.
   * @param callback Receives the signal path and owned decoded arguments.
   * @return A move-only cancellation token or a typed setup failure.
   */
  [[nodiscard]] auto subscribeWithPath(const Dmn_DbusSignalMatch &match,
                                       SignalCallbackWithPath callback)
      -> Dmn_DbusResult<Dmn_DbusSubscription>;

  /**
   * @brief Wait for all signal callbacks queued before this call to finish.
   *
   * Calling this from a signal callback is invalid because that would wait for
   * the current callback to complete.
   *
   * @return Success after the callback queue reaches this synchronization
   * point, or a typed error if the worker has shut down.
   */
  [[nodiscard]] auto drainSignalCallbacks() -> Dmn_DbusResult<void>;

  /**
   * @brief Return the identifier of the client-owned libdbus dispatch thread.
   * @return Thread identifier; it is stable for the lifetime of the client.
   */
  [[nodiscard]] auto dispatchThreadId() const -> std::thread::id;

  /**
   * @brief Return a thread-safe snapshot of connection and queue status.
   * @return Current connection, queue, and terminal-error counters.
   */
  [[nodiscard]] auto status() const -> Dmn_DbusClientStatus;

private:
  class Impl;
  std::shared_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_DBUS_CLIENT_HPP_
