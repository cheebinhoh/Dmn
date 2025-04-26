/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
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

class DMesgNet_Kafka {
public:
  /**
   * @brief The constructor method to initiate a created kafka consumer and
   *        producer from configuration as input and output handles for the
   *        DMesgNet object.
   *
   *        The user of the api must provide most of the kafka configuration
   *        besides following "group.id", "auto.offset.reset", "acks",
   *        dmn::Kafka::Topic and dmn::Kafka::Key which are provided
   *        by DMesgNet_Kafka.
   *
   * @param name    The name for DMesgNet and kafka group id
   * @param configs The Kafka configuration
   */
  DMesgNet_Kafka(std::string_view name, Kafka::ConfigType configs);
  ~DMesgNet_Kafka() noexcept;

  DMesgNet_Kafka(const DMesgNet_Kafka &obj) = delete;
  const DMesgNet_Kafka &operator=(const DMesgNet_Kafka &obj) = delete;
  DMesgNet_Kafka(DMesgNet_Kafka &&obj) = delete;
  DMesgNet_Kafka &operator=(DMesgNet_Kafka &&obj) = delete;

  /**
   * @brief This method is a forwarding call to the DMesgNet::openHandler().
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
  std::shared_ptr<DMesg::DMesgHandler> openHandler(U &&...arg) {
    return m_dmesgnet->openHandler(std::forward<U>(arg)...);
  }

  /**
   * @brief This method is a forwarding call to the
   *        DMesgNet::closeHandler().
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
  std::unique_ptr<DMesgNet> m_dmesgnet{};
}; // class DMesgNet_Kafka

} // namespace dmn

#endif // DMN_DMESGNET_KAFKA_HPP_
