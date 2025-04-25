/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that we can have two Dmn_DMesgNet objects with
 * external Dmn_Kafka objects as its input and output handlers and join in
 * a virtual distrbuted messaging network that spans cross a confluent.cloud
 * via Dmn_Kafka I/O and rdkafka, aka the primitive form of Dmn_DMesgNet_Kafka.
 *
 * Each Dmn_DMesgNet_Kafka object has a sys state' nodelist that includes
 * another object identifier as its neighbor.
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
  dmn::Dmn_Kafka::ConfigType readConfigs_other{};
  readConfigs_other["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs_other["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs_other["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs_other["security.protocol"] = "SASL_SSL";
  readConfigs_other["sasl.mechanisms"] = "PLAIN";
  readConfigs_other["group.id"] = "dmesg_other";
  readConfigs_other["auto.offset.reset"] = "earliest";
  readConfigs_other[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  readConfigs_other[dmn::Dmn_Kafka::PollTimeoutMs] = "7000";

  dmn::Dmn_Kafka consumer_other{dmn::Dmn_Kafka::Role::kConsumer,
                                readConfigs_other};

  // dmesgnet1
  // writer for DMesgNet
  dmn::Dmn_Kafka::ConfigType writeConfigs1{};
  writeConfigs1["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  writeConfigs1["sasl.username"] = "ICCN4A57TNKONPQ3";
  writeConfigs1["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  writeConfigs1["security.protocol"] = "SASL_SSL";
  writeConfigs1["sasl.mechanisms"] = "PLAIN";
  writeConfigs1["acks"] = "all";
  writeConfigs1[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  writeConfigs1[dmn::Dmn_Kafka::Key] = "Dmn_dmesgnet";

  std::unique_ptr<dmn::Dmn_Kafka> producer1 = std::make_unique<dmn::Dmn_Kafka>(
      dmn::Dmn_Kafka::Role::kProducer, writeConfigs1);

  // reader for DMesgNet
  dmn::Dmn_Kafka::ConfigType readConfigs1{};
  readConfigs1["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs1["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs1["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs1["security.protocol"] = "SASL_SSL";
  readConfigs1["sasl.mechanisms"] = "PLAIN";
  readConfigs1["group.id"] = "dmesg1";
  readConfigs1[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  readConfigs1["auto.offset.reset"] = "earliest";
  readConfigs1[dmn::Dmn_Kafka::PollTimeoutMs] = "7000";

  std::unique_ptr<dmn::Dmn_Kafka> consumer1 = std::make_unique<dmn::Dmn_Kafka>(
      dmn::Dmn_Kafka::Role::kConsumer, readConfigs1);

  // dmesgnet2
  // writer for DMesgNet
  dmn::Dmn_Kafka::ConfigType writeConfigs2{};
  writeConfigs2["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  writeConfigs2["sasl.username"] = "ICCN4A57TNKONPQ3";
  writeConfigs2["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  writeConfigs2["security.protocol"] = "SASL_SSL";
  writeConfigs2["sasl.mechanisms"] = "PLAIN";
  writeConfigs2["acks"] = "all";
  writeConfigs2[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  writeConfigs2[dmn::Dmn_Kafka::Key] = "Dmn_dmesgnet";

  std::unique_ptr<dmn::Dmn_Kafka> producer2 = std::make_unique<dmn::Dmn_Kafka>(
      dmn::Dmn_Kafka::Role::kProducer, writeConfigs2);

  // reader for DMesgNet
  dmn::Dmn_Kafka::ConfigType readConfigs2{};
  readConfigs2["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs2["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs2["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs2["security.protocol"] = "SASL_SSL";
  readConfigs2["sasl.mechanisms"] = "PLAIN";
  readConfigs2["group.id"] = "dmesg2";
  readConfigs2[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  readConfigs2["auto.offset.reset"] = "earliest";
  readConfigs2[dmn::Dmn_Kafka::PollTimeoutMs] = "7000";

  std::unique_ptr<dmn::Dmn_Kafka> consumer2 = std::make_unique<dmn::Dmn_Kafka>(
      dmn::Dmn_Kafka::Role::kConsumer, readConfigs2);

  // dmesgnet1
  dmn::Dmn_DMesgNet dmesgnet1{"dmesg1", std::move(consumer1),
                              std::move(producer1)};
  producer1.reset();
  consumer1.reset();

  // dmesgnet2
  dmn::Dmn_DMesgNet dmesgnet2{"dmesg2", std::move(consumer2),
                              std::move(producer2)};
  producer2.reset();
  consumer2.reset();

  std::this_thread::sleep_for(std::chrono::seconds(5));

  // consume prior messages from topic.
  dmn::DMesgPb dmesgPbRead{};
  std::map<std::string, std::string> nodeList{};
  std::map<std::string, std::string> masterList{};
  int n{};
  while (n < 10000) {
    auto dataRead = consumer_other.read();
    if (dataRead) {
      dmesgPbRead.ParseFromString(*dataRead);

      int i = 0;
      while (i < dmesgPbRead.body().sys().nodelist().size()) {
        auto id = dmesgPbRead.body().sys().nodelist().Get(i).identifier();
        nodeList[dmesgPbRead.body().sys().self().identifier()] = id;
        i++;
      }

      EXPECT_TRUE(i <= 1);

      masterList[dmesgPbRead.body().sys().self().identifier()] =
          dmesgPbRead.body().sys().self().masteridentifier();

      if (nodeList.size() == 2) {
        std::string master{};
        bool ok{true};

        for (auto &mp : masterList) {
          if (master != "") {
            if (master != mp.second) {
              ok = false;
              break;
            }
          }

          master = mp.second;
        }

        if (ok) {
          std::cout << "all checked\n";
          break;
        }
      }
    }

    n++;
  }

  EXPECT_TRUE(n < 10000);

  return RUN_ALL_TESTS();
}
