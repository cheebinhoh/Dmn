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

DMesgNet_Kafka::DMesgNet_Kafka(std::string_view name, Kafka::ConfigType configs)
    : m_name{name} {
  // input handle for DMesgNet
  dmn::Kafka::ConfigType input_configs{configs};
  input_configs["group.id"] = name;
  input_configs[dmn::Kafka::Topic] = "dmesgnet";
  input_configs["auto.offset.reset"] = "earliest";
  input_configs[dmn::Kafka::PollTimeoutMs] = "500";

  std::unique_ptr<dmn::Kafka> input =
      std::make_unique<dmn::Kafka>(dmn::Kafka::Role::kConsumer, input_configs);

  // output handle for DMesgNet
  dmn::Kafka::ConfigType output_configs{configs};
  output_configs["acks"] = "all";
  output_configs[dmn::Kafka::Topic] = "dmesgnet";
  output_configs[dmn::Kafka::Key] = "dmesgnet";

  std::unique_ptr<dmn::Kafka> output =
      std::make_unique<dmn::Kafka>(dmn::Kafka::Role::kProducer, output_configs);

  // DMesgNet
  m_dmesgnet = std::make_unique<dmn::DMesgNet>(name, std::move(input),
                                               std::move(output));
}

DMesgNet_Kafka::~DMesgNet_Kafka() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

} // namespace dmn
