/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that we can have two Dmn_DMesgNet_Kafka object(s)
 * that joins in a virtual distrbuted messaging network that spans cross a
 * confluent.cloud via Dmn_Kafka I/O and rdkafka.
 *
 * Each Dmn_DMesgNet_Kafka object has a sys state' nodelist that includes
 * another object identifier as its neighbor.
 *
 * Part of the last test is to have a handler from one Dmn_DMesgNet_Kafka
 * object to subscribe a topic and then another handler of another
 * Dmn_DMesgNet_Kafka object to write a series of topic message over kafka
 * network.
 */

#include "kafka/dmn-dmesgnet-kafka.hpp"
#include "kafka/dmn-kafka.hpp"

#include "proto/dmn-dmesg.pb.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <sys/time.h>

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  // reader
  Dmn::Dmn_Kafka::ConfigType readConfigs_other{};
  readConfigs_other["bootstrap.servers"] = "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs_other["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs_other["sasl.password"] = "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs_other["security.protocol"] = "SASL_SSL";
  readConfigs_other["sasl.mechanisms"] = "PLAIN";
  readConfigs_other["group.id"] = "dmesg_other";
  readConfigs_other["auto.offset.reset"] = "earliest";
  readConfigs_other[Dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
  readConfigs_other[Dmn::Dmn_Kafka::PollTimeoutMs] = "7000";

  Dmn::Dmn_Kafka consumer_other{Dmn::Dmn_Kafka::Role::Consumer, readConfigs_other};

  // dmesgnet1
  // writer for DMesgNet
  Dmn::Dmn_Kafka::ConfigType configs{};
  configs["bootstrap.servers"] = "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  configs["sasl.username"] = "ICCN4A57TNKONPQ3";
  configs["sasl.password"] = "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  configs["security.protocol"] = "SASL_SSL";
  configs["sasl.mechanisms"] = "PLAIN";

  // dmesgnet1
  Dmn::Dmn_DMesgNet_Kafka dmesgnet1{"dmesg1", configs};

  // dmesgnet2
  Dmn::Dmn_DMesgNet_Kafka dmesgnet2{"dmesg2", configs};

  std::this_thread::sleep_for(std::chrono::seconds(5));

  // consume prior messages from topic.
  Dmn::DMesgPb dmesgPbRead{};
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

      masterList[dmesgPbRead.body().sys().self().identifier()] = dmesgPbRead.body().sys().self().masteridentifier();

      if (nodeList.size() == 2) {
        std::string master{};
        bool ok{true};

        for (auto & mp : masterList) {
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

  std::vector<std::string> topics{"counter sync 1", "counter sync 2"};
  std::vector<std::string> subscribedTopics{"counter sync 1"};

  int cnt{0};
  std::shared_ptr<Dmn::Dmn_DMesg::Dmn_DMesgHandler> dmesgHandler = dmesgnet1.openHandler(subscribedTopics, "handler1",
                                                                     false, nullptr,
                                                                     [&cnt](const Dmn::DMesgPb &msg) mutable {
                                                                            EXPECT_TRUE("counter sync 1" == msg.topic());
                                                                            cnt++;
                                                                     });
  EXPECT_TRUE(dmesgHandler);

  auto dmesgWriteHandler = dmesgnet2.openHandler("writeHandler");
  EXPECT_TRUE(dmesgWriteHandler);

  std::this_thread::sleep_for(std::chrono::seconds(3));

  for (int n = 0; n < 6; n++) {
    Dmn::DMesgPb dmesgPb{};
    dmesgPb.set_topic(topics[n % 2]);
    dmesgPb.set_type(Dmn::DMesgTypePb::message);

    std::string data{"Hello dmesg async"};
    Dmn::DMesgBodyPb *dmsgbodyPb = dmesgPb.mutable_body();
    dmsgbodyPb->set_message(data);

    dmesgWriteHandler->write(dmesgPb);
  }

  std::this_thread::sleep_for(std::chrono::seconds(8));

  dmesgnet2.closeHandler(dmesgWriteHandler);
  dmesgnet1.closeHandler(dmesgHandler);
  EXPECT_TRUE(3 == cnt);

  return RUN_ALL_TESTS();
}
