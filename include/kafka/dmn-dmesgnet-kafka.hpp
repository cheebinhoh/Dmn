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
   * @brief The constructor method to initiate a created kafka consumer and
   *        producer from configuration as input and output handles for the
   *        Dmn_DMesgNet object.
   *
   *        The user of the api must provide most of the kafka configuration
   *        besides following "group.id", "auto.offset.reset", "acks",
   *        dmn::Dmn_Kafka::Topic and dmn::Dmn_Kafka::Key which are provided
   *        by Dmn_DMesgNet_Kafka.
   *
   * @param name    The name for Dmn_DMesgNet and kafka group id
   * @param configs The Dmn_Kafka configuration
   */
  Dmn_DMesgNet_Kafka(std::string_view name, Dmn_Kafka::ConfigType configs);
  ~Dmn_DMesgNet_Kafka() noexcept;

  Dmn_DMesgNet_Kafka(const Dmn_DMesgNet_Kafka &obj) = delete;
  const Dmn_DMesgNet_Kafka &operator=(const Dmn_DMesgNet_Kafka &obj) = delete;
  Dmn_DMesgNet_Kafka(Dmn_DMesgNet_Kafka &&obj) = delete;
  Dmn_DMesgNet_Kafka &operator=(Dmn_DMesgNet_Kafka &&obj) = delete;

  /**
   * @brief This method is a forwarding call to the Dmn_DMesgNet::openHandler().
   *
   * @param topics          The list of topics to be subscribed for the opened
   *                        handler
   * @param name            The name or unique identification to the handler
   * @param includeDMesgSys True if the handler will be notified of DMesgPb
   *                        sys message, default is false, which is optional.
   * @param filterFn        The functor callback that returns false to filter
   *                        out DMesgPB message, if no functor is provided,
   *                        no filter is performed
   * @param asyncProcessFn  The functor callback to process each notified
   *                        DMesgPb message
   *
   * @return newly created handler
   */
  template <class... U>
  auto openHandler(U &&...arg) -> std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> {
    return m_dmesgnet->openHandler(std::forward<U>(arg)...);
  }

  /**
   * @brief This method is a forwarding call to the
   *        Dmn_DMesgNet::closeHandler().
   *
   * @param handlerToClose The handler to be closed
   */
  template <class... U> void closeHandler(U &&...arg) {
    m_dmesgnet->closeHandler(std::forward<U>(arg)...);
  }

private:
  /**
   * data members for constructor to instantiate the object.
   */
  std::string m_name{};

  /**
   * data members for internal logic.
   */
  std::unique_ptr<Dmn_DMesgNet> m_dmesgnet{};
}; // class Dmn_DMesgNet_Kafka

} // namespace dmn

#endif // DMN_DMESGNET_KAFKA_HPP_
