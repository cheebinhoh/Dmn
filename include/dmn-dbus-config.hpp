/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-config.hpp
 * @brief Shared configuration and status types for optional D-Bus signal I/O.
 */

#ifndef DMN_DBUS_CONFIG_HPP_
#define DMN_DBUS_CONFIG_HPP_

#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

namespace dmn {

/**
 * @brief Configuration shared by the D-Bus input and output endpoints.
 *
 * Endpoints carry byte arrays in D-Bus signals. The default routing tuple is
 * reserved for Dmn_DMesgNet; callers can select another tuple for unrelated
 * byte-oriented protocols. An empty bus address selects the session bus.
 * Explicit addresses are opened as private bus connections and never fall
 * back to another bus.
 */
struct Dmn_DbusConfig {
  /** @brief Empty selects the session bus; otherwise this exact address is
   * used. */
  std::string bus_address{};
  /** @brief Signal object path; defaults to the Dmn_DMesgNet wire path. */
  std::string signal_path{"/org/dmn/DMesg1"};
  /** @brief Signal interface; defaults to the Dmn_DMesgNet wire interface. */
  std::string signal_interface{"org.dmn.DMesg1.Transport"};
  /** @brief Signal member; defaults to the Dmn_DMesgNet wire member. */
  std::string signal_member{"Message"};
  /** @brief Maximum accepted payload size. */
  std::size_t max_message_bytes{1024 * 1024};
  /** @brief Maximum items retained by either application queue. */
  std::size_t max_queued_messages{1024};
  /** @brief Maximum payload bytes retained by either application queue. */
  std::size_t max_queued_bytes{16 * 1024 * 1024};
};

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
  std::uint64_t output_queue_rejections{};
  std::uint64_t output_worker_errors{};
  std::size_t pending_output_messages{};
  std::size_t pending_output_bytes{};
  std::size_t libdbus_outgoing_bytes{};
  std::uint64_t shutdown_unsent_messages{};
  std::size_t shutdown_unsent_bytes{};
  std::size_t libdbus_bytes_discarded_on_shutdown{};
  std::error_code terminal_error{};
};

} // namespace dmn

#endif // DMN_DBUS_CONFIG_HPP_
