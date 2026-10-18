/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-kafka-receiver.cpp
 * @brief Application entry point for the Kafka consumer example.
 *
 * This program creates a Dmn_Kafka consumer using broker credentials from
 * the DMN_KAFKA_BOOTSTRAP_SERVERS, DMN_KAFKA_SASL_USERNAME, and
 * DMN_KAFKA_SASL_PASSWORD environment variables and polls the
 * "timer_counter" topic until SIGINT.
 *
 * Original reference:
 *   https://developer.confluent.io/get-started/c/
 */

#include "kafka/dmn-kafka.hpp"

#include <cstdlib>
#include <iostream>
#include <signal.h>
#include <utility>

#include "dmn-kafka-example-config.hpp"

static volatile sig_atomic_t run = 1;

/**
 * @brief Signal termination of program
 */
static void stop([[maybe_unused]] int signal) { run = 0; }

int main() {
  auto config = dmn::app::kafkaExampleConfig();
  if (!config) {
    std::cerr << "Kafka example configuration error: " << config.error()
              << '\n';

    return EXIT_FAILURE;
  }

  auto read_configs = std::move(*config);
  read_configs["group.id"] = "dmn-kafka-receiver";
  read_configs[dmn::Dmn_Kafka::Topic] = "timer_counter";
  read_configs["auto.offset.reset"] = "earliest";

  dmn::Dmn_Kafka consumer{dmn::Dmn_Kafka::Role::kConsumer, read_configs};

  // Install a signal handler for clean shutdown.
  signal(SIGINT, stop);

  // Start polling for messages.
  while (run) {
    auto data = consumer.read();

    if (data) {
      std::cout << "Consumed message: " << (*data) << "\n";
    }
  }

  return 0;
}
