/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#ifndef DMN_DMESGNET_KAFKA_HPP_HAVE_SEEN

#define DMN_DMESGNET_KAFKA_HPP_HAVE_SEEN

#include "dmn-dmesg.hpp"
#include "dmn-dmesgnet.hpp"
#include "dmn-kafka.hpp"

#include <memory>
#include <string_view>

namespace Dmn {

class Dmn_DMesgNet_Kafka {
public:
  /**
   * @brief The constructor method to initiate a created kafka consumer and
   *        producer from configs and used them as input and output handles
   *        for the Dmn_DMesgNet objects.
   *
   *        The user of the api must provide most of the kafka configuration
   *        besides following "group.id", "auto.offset.reset", "acks",
   *        Dmn::Dmn_Kafka::Topic and Dmn::Dmn_Kafka::Key which are provided
   *        by Dmn_DMesgNet_Kafka.
   *
   * @param name    The name for Dmn_DMesgNet and kafka group id for consumer
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
  std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> openHandler(U &&...arg) {
    return m_dmesgNet->openHandler(std::forward<U>(arg)...);
  }

  /**
   * @brief This method is a forwarding call to the Dmn_DMesgNet::closeHandler().
   *
   * @param handlerToClose The handler to be closed
   */
  template <class... U>
  void closeHandler(U &&...arg) {
    m_dmesgNet->closeHandler(std::forward<U>(arg)...);
  }

private:
  /**
   * data members for constructor to instantiate the object.
   */
  std::string m_name{};

  /**
   * data members for internal logic.
   */
  std::unique_ptr<Dmn_DMesgNet> m_dmesgNet{};
};

} /* End of namespace Dmn */

#endif /* End of macro DMN_DMESGNET_KAFKA_HPP_HAVE_SEEN */
