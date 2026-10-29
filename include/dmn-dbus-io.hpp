/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-io.hpp
 * @brief Byte-oriented D-Bus signal input and output adapters.
 *
 * The endpoints carry arbitrary byte payloads in a configured broadcast
 * signal. The default signal tuple is the wire contract used by
 * Dmn_DMesgNet; other protocols should choose their own tuple. Each endpoint
 * owns a dedicated libdbus private connection and worker. The selected bus
 * may be the standard session bus, the system bus, or a separately started
 * private bus instance (selected by address). The bus daemon routes signals
 * between matching participants; this is not a direct peer-to-peer D-Bus
 * connection. These endpoints neither create a bus daemon nor export or call
 * a Dmn D-Bus service.
 *
 * Direction
 * ---------
 * Both classes derive from Dmn_Io<std::string> but are unidirectional:
 * Dmn_DbusInput supports only read() and Dmn_DbusOutput only write(). The
 * unsupported method of each is private, so calling it on the concrete type
 * fails to compile; if reached through a Dmn_Io<std::string> reference it
 * still throws std::system_error with std::errc::operation_not_supported.
 *
 * Data flow
 * ---------
 * Dmn_DbusOutput::write() is a blocking call, like the other Dmn_Io writers:
 * it admits a byte payload to a bounded local queue and wakes its output
 * worker. It waits only when the queue is full (or earlier writers are
 * already blocked); with space available it returns at once. A blocked writer
 * takes a ticket and is admitted in ticket (arrival) order as the worker frees
 * space, while shutdown or failure wakes every blocked writer. The worker
 * encodes the payload as one D-Bus byte array signal using the configured
 * path/interface/member tuple. It submits queued signals to libdbus only while
 * the connection's outgoing queue is below the configured byte threshold, then
 * calls the read/write/dispatch API to progress pending transport I/O and
 * observe connection failure. Thus the worker does more than dequeue and send:
 * it also pumps and monitors the libdbus connection while idle. A successful
 * write reports local queue admission only; neither libdbus submission nor
 * signal routing confirms receipt by a subscriber.
 *
 * Dmn_DbusInput's worker dispatches its connection and receives signals through
 * an installed daemon-side match. The input filter accepts only the configured
 * signal tuple with a byte-array body, checks payload and queue limits, copies
 * the bytes into a bounded local queue, and wakes a thread blocked in read().
 * read() returns the next queued payload; it does not read directly from the
 * D-Bus connection. Both endpoints own separate connections and workers.
 */

#ifndef DMN_DBUS_IO_HPP_
#define DMN_DBUS_IO_HPP_

#include "dmn-dbus-config.hpp"
#include "dmn-io.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace dmn {

/**
 * @brief Thread-safe snapshot of endpoint counters and queue state.
 *
 * Counters describe local endpoint activity only; none indicate peer delivery.
 */
struct Dmn_DbusIoStatus {
  std::uint64_t messages_received{};
  std::uint64_t messages_written{};
  std::uint64_t messages_queued_to_libdbus{};
  std::uint64_t malformed_received{};
  std::uint64_t oversized_received{};
  std::uint64_t input_allocation_errors{};
  std::uint64_t input_queue_drops{};
  std::size_t pending_input_messages{};
  std::size_t pending_input_bytes{};
  std::uint64_t output_write_waits{}; ///< Writes that blocked on a full queue.
  std::uint64_t output_worker_errors{};
  std::size_t pending_output_messages{};
  std::size_t pending_output_bytes{};
  std::size_t libdbus_outgoing_bytes{};
  std::uint64_t shutdown_unsent_messages{};
  std::size_t shutdown_unsent_bytes{};
  std::size_t libdbus_bytes_discarded_on_shutdown{};
  std::error_code terminal_error{};
};

/**
 * @brief Blocking input endpoint for the configured D-Bus byte signal.
 *
 * The endpoint installs a signal match on its configured bus daemon; its
 * dispatch worker filters incoming messages for the configured signal tuple.
 * Valid `ay` payloads within size and queue limits are copied to a
 * bounded local FIFO and wake blocked readers; read() returns from that FIFO,
 * not directly from libdbus. It does not subscribe to a direct connection
 * with a sender process. Explicit shutdown preserves already queued payloads
 * for reads; after they are consumed, read() throws operation_canceled. This
 * class is thread-safe; concurrent destruction with active method calls is
 * not supported.
 *
 * Input-only: write() is private and throws operation_not_supported when
 * reached through the Dmn_Io<std::string> interface.
 */
class Dmn_DbusInput final : public Dmn_Io<std::string> {
public:
  /**
   * @brief Connect to the configured bus and install the transport match.
   *
   * @param config Bus address and finite payload/queue limits.
   */
  explicit Dmn_DbusInput(const Dmn_DbusConfig &config);
  ~Dmn_DbusInput() noexcept override;

  Dmn_DbusInput(const Dmn_DbusInput &) = delete;
  auto operator=(const Dmn_DbusInput &) -> Dmn_DbusInput & = delete;
  Dmn_DbusInput(Dmn_DbusInput &&) = delete;
  auto operator=(Dmn_DbusInput &&) -> Dmn_DbusInput & = delete;

  /**
   * @brief Read the next queued payload, blocking until data or termination.
   *
   * @throws std::system_error after explicit shutdown or terminal bus failure.
   */
  auto read() -> std::optional<std::string> override;

  /**
   * @brief Stop dispatch and wake readers; safe to call repeatedly.
   */
  void shutdown() noexcept override;

  /**
   * @brief Return a thread-safe snapshot of input counters and state.
   */
  [[nodiscard]] auto status() const -> Dmn_DbusIoStatus;

private:
  // Unsupported direction: private so concrete-typed callers fail to
  // compile; calls through Dmn_Io<std::string> still throw
  // std::errc::operation_not_supported.
  /**
   * @brief Reject writes because this endpoint is input-only.
   *
   * @param item Unused input value.
   */
  void write(const std::string &item) override;

  /**
   * @brief Reject writes because this endpoint is input-only.
   *
   * @param item Unused input value.
   */
  void write(std::string &&item) override;

  class Impl;
  std::unique_ptr<Impl> m_impl;
};

/**
 * @brief Bounded output endpoint for the configured D-Bus byte signal.
 *
 * The endpoint emits broadcast signals on its configured bus daemon. The bus
 * routes each signal to connections whose installed matches accept it.
 * write() validates the payload and admits it to a bounded local FIFO,
 * blocking only while the FIFO is full, in ticket (arrival) order, until the
 * worker drains space (or the endpoint shuts down or fails); its worker
 * turns each dequeued payload into an `ay` signal and submits it to libdbus.
 * The worker also pumps connection I/O and monitors disconnects, and it
 * applies backpressure based on libdbus's pending outgoing bytes. Successful
 * writes mean local queue admission only; they do not acknowledge libdbus
 * submission, daemon routing, or subscriber receipt.
 *
 * Output-only: read() is private and throws operation_not_supported when
 * reached through the Dmn_Io<std::string> interface.
 */
class Dmn_DbusOutput final : public Dmn_Io<std::string> {
public:
  /**
   * @brief Connect to the configured bus and start the output worker.
   *
   * @param config Bus address and finite payload/queue limits.
   */
  explicit Dmn_DbusOutput(const Dmn_DbusConfig &config);
  ~Dmn_DbusOutput() noexcept override;

  Dmn_DbusOutput(const Dmn_DbusOutput &) = delete;
  auto operator=(const Dmn_DbusOutput &) -> Dmn_DbusOutput & = delete;
  Dmn_DbusOutput(Dmn_DbusOutput &&) = delete;
  auto operator=(Dmn_DbusOutput &&) -> Dmn_DbusOutput & = delete;

  /**
   * @brief Enqueue a payload, blocking only while the queue is full.
   *
   * With queue space available and no earlier blocked writer, the payload is
   * admitted immediately and the call returns without waiting. Otherwise the
   * writer takes a ticket and blocks; as the worker frees space, blocked
   * writers are admitted strictly in ticket (arrival) order, so a later
   * payload never overtakes an earlier one. Shutdown or worker failure wakes
   * every blocked writer regardless of ticket. Returning means local queue
   * admission only, not libdbus submission or subscriber receipt.
   *
   * @param item Serialized payload copied into the application queue.
   * @throws std::system_error with std::errc::message_size if the payload
   * exceeds the configured message limit.
   * @throws std::system_error with std::errc::operation_canceled if the
   * endpoint is shut down, including while the writer is blocked.
   * @throws std::system_error carrying the terminal error if the worker has
   * failed, including while the writer is blocked.
   */
  void write(const std::string &item) override;

  /**
   * @brief Enqueue a payload, moving its storage when possible.
   *
   * Blocking, ordering, and error behavior are identical to the const
   * overload.
   *
   * @param item Serialized payload moved into the application queue.
   * @throws std::system_error See write(const std::string &).
   */
  void write(std::string &&item) override;

  /**
   * @brief Stop admission and drain the application queue for a bounded time.
   */
  void shutdown() noexcept override;

  /**
   * @brief Return a thread-safe snapshot of output counters and state.
   */
  [[nodiscard]] auto status() const -> Dmn_DbusIoStatus;

private:
  // Unsupported direction: private so concrete-typed callers fail to
  // compile; calls through Dmn_Io<std::string> still throw
  // std::errc::operation_not_supported.
  /**
   * @brief Reject reads because this endpoint is output-only.
   *
   * @return No value; this method always throws operation_not_supported.
   */
  auto read() -> std::optional<std::string> override;

  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_DBUS_IO_HPP_
