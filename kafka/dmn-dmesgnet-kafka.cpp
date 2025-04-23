/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include "dmn-dmesgnet-kafka.hpp"

#include <memory>
#include <string_view>

#include "dmn-dmesg.hpp"
#include "dmn-kafka-util.hpp"
#include "dmn-kafka.hpp"

namespace Dmn {

Dmn_DMesgNet_Kafka::Dmn_DMesgNet_Kafka(std::string_view name,
                                       Dmn_Kafka::ConfigType configs)
    : m_name{name} {
  // input handle for DMesgNet
  Dmn::Dmn_Kafka::ConfigType inputConfigs{configs};
  inputConfigs["group.id"] = name;
  inputConfigs[Dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  inputConfigs["auto.offset.reset"] = "earliest";
  inputConfigs[Dmn::Dmn_Kafka::PollTimeoutMs] = "500";

  std::unique_ptr<Dmn::Dmn_Kafka> input = std::make_unique<Dmn::Dmn_Kafka>(
      Dmn::Dmn_Kafka::Role::Consumer, inputConfigs);

  // output handle for DMesgNet
  Dmn::Dmn_Kafka::ConfigType outputConfigs{configs};
  outputConfigs["acks"] = "all";
  outputConfigs[Dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  outputConfigs[Dmn::Dmn_Kafka::Key] = "Dmn_dmesgnet";

  std::unique_ptr<Dmn::Dmn_Kafka> output = std::make_unique<Dmn::Dmn_Kafka>(
      Dmn::Dmn_Kafka::Role::Producer, outputConfigs);

  // DMesgNet
  m_dmesgNet = std::make_unique<Dmn::Dmn_DMesgNet>(name, std::move(input),
                                                   std::move(output));
}

Dmn_DMesgNet_Kafka::~Dmn_DMesgNet_Kafka() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

} // namespace Dmn
