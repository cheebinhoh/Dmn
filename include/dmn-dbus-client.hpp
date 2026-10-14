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

/** @brief Selects the bus source for one private D-Bus connection. */
enum class Dmn_DbusBusKind {
  /** @brief Connect to the session bus. */
  Session,

  /** @brief Connect to the system bus. */
  System,

  /** @brief Connect to the exact caller-supplied address. */
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

struct Dmn_DbusError {
  /** @brief Local category describing the failed D-Bus operation. */
  Dmn_DbusErrorKind kind{Dmn_DbusErrorKind::Transport};

  /** @brief Human-readable local or remote error message. */
  std::string message;

  /** @brief Remote D-Bus error name, empty for local failures. */
  std::string remote_name;
};

template <typename T> using Dmn_DbusResult = std::expected<T, Dmn_DbusError>;

struct Dmn_DbusClientLimits {
  /** @brief Default finite timeout for method calls. */
  std::chrono::milliseconds method_timeout{5000};

  /** @brief Maximum admitted method calls awaiting completion. */
  std::size_t max_in_flight_calls{128};

  /** @brief Maximum admitted commands waiting to be sent. */
  std::size_t max_queued_commands{256};

  /** @brief Maximum number of queued signal messages. */
  std::size_t max_signal_messages{256};

  /** @brief Maximum bytes of queued signal bodies. */
  std::size_t max_signal_bytes{16U * 1024U * 1024U};

  /** @brief Maximum encoded or decoded body size. */
  std::size_t max_message_bytes{8U * 1024U * 1024U};

  /** @brief Maximum recursive array/dictionary/struct nesting depth. */
  std::size_t max_container_depth{32};

  /** @brief Maximum values or entries accepted in one message. */
  std::size_t max_values{65536};
};

struct Dmn_DbusClientConfig {
  /** @brief Bus selection; defaults to the session bus. */
  Dmn_DbusBusKind bus_kind{Dmn_DbusBusKind::Session};

  /** @brief Explicit address, used only when `bus_kind` is `Address`. */
  std::string bus_address;

  /** @brief Finite resource limits for calls and signals. */
  Dmn_DbusClientLimits limits;
};

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

struct Dmn_DbusClientStatus {
  /** @brief Whether the private connection is currently usable. */
  bool connected{};

  /** @brief Whether shutdown has stopped new operation admission. */
  bool shutting_down{};

  /** @brief Number of pending method calls. */
  std::size_t pending_calls{};

  /** @brief Number of signal messages waiting for callbacks. */
  std::size_t queued_signal_messages{};

  /** @brief Bytes retained by queued signal messages. */
  std::size_t queued_signal_bytes{};

  /** @brief Signal messages dropped because the callback queue was full. */
  std::size_t dropped_signal_messages{};

  /** @brief Bytes dropped because the callback queue was full. */
  std::size_t dropped_signal_bytes{};

  /** @brief Signal messages discarded during shutdown. */
  std::size_t shutdown_discarded_signal_messages{};

  /** @brief Bytes discarded during shutdown. */
  std::size_t shutdown_discarded_signal_bytes{};

  /** @brief Human-readable terminal connection error, if any. */
  std::string terminal_error;
};

class Dmn_DbusSubscription {
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

class Dmn_DbusCall {
public:
  using Arguments = std::vector<Dmn_DbusValue>;
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
 * @brief Generic client for method, property, and signal traffic on one bus.
 *
 * Method replies are dispatched on the client-owned I/O worker, while signal
 * callbacks execute serially on a separate client-owned worker. Do not destroy
 * the client from a signal callback; concurrent destruction with public calls
 * is also unsupported.
 */
class Dmn_DbusClient {
public:
  using Arguments = std::vector<Dmn_DbusValue>;
  using SignalCallback = std::function<void(Arguments)>;
  using SignalCallbackWithPath = std::function<void(std::string, Arguments)>;

  /** @brief Connect to the explicitly selected bus without fallback. */
  explicit Dmn_DbusClient(const Dmn_DbusClientConfig &config = {});

  /** @brief Stop the client and release the private bus connection. */
  ~Dmn_DbusClient();

  /** @brief Stop admission and complete outstanding work; safe to repeat. */
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
   * @param destination Destination bus name.
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
   * @param destination Destination bus name.
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
   * @param destination Well-known or unique destination name.
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
   * @param destination Well-known or unique destination name.
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
   * Both interface and member are required in `match`. The callback runs
   * serially on the client-owned callback worker, not the dispatch thread.
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
