/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include <sys/time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "kafka/dmn-kafka.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  // writer
  dmn::Dmn_Kafka::ConfigType writeConfigs{};
  writeConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  writeConfigs["sasl.username"] = "ICCN4A57TNKONPQ3";
  writeConfigs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  writeConfigs["security.protocol"] = "SASL_SSL";
  writeConfigs["sasl.mechanisms"] = "PLAIN";
  writeConfigs["acks"] = "all";
  writeConfigs[dmn::Dmn_Kafka::Topic] = "timer_counter";
  writeConfigs[dmn::Dmn_Kafka::Key] = "tick";

  dmn::Dmn_Kafka producer{dmn::Dmn_Kafka::Role::Producer, writeConfigs};

  // reader
  dmn::Dmn_Kafka::ConfigType readConfigs{};
  readConfigs["bootstrap.servers"] =
      "pkc-619z3.us-east1.gcp.confluent.cloud:9092";
  readConfigs["sasl.username"] = "ICCN4A57TNKONPQ3";
  readConfigs["sasl.password"] =
      "Fz6AqWg1WCBqkBV2FX2FD/9iBNbs1qHM5Po12iaVn6OMVKZm8WhH4W20IaZTTEcV";
  readConfigs["security.protocol"] = "SASL_SSL";
  readConfigs["sasl.mechanisms"] = "PLAIN";
  readConfigs["group.id"] = "dmn-kafka-receiver";
  readConfigs[dmn::Dmn_Kafka::Topic] = "timer_counter";
  readConfigs["auto.offset.reset"] = "earliest";

  dmn::Dmn_Kafka consumer{dmn::Dmn_Kafka::Role::Consumer, readConfigs};

  std::vector<std::string> data{"heartbeat : test 1", "heartbeat : test 2"};
  for (auto &d : data) {
    producer.write(d);
  }

  // sleep and wait for data to sync to reader
  std::this_thread::sleep_for(std::chrono::seconds(7));
  int index = 0;

  while (true) {
    auto dataRead = consumer.read();
    if (!dataRead) {
      break; // no data
    }

    EXPECT_TRUE((*dataRead) == data[index]);

    index++;
  }

  EXPECT_TRUE(data.size() == index);

  readConfigs[dmn::Dmn_Kafka::PollTimeoutMs] = "7000";
  dmn::Dmn_Kafka consumer2{dmn::Dmn_Kafka::Role::Consumer, readConfigs};

  std::cout << "read without data\n";
  struct timeval tv;
  gettimeofday(&tv, NULL);

  auto dataRead = consumer2.read();
  struct timeval tvAfter;
  gettimeofday(&tvAfter, NULL);

  EXPECT_TRUE((tvAfter.tv_sec - tv.tv_sec) >= 5);

  return RUN_ALL_TESTS();
}
