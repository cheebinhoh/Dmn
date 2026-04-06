/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesgnet-kafka.hpp
 * @brief Convenience wrapper that wires Dmn_DMesgNet to Apache Kafka I/O.
 *
 * Dmn_DMesgNet_Kafka is a thin composition class that instantiates a
 * Dmn_Kafka consumer (input) and a Dmn_Kafka producer (output) and
 * passes them to a Dmn_DMesgNet instance. The result is a
 * network-aware DMesg node whose transport layer is backed by a Kafka
 * broker instead of a raw socket.
 *
 * The class handles all Kafka-specific configuration detail
 * (consumer group id, topic name, auto.offset.reset, acks, etc.) so
 * callers only need to supply the common broker connection parameters
 * (bootstrap.servers, SASL credentials, etc.) via a Dmn_Kafka::ConfigType
 * map. The fixed Kafka topic used for cluster communication is
 * "Dmn_dmesgnet".
 *
 * Handler management (openHandler / closeHandler) is forwarded
 * directly to the underlying Dmn_DMesgNet instance so callers work
 * with the standard Dmn_DMesgHandler API.
 *
 * See also:
 *  - dmn-dmesgnet.hpp : Dmn_DMesgNet — the network-aware DMesg base.
 *  - kafka/dmn-kafka.hpp : Dmn_Kafka — the Kafka Dmn_Io adapter.
 */

#ifndef DMN_DMESGNET_KAFKA_HPP_

#define DMN_DMESGNET_KAFKA_HPP_

#include <memory>
#include <string>
#include <string_view>

#include "dmn-dmesg.hpp"
#include "dmn-dmesgnet.hpp"
#include "dmn-kafka.hpp"

namespace dmn {

class Dmn_DMesgNet_Kafka {
public:
  /**
   * @brief Construct a @c Dmn_DMesgNet_Kafka by creating a Kafka consumer and
   *        producer from @p configs and wiring them as the input and output
   *        handlers of the underlying @c Dmn_DMesgNet.
   *
   * The caller must supply the common broker connection parameters in @p
   * configs (e.g. @c bootstrap.servers, SASL credentials).  The following keys
   * are set internally and must NOT be present in @p configs:
   *  - @c "group.id"         : set to @p name.
   *  - @c "auto.offset.reset": set to @c "earliest".
   *  - @c "acks"             : set to @c "all".
   *  - @c Dmn_Kafka::Topic   : set to @c "Dmn_dmesgnet".
   *  - @c Dmn_Kafka::Key     : set to @c "Dmn_dmesgnet".
   *
   * @param name    Identifier used for both the @c Dmn_DMesgNet instance and
   *                the Kafka consumer group ID.
   * @param configs Kafka configuration entries (see above for reserved keys).
   */
  Dmn_DMesgNet_Kafka(std::string_view name, Dmn_Kafka::ConfigType configs);

  /** @brief Destroy the @c Dmn_DMesgNet_Kafka and release all resources. */
  ~Dmn_DMesgNet_Kafka() noexcept;

  Dmn_DMesgNet_Kafka(const Dmn_DMesgNet_Kafka &obj) = delete;
  Dmn_DMesgNet_Kafka &operator=(const Dmn_DMesgNet_Kafka &obj) = delete;
  Dmn_DMesgNet_Kafka(Dmn_DMesgNet_Kafka &&obj) = delete;
  Dmn_DMesgNet_Kafka &operator=(Dmn_DMesgNet_Kafka &&obj) = delete;

  /**
   * @brief Forward all arguments to @c Dmn_DMesgNet::openHandler() and return
   *        the resulting handler proxy.
   *
   * @param arg Arguments forwarded verbatim to @c Dmn_DMesgNet::openHandler().
   * @return A @c Dmn_DMesg::HandlerType proxy to the newly opened handler.
   */
  template <class... U> auto openHandler(U &&...arg) -> Dmn_DMesg::HandlerType {
    return m_dmesgnet->openHandler(std::forward<U>(arg)...);
  }

  /**
   * @brief Forward all arguments to @c Dmn_DMesgNet::closeHandler() to close
   *        a previously opened handler.
   *
   * @param arg Arguments forwarded verbatim to @c Dmn_DMesgNet::closeHandler().
   */
  template <class... U> void closeHandler(U &&...arg) {
    m_dmesgnet->closeHandler(std::forward<U>(arg)...);
  }

private:
  std::string m_name{}; ///< Instance name (also used as Kafka group ID).
  std::unique_ptr<Dmn_DMesgNet>
      m_dmesgnet{}; ///< Underlying network-aware DMesg node.
}; // class Dmn_DMesgNet_Kafka

} // namespace dmn

#endif // DMN_DMESGNET_KAFKA_HPP_
