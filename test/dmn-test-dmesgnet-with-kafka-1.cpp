/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that the Dmn_DMesgNet object can be constructed
 * with the external Dmn_Kafka objects as its input and output handler and
 * perform object teardown without problem.
 */

#include <sys/time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "dmn-dmesgnet.hpp"

#include "kafka/dmn-kafka.hpp"

#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  // writer
  Dmn::Dmn_Kafka::ConfigType writeConfigs{};
  writeConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  writeConfigs["sasl.username"] = "ICCN4A57TNKONPQ3";
  writeConfigs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  writeConfigs["security.protocol"] = "SASL_SSL";
  writeConfigs["sasl.mechanisms"] = "PLAIN";
  writeConfigs["acks"] = "all";
  writeConfigs[Dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  writeConfigs[Dmn::Dmn_Kafka::Key] = "Dmn_dmesgnet";

  std::unique_ptr<Dmn::Dmn_Kafka> producer = std::make_unique<Dmn::Dmn_Kafka>(
      Dmn::Dmn_Kafka::Role::Producer, writeConfigs);

  // reader
  Dmn::Dmn_Kafka::ConfigType readConfigs{};
  readConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs["security.protocol"] = "SASL_SSL";
  readConfigs["sasl.mechanisms"] = "PLAIN";
  readConfigs["group.id"] = "dmesg1";
  readConfigs[Dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  readConfigs["auto.offset.reset"] = "latest";
  readConfigs[Dmn::Dmn_Kafka::PollTimeoutMs] = "1000";

  std::unique_ptr<Dmn::Dmn_Kafka> consumer = std::make_unique<Dmn::Dmn_Kafka>(
      Dmn::Dmn_Kafka::Role::Consumer, readConfigs);

  Dmn::Dmn_DMesgNet dmesgnet1{"dmesg1", std::move(consumer),
                              std::move(producer)};
  consumer.reset();
  producer.reset();

  std::this_thread::sleep_for(std::chrono::seconds(5));

  return RUN_ALL_TESTS();
}
