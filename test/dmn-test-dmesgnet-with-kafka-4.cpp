/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that we can have two DMesgNet_Kafka object(s)
 * that joins in a virtual distrbuted messaging network that spans cross a
 * confluent.cloud via Kafka I/O and rdkafka.
 *
 * Each DMesgNet_Kafka object has a sys state' nodelist that includes
 * another object identifier as its neighbor.
 */

#include <sys/time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "kafka/dmn-dmesgnet-kafka.hpp"

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
  readConfigs_other["auto.offset.reset"] = "earliest";
  readConfigs_other[dmn::Kafka::Topic] = "dmesgnet";
  readConfigs_other[dmn::Kafka::PollTimeoutMs] = "7000";

  dmn::Kafka consumer_other{dmn::Kafka::Role::kConsumer, readConfigs_other};

  // dmesgnet1
  // writer for DMesgNet
  dmn::Kafka::ConfigType configs{};
  configs["bootstrap.servers"] = "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  configs["sasl.username"] = "ICCN4A57TNKONPQ3";
  configs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  configs["security.protocol"] = "SASL_SSL";
  configs["sasl.mechanisms"] = "PLAIN";

  // dmesgnet1
  dmn::DMesgNet_Kafka dmesgnet1{"dmesg1", configs};

  // dmesgnet2
  dmn::DMesgNet_Kafka dmesgnet2{"dmesg2", configs};

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
