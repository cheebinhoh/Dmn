/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that the DMesgNet object can integrate with
 * the external Kafka objects that DmesgPb message sent by DMesgNet
 * object through outbound handler of Kafka object (as producer) can be
 * consumed by external kafka object serves as consumer of the same topic
 * published through outbound handler of kafka object (within DMesgNet
 * object).
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

  // reader
  dmn::Kafka::ConfigType readConfigs_other{};
  readConfigs_other["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs_other["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs_other["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs_other["security.protocol"] = "SASL_SSL";
  readConfigs_other["sasl.mechanisms"] = "PLAIN";
  readConfigs_other["group.id"] = "dmesg_other";
  readConfigs_other["auto.offset.reset"] = "latest";
  readConfigs_other[dmn::Kafka::Topic] = "dmesgnet";
  readConfigs_other[dmn::Kafka::PollTimeoutMs] = "7000";

  dmn::Kafka consumer_other{dmn::Kafka::Role::kConsumer, readConfigs_other};

  // writer for DMesgNet
  dmn::Kafka::ConfigType writeConfigs{};
  writeConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  writeConfigs["sasl.username"] = "ICCN4A57TNKONPQ3";
  writeConfigs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  writeConfigs["security.protocol"] = "SASL_SSL";
  writeConfigs["sasl.mechanisms"] = "PLAIN";
  writeConfigs["acks"] = "all";
  writeConfigs[dmn::Kafka::Topic] = "dmesgnet";
  writeConfigs[dmn::Kafka::Key] = "dmesgnet";

  std::unique_ptr<dmn::Kafka> producer =
      std::make_unique<dmn::Kafka>(dmn::Kafka::Role::kProducer, writeConfigs);

  // reader for DMesgNet
  dmn::Kafka::ConfigType readConfigs{};
  readConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs["security.protocol"] = "SASL_SSL";
  readConfigs["sasl.mechanisms"] = "PLAIN";
  readConfigs["group.id"] = "dmesg1";
  readConfigs[dmn::Kafka::Topic] = "dmesgnet";
  readConfigs["auto.offset.reset"] = "latest";
  readConfigs[dmn::Kafka::PollTimeoutMs] = "7000";

  std::unique_ptr<dmn::Kafka> consumer =
      std::make_unique<dmn::Kafka>(dmn::Kafka::Role::kConsumer, readConfigs);

  dmn::DMesgNet dmesgnet1{"dmesg1", std::move(consumer), std::move(producer)};
  consumer.reset();
  producer.reset();

  std::this_thread::sleep_for(std::chrono::seconds(3));

  // consume prior messages from topic.
  dmn::DMesgPb dmesgPbRead{};
  while (true) {
    auto dataRead = consumer_other.read();
    if (dataRead) {
      dmesgPbRead.ParseFromString(*dataRead);

      if (dmesgPbRead.body().sys().self().state() == dmn::DMesgStatePb::Ready) {
        break;
      }
    }
  }

  std::cout << "DebugPrint: " << dmesgPbRead.ShortDebugString() << "\n";

  return RUN_ALL_TESTS();
}
