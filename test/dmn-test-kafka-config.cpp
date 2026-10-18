/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-kafka-config.cpp
 * @brief Broker-independent tests for Dmn_Kafka configuration errors.
 */

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "kafka/dmn-kafka.hpp"

TEST(DmnKafkaConfigTest, DoesNotEchoRejectedConfigurationValue) {
  constexpr std::string_view secret_canary{"private-config-canary"};
  dmn::Dmn_Kafka::ConfigType configs{};
  configs["invalid.configuration.property"] = std::string{secret_canary};

  try {
    dmn::Dmn_Kafka producer{dmn::Dmn_Kafka::Role::kProducer,
                            std::move(configs)};
    FAIL() << "Expected invalid librdkafka configuration to be rejected";
  } catch (const std::runtime_error &error) {
    EXPECT_EQ(std::string{error.what()}.find(std::string{secret_canary}),
              std::string::npos);
  }
}
