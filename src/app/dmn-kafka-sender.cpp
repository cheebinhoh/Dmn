/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-kafka-sender.cpp
 * @brief Application entry point for the Kafka producer example.
 *
 * This program creates a Dmn_Kafka producer using broker credentials from
 * the DMN_KAFKA_BOOTSTRAP_SERVERS, DMN_KAFKA_SASL_USERNAME, and
 * DMN_KAFKA_SASL_PASSWORD environment variables, then publishes ten
 * numbered messages to the "timer_counter" topic.
 *
 * Original reference:
 *   https://developer.confluent.io/get-started/c/
 */

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

#include "dmn-kafka-example-config.hpp"

int main() {
  auto config = dmn::app::kafkaExampleConfig();
  if (!config) {
    std::cerr << "Kafka example configuration error: " << config.error()
              << '\n';
    return EXIT_FAILURE;
  }

  auto write_configs = std::move(*config);
  write_configs["acks"] = "all";
  write_configs[dmn::Dmn_Kafka::Topic] = "timer_counter";
  write_configs[dmn::Dmn_Kafka::Key] = "tick";

  dmn::Dmn_Kafka producer{dmn::Dmn_Kafka::Role::kProducer, write_configs};

  for (int i = 0; i < 10; i++) {
    std::stringstream os{};

    os << "heartbeat: " << i;

    producer.write(os.str());
  }

  return 0;
}
