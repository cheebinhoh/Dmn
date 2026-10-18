/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesg-3.cpp
 * @brief The unit test that asserts that the dmn-dmesg with two subscribers of
 * the same Dmn_DMesg object will receive the same DMesgPb message
 * published by publisher of the Dmn_DMesg object.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <iostream>
#include <string>

#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg.pb.h"

#include "dmn-dmesg.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  {
    dmn::Dmn_DMesg dmesg{"dmesg"};
    std::promise<std::string> received1;
    auto received1_future = received1.get_future();

    auto dmesg_handle1 = dmesg.openHandler(
        "handler1", nullptr, [&received1](const dmn::DMesgPb &dmesgpb) -> void {
          received1.set_value(dmesgpb.body().message());
        });

    EXPECT_TRUE(dmesg_handle1);

    std::promise<std::string> received2;
    auto received2_future = received2.get_future();
    auto dmesg_handle2 = dmesg.openHandler(
        "handler2", nullptr, [&received2](const dmn::DMesgPb &dmesgpb) -> void {
          received2.set_value(dmesgpb.body().message());
        });

    EXPECT_TRUE(dmesg_handle2);

    auto dmesg_handle3 = dmesg.openHandler("handler3");
    EXPECT_TRUE(dmesg_handle3);

    dmn::DMesgPb dmesgpb{};
    dmesgpb.set_topic("counter sync");
    dmesgpb.set_type(dmn::DMesgTypePb::message);

    std::string data{"Hello dmesg async"};
    dmn::DMesgBodyPb *dmesgpb_body = dmesgpb.mutable_body();
    dmesgpb_body->set_message(data);

    dmesg_handle3->write(dmesgpb);

    auto received1_status = received1_future.wait_for(std::chrono::seconds(30));
    EXPECT_EQ(received1_status, std::future_status::ready);
    if (received1_status == std::future_status::ready) {
      EXPECT_EQ(data, received1_future.get());
    }

    auto received2_status = received2_future.wait_for(std::chrono::seconds(30));
    EXPECT_EQ(received2_status, std::future_status::ready);
    if (received2_status == std::future_status::ready) {
      EXPECT_EQ(data, received2_future.get());
    }

    dmesg.waitForEmpty();
    dmesg.closeHandler(dmesg_handle1);
    dmesg.closeHandler(dmesg_handle2);
    dmesg.closeHandler(dmesg_handle3);
  }

  google::protobuf::ShutdownProtobufLibrary();

  return RUN_ALL_TESTS();
}
