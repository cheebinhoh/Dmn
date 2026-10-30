/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesg-7.cpp
 * @brief The unit test that asserts that the dmn-dmesg with two subscribers of
 * the same Dmn_DMesg object will receive different message filtered by
 * topic.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <utility>

#include "dmn-dmesg.hpp"

#include "proto/dmn-dmesg-body.pb.h"
#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  {
    auto dmesg{std::make_unique<dmn::Dmn_DMesg>("dmesg")};
    auto topic1_write_handler{
        dmesg->openHandler("topic1_write_handler", "topic1")};
    auto topic2_write_handler{
        dmesg->openHandler("topic2_write_handler", "topic2")};

    std::promise<dmn::DMesgPb> topic1_message;
    auto topic1_message_future{topic1_message.get_future()};
    auto topic1_read_handler{
        dmesg->openHandler("topic1_read_handler", "topic1", nullptr,
                           [&topic1_message](dmn::DMesgPb dmesgpb) -> void {
                             std::cout << "read topic1\n";
                             topic1_message.set_value(std::move(dmesgpb));
                           })};

    std::promise<dmn::DMesgPb> topic2_message;
    auto topic2_message_future{topic2_message.get_future()};
    auto topic2_read_handler{
        dmesg->openHandler("topic2_read_handler", "topic2", nullptr,
                           [&topic2_message](dmn::DMesgPb dmesgpb) -> void {
                             std::cout << "read topic2\n";
                             topic2_message.set_value(std::move(dmesgpb));
                           })};
    dmn::DMesgPb dmesgpb{};
    dmesgpb.set_type(dmn::DMesgTypePb::message);

    std::string data1{"Hello topic1"};
    dmn::DMesgBodyPb *dmesgpb_body = dmesgpb.mutable_body();
    dmesgpb_body->set_message(data1);

    topic1_write_handler->write(dmesgpb);

    auto topic1_status{
        topic1_message_future.wait_for(std::chrono::seconds(30))};
    EXPECT_EQ(topic1_status, std::future_status::ready);
    if (topic1_status == std::future_status::ready) {
      auto dmesgpb_topic1_read{topic1_message_future.get()};
      EXPECT_EQ("topic1", dmesgpb_topic1_read.topic());
      EXPECT_EQ("Hello topic1", dmesgpb_topic1_read.body().message());
    }

    dmesg->waitForEmpty();
    EXPECT_EQ(topic2_message_future.wait_for(std::chrono::seconds(0)),
              std::future_status::timeout);

    dmn::DMesgPb dmesgpb2{};
    dmesgpb2.set_type(dmn::DMesgTypePb::message);

    std::string data2{"Hello topic2"};
    dmn::DMesgBodyPb *dmesgpb_body2 = dmesgpb2.mutable_body();
    dmesgpb_body2->set_message(data2);

    topic2_write_handler->write(dmesgpb2);

    auto topic2_status{
        topic2_message_future.wait_for(std::chrono::seconds(30))};
    EXPECT_EQ(topic2_status, std::future_status::ready);
    if (topic2_status == std::future_status::ready) {
      auto dmesgpb_topic2_read{topic2_message_future.get()};
      EXPECT_EQ("topic2", dmesgpb_topic2_read.topic());
      EXPECT_EQ("Hello topic2", dmesgpb_topic2_read.body().message());
    }

    dmesg = {};
  }

  google::protobuf::ShutdownProtobufLibrary();

  return RUN_ALL_TESTS();
}
