/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include "dmn-dmesgnet-kafka.hpp"

#include <memory>
#include <string_view>

#include "dmn-dmesg.hpp"
#include "dmn-kafka-util.hpp"
#include "dmn-kafka.hpp"

namespace dmn {

Dmn_DMesgNet_Kafka::Dmn_DMesgNet_Kafka(std::string_view name,
                                       Dmn_Kafka::ConfigType configs)
    : m_name{name} {
  // input handle for DMesgNet
  dmn::Dmn_Kafka::ConfigType inputConfigs{configs};
  inputConfigs["group.id"] = name;
  inputConfigs[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  inputConfigs["auto.offset.reset"] = "earliest";
  inputConfigs[dmn::Dmn_Kafka::PollTimeoutMs] = "500";

  std::unique_ptr<dmn::Dmn_Kafka> input = std::make_unique<dmn::Dmn_Kafka>(
      dmn::Dmn_Kafka::Role::Consumer, inputConfigs);

  // output handle for DMesgNet
  dmn::Dmn_Kafka::ConfigType outputConfigs{configs};
  outputConfigs["acks"] = "all";
  outputConfigs[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  outputConfigs[dmn::Dmn_Kafka::Key] = "Dmn_dmesgnet";

  std::unique_ptr<dmn::Dmn_Kafka> output = std::make_unique<dmn::Dmn_Kafka>(
      dmn::Dmn_Kafka::Role::Producer, outputConfigs);

  // DMesgNet
  m_dmesgNet = std::make_unique<dmn::Dmn_DMesgNet>(name, std::move(input),
                                                   std::move(output));
}

Dmn_DMesgNet_Kafka::~Dmn_DMesgNet_Kafka() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

} // namespace dmn
