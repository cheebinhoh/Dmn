/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This program asserts that a DMesgNet object will self-proclaim
 * as a master if it is started without other DMesgNet objects
 * and a emulated system message with other node identifier as master
 * will not derail the DMesgNet object notion of who is master
 * as long as the DMesgNet object timestamp is earlier than other.
 */

#include <sys/time.h>

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "dmn-dmesgnet.hpp"
#include "dmn-socket.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::unique_ptr<dmn::Io<std::string>> readSocket1 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5001);
  std::unique_ptr<dmn::Io<std::string>> writeSocket1 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5000, true);

  dmn::DMesgPb sysPb_3{};
  std::unique_ptr<dmn::DMesgNet> dmesgnet1 = std::make_unique<dmn::DMesgNet>(
      "dmesg-3", std::move(readSocket1), std::move(writeSocket1));
  readSocket1.reset();
  writeSocket1.reset();

  auto listenHandler3 = dmesgnet1->openHandler(
      "dmesg-3-listen", true, nullptr, [&sysPb_3](dmn::DMesgPb data) mutable {
        if (data.type() == dmn::DMesgTypePb::sys) {
          sysPb_3 = data;
        }
      });

  dmn::DMesgPb sysPb_4{};

  std::this_thread::sleep_for(std::chrono::seconds(1));
  dmn::Proc dmesg_4_Proc{
      "dmesg_4_Proc", [&sysPb_4]() mutable {
        std::unique_ptr<dmn::Io<std::string>> writeSocket1 =
            std::make_unique<dmn::Socket>("127.0.0.1", 5001, true);
        dmn::DMesgPb sys{};
        struct timeval tv;

        gettimeofday(&tv, NULL);

        DMESG_PB_SET_MSG_TOPIC(sys, "sys.dmn-dmesg");
        DMESG_PB_SET_MSG_TYPE(sys, dmn::DMesgTypePb::sys);
        DMESG_PB_SYS_SET_TIMESTAMP_FROM_TV(sys, tv);
        DMESG_PB_SET_MSG_SOURCEIDENTIFIER(sys, "dmesg-4");

        auto *self = sys.mutable_body()->mutable_sys()->mutable_self();
        DMESG_PB_SYS_NODE_SET_INITIALIZEDTIMESTAMP_FROM_TV(self, tv);
        DMESG_PB_SYS_NODE_SET_IDENTIFIER(self, "dmesg-4");
        DMESG_PB_SYS_NODE_SET_STATE(self, dmn::DMesgStatePb::Ready);
        DMESG_PB_SYS_NODE_SET_MASTERIDENTIFIER(self, "dmesg-4");

        for (long long n = 0; n < 30; n++) {
          DMESG_PB_SYS_NODE_SET_UPDATEDTIMESTAMP_FROM_TV(self, tv);
          std::string serialized_string{};

          sys.SerializeToString(&serialized_string);

          writeSocket1->write(serialized_string);
          std::this_thread::sleep_for(std::chrono::milliseconds(100));

          gettimeofday(&tv, NULL);
        }
      }};

  dmesg_4_Proc.exec();
  dmesg_4_Proc.wait();

  std::this_thread::sleep_for(std::chrono::seconds(10));
  EXPECT_TRUE(sysPb_3.body().sys().self().masteridentifier() == "dmesg-3");

  return RUN_ALL_TESTS();
}
