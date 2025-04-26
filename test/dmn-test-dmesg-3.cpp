/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that two subscribers of the same
 * Dmn_DMesg object will receive the same DMesgPb message
 * published by publisher of the Dmn_DMesg object.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>

#include "dmn-dmesg.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_DMesg dmesg{"dmesg"};
  std::string data1{};
  std::string data2{};

  auto dmesgHandler1 =
      dmesg.openHandler("handler1", nullptr, [&data1](dmn::DMesgPb dmesgpb) {
        data1 = dmesgpb.body().message();
      });
  EXPECT_TRUE(dmesgHandler1);

  auto dmesgHandler2 =
      dmesg.openHandler("handler2", nullptr, [&data2](dmn::DMesgPb dmesgpb) {
        data2 = dmesgpb.body().message();
      });

  EXPECT_TRUE(dmesgHandler2);

  auto dmesgHandler3 = dmesg.openHandler("handler3", nullptr, nullptr);
  EXPECT_TRUE(dmesgHandler3);

  dmn::DMesgPb dmesgpb{};
  dmesgpb.set_topic("counter sync");
  dmesgpb.set_type(dmn::DMesgTypePb::message);

  std::string data{"Hello dmesg async"};
  dmn::DMesgBodyPb *dmsgbodyPb = dmesgpb.mutable_body();
  dmsgbodyPb->set_message(data);

  dmesgHandler3->write(dmesgpb);

  std::this_thread::sleep_for(std::chrono::seconds(5));

  std::cout << "after wait for data to sync\n";

  dmesg.waitForEmpty();
  dmesgHandler1->waitForEmpty();
  dmesgHandler2->waitForEmpty();
  dmesgHandler3->waitForEmpty();
  dmesg.closeHandler(dmesgHandler1);
  dmesg.closeHandler(dmesgHandler2);
  dmesg.closeHandler(dmesgHandler3);

  EXPECT_TRUE(data == data1);
  EXPECT_TRUE(data == data2);

  return RUN_ALL_TESTS();
}
