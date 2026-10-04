/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-io.hpp
 * @brief Byte-oriented D-Bus signal input and output adapters.
 *
 * The endpoints carry arbitrary byte payloads in the configured broadcast
 * signal. The default signal tuple is the wire contract used by
 * Dmn_DMesgNet; other protocols should choose their own tuple. Each endpoint
 * owns a private connection and worker.
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
 * The dispatch callback admits valid payloads to a bounded queue. Explicit
 * shutdown drains queued payloads and then makes read() throw
 * operation_canceled. This class is thread-safe; concurrent destruction with
 * active method calls is not supported.
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
 * Successful writes mean local queue admission only. They do not acknowledge
 * daemon transmission or subscriber delivery.
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
