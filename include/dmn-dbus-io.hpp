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
 * Data flow
 * ---------
 * Dmn_DbusOutput::write() admits a byte payload to a bounded local queue and
 * wakes its output worker. The worker encodes the payload as one D-Bus byte
 * array signal using the configured path/interface/member tuple. It submits
 * queued signals to libdbus only while the connection's outgoing queue is
 * below the configured byte threshold, then calls the read/write/dispatch API
 * to progress pending transport I/O and observe connection failure. Thus the
 * worker does more than dequeue and send: it also pumps and monitors the
 * libdbus connection while idle. A successful write reports local queue
 * admission only; neither libdbus submission nor signal routing confirms
 * receipt by a subscriber.
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

#include <memory>
#include <optional>
#include <string>

namespace dmn {

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

  /** @brief Stop dispatch and wake readers; safe to call repeatedly. */
  void shutdown() noexcept override;

  /** @brief Return a thread-safe snapshot of input counters and state. */
  [[nodiscard]] auto status() const -> Dmn_DbusIoStatus;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

/**
 * @brief Bounded output endpoint for the configured D-Bus byte signal.
 *
 * The endpoint emits broadcast signals on its configured bus daemon. The bus
 * routes each signal to connections whose installed matches accept it.
 * write() validates and admits the payload to a bounded local FIFO; its worker
 * turns each dequeued payload into an `ay` signal and submits it to libdbus.
 * The worker also pumps connection I/O and monitors disconnects, and it
 * applies backpressure based on libdbus's pending outgoing bytes. Successful
 * writes mean local queue admission only; they do not acknowledge libdbus
 * submission, daemon routing, or subscriber receipt.
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
   * @brief Reject reads because this endpoint is output-only.
   *
   * @return No value; this method always throws operation_not_supported.
   */
  auto read() -> std::optional<std::string> override;

  /**
   * @brief Enqueue a payload, or throw if invalid, full, shut down, or failed.
   *
   * @param item Serialized payload copied into the application queue.
   */
  void write(const std::string &item) override;

  /**
   * @brief Enqueue a payload, moving its storage when possible.
   *
   * @param item Serialized payload moved into the application queue.
   */
  void write(std::string &&item) override;

  /** @brief Stop admission and drain the application queue for a bounded time.
   */
  void shutdown() noexcept override;

  /** @brief Return a thread-safe snapshot of output counters and state. */
  [[nodiscard]] auto status() const -> Dmn_DbusIoStatus;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace dmn

#endif // DMN_DBUS_IO_HPP_
