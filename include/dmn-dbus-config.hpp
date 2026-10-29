/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-config.hpp
 * @brief Shared configuration for optional D-Bus signal I/O.
 */

#ifndef DMN_DBUS_CONFIG_HPP_
#define DMN_DBUS_CONFIG_HPP_

#include <cstddef>
#include <string>

namespace dmn {

/**
 * @brief Configuration shared by the D-Bus input and output endpoints.
 *
 * Endpoints carry byte arrays in D-Bus signals. The default routing tuple is
 * the Dmn_DMesgNet wire contract; callers can select another tuple for
 * unrelated byte-oriented protocols.
 *
 * The bus daemon must already be running. An empty address selects the
 * standard session bus. A non-empty address selects that exact bus daemon; it
 * may be the standard system bus, an explicitly addressed session bus, or a
 * separately started private bus instance. "Private bus" means a separate
 * daemon instance; it is distinct from the private client connection that
 * each endpoint opens to any selected bus. This configuration neither starts
 * a daemon nor exports a Dmn D-Bus service, and connection failure never
 * falls back to another bus.
 */
struct Dmn_DbusConfig {
  /**
   * @brief Bus daemon address; empty selects the standard session bus.
   *
   * A non-empty value is used exactly as supplied and must identify a running
   * bus daemon. It can address the system bus, an explicitly addressed
   * session bus, or a separately started private bus instance. It
   * selects a bus, not a Dmn service. Each input/output endpoint opens its own
   * dedicated private connection to that bus.
   */
  std::string bus_address{};

  /**
   * @brief Signal object path; defaults to the Dmn_DMesgNet wire path.
   */
  std::string signal_path{"/org/dmn/DMesg1"};

  /**
   * @brief Signal interface; defaults to the Dmn_DMesgNet wire interface.
   */
  std::string signal_interface{"org.dmn.DMesg1.Transport"};

  /**
   * @brief Signal member; defaults to the Dmn_DMesgNet wire member.
   */
  std::string signal_member{"Message"};

  /**
   * @brief Maximum accepted payload size.
   */
  std::size_t max_message_bytes{1024 * 1024};

  /**
   * @brief Maximum items retained by either application queue.
   */
  std::size_t max_queued_messages{1024};

  /**
   * @brief Maximum payload bytes retained by either application queue.
   */
  std::size_t max_queued_bytes{16 * 1024 * 1024};
};

} // namespace dmn

#endif // DMN_DBUS_CONFIG_HPP_
