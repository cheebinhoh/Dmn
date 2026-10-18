/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-kafka-example-config.hpp
 * @brief Shared runtime configuration for the Kafka example applications.
 */

#ifndef DMN_KAFKA_EXAMPLE_CONFIG_HPP_
#define DMN_KAFKA_EXAMPLE_CONFIG_HPP_

#include <cstdlib>
#include <expected>
#include <iterator>
#include <string>

#include "kafka/dmn-kafka.hpp"

namespace dmn::app {

/**
 * @brief Load the non-empty broker and SASL settings for the Kafka examples.
 *
 * The credentials are supplied at runtime rather than stored in source.
 * Error messages identify a missing variable but never echo its value.
 */
inline auto
kafkaExampleConfig() -> std::expected<Dmn_Kafka::ConfigType, std::string> {
  constexpr const char *requiredVariables[]{
      "DMN_KAFKA_BOOTSTRAP_SERVERS",
      "DMN_KAFKA_SASL_USERNAME",
      "DMN_KAFKA_SASL_PASSWORD",
  };

  const char *values[std::size(requiredVariables)]{};
  for (std::size_t index = 0; index < std::size(requiredVariables); ++index) {
    values[index] = std::getenv(requiredVariables[index]);
    if (values[index] == nullptr || values[index][0] == '\0') {
      return std::unexpected{std::string{requiredVariables[index]} +
                             " must be set to a non-empty value"};
    }
  }

  Dmn_Kafka::ConfigType config{};
  config["bootstrap.servers"] = values[0];
  config["sasl.username"] = values[1];
  config["sasl.password"] = values[2];
  config["security.protocol"] = "SASL_SSL";
  config["sasl.mechanisms"] = "PLAIN";

  return config;
}

} // namespace dmn::app

#endif // DMN_KAFKA_EXAMPLE_CONFIG_HPP_
