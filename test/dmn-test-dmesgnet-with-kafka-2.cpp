/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that the Dmn_DMesgNet object can integrate with
 * the external Dmn_Kafka objects that Dmn_DmesgPb message sent by Dmn_DMesgNet
 * object through outbound handler of Dmn_Kafka object (as producer) can be
 * consumed by external Dmn_kafka object serves as consumer of the same topic
 * published through outbound handler of Dmn_kafka object (within Dmn_DMesgNet
 * object).
 */

#include "dmn-dmesgnet.hpp"
#include "kafka/dmn-kafka.hpp"

#include "proto/dmn-dmesg.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <sys/time.h>
#include <thread>

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  // reader
  Dmn::Dmn_Kafka::ConfigType readConfigs_other{};
  readConfigs_other["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs_other["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs_other["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs_other["security.protocol"] = "SASL_SSL";
  readConfigs_other["sasl.mechanisms"] = "PLAIN";
  readConfigs_other["group.id"] = "dmesg_other";
  readConfigs_other["auto.offset.reset"] = "latest";
  readConfigs_other[Dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  readConfigs_other[Dmn::Dmn_Kafka::PollTimeoutMs] = "7000";

  Dmn::Dmn_Kafka consumer_other{Dmn::Dmn_Kafka::Role::Consumer,
                                readConfigs_other};

  // writer for DMesgNet
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

  // reader for DMesgNet
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
  readConfigs[Dmn::Dmn_Kafka::PollTimeoutMs] = "7000";

  std::unique_ptr<Dmn::Dmn_Kafka> consumer = std::make_unique<Dmn::Dmn_Kafka>(
      Dmn::Dmn_Kafka::Role::Consumer, readConfigs);

  Dmn::Dmn_DMesgNet dmesgnet1{"dmesg1", std::move(consumer),
                              std::move(producer)};
  std::this_thread::sleep_for(std::chrono::seconds(3));
  producer = {};
  consumer = {};

  // consume prior messages from topic.
  Dmn::DMesgPb dmesgPbRead{};
  while (true) {
    auto dataRead = consumer_other.read();
    if (dataRead) {
      dmesgPbRead.ParseFromString(*dataRead);

      if (dmesgPbRead.body().sys().self().state() == Dmn::DMesgStatePb::Ready) {
        break;
      }
    }
  }

  std::cout << "DebugPrint: " << dmesgPbRead.ShortDebugString() << "\n";

  return RUN_ALL_TESTS();
}
