/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test programs asserts that two Dmn_DMesgNet objects can
 * one send message through a Dmn_Socket at a particular ip and port
 * and another one receive sent message through another Dmn_Socket
 * at the same ip and port.
 */

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

  std::unique_ptr<dmn::Dmn_Io<std::string>> writeSocket1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000, true);
  std::unique_ptr<dmn::Dmn_Io<std::string>> readSocket2 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000);

  dmn::Dmn_DMesgNet dmesgnet1{"dmesg1", nullptr, std::move(writeSocket1)};
  writeSocket1.reset();

  dmn::Dmn_DMesgNet dmesgnet2{"dmesg2", std::move(readSocket2)};
  readSocket2.reset();

  auto readHandler2 = dmesgnet2.openHandler("dmesg2.readHandler");

  dmn::DMesgPb dmesgPbRead{};
  dmn::Dmn_Proc proc2{"dmesg2", [readHandler2, &dmesgPbRead]() {
                        auto data = readHandler2->read();
                        if (data) {
                          dmesgPbRead = *data;
                        }
                      }};

  proc2.exec();
  dmn::Dmn_Proc::yield();
  std::this_thread::sleep_for(std::chrono::seconds(3));

  auto dmesgHandler = dmesgnet1.openHandler("writeHandler", nullptr, nullptr);
  EXPECT_TRUE(dmesgHandler);

  dmn::DMesgPb dmesgPb{};
  dmesgPb.set_topic("counter sync");
  dmesgPb.set_type(dmn::DMesgTypePb::message);
  dmesgPb.set_sourceidentifier("writehandler");
  std::string data{"Hello dmesg async"};
  dmn::DMesgBodyPb *dmsgbodyPb = dmesgPb.mutable_body();
  dmsgbodyPb->set_message(data);

  dmesgHandler->write(dmesgPb);

  std::this_thread::sleep_for(std::chrono::seconds(5));

  std::string source = dmesgPbRead.sourceidentifier();
  EXPECT_TRUE(dmesgPbRead.type() == dmesgPb.type());
  EXPECT_TRUE(dmesgPbRead.sourceidentifier() ==
              dmesgPb.sourceidentifier()); // the source is the local DmesgNet
                                           // agent that read
  EXPECT_TRUE(dmesgPbRead.body().message() == dmesgPb.body().message());

  std::this_thread::sleep_for(std::chrono::seconds(5));

  return RUN_ALL_TESTS();
}
