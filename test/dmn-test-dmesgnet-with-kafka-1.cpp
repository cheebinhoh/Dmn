/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesgnet-with-kafka-1.cpp
 * @brief Unit test for Dmn_DMesgNet construction and teardown with Dmn_Kafka as
 * I/O handlers.
 *
 * This test constructs Dmn_DMesgNet with external Dmn_Kafka input and output
 * handlers and verifies that teardown completes without problems.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "dmn-dmesgnet.hpp"

#include "kafka/dmn-kafka.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  {
    // writer
    dmn::Dmn_Kafka::ConfigType write_configs{};
    write_configs["bootstrap.servers"] = "localhost:9092";
    write_configs["acks"] = "all";
    write_configs[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
    write_configs[dmn::Dmn_Kafka::Key] = "Dmn_dmesgnet";

    std::shared_ptr<dmn::Dmn_Kafka> producer{std::make_unique<dmn::Dmn_Kafka>(
        dmn::Dmn_Kafka::Role::kProducer, write_configs)};

    // reader
    dmn::Dmn_Kafka::ConfigType read_configs{};
    read_configs["bootstrap.servers"] = "localhost:9092";
    read_configs["group.id"] = "dmesg1";
    read_configs[dmn::Dmn_Kafka::Topic] = "Dmn_dmesgnet";
    read_configs["auto.offset.reset"] = "latest";
    read_configs[dmn::Dmn_Kafka::PollTimeoutMs] = "1000";

    std::shared_ptr<dmn::Dmn_Kafka> consumer{std::make_unique<dmn::Dmn_Kafka>(
        dmn::Dmn_Kafka::Role::kConsumer, read_configs)};

    const dmn::Dmn_DMesgNet dmesgnet1{"dmesg1", std::move(consumer),
                                      std::move(producer)};
    consumer.reset();
    producer.reset();

    std::this_thread::sleep_for(std::chrono::seconds(5));
  }

  google::protobuf::ShutdownProtobufLibrary();

  return RUN_ALL_TESTS();
}
