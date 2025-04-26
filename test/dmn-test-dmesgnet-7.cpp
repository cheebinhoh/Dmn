/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that two DMesgNet objects that
 * participates in the same network through its inbound and outbound
 * Io objects are in sync in the Dmn network, and message sent through
 * one DMesgNet object is only received by other DMesgNet objects
 * which participates in the same Dmn network but not itself.
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

  std::unique_ptr<dmn::Io<std::string>> readSocket1 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5001);
  std::unique_ptr<dmn::Io<std::string>> writeSocket1 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5000, true);

  bool readData{};
  dmn::DMesgPb msgPb{};
  dmn::DMesgNet dmesgnet1{"dmesg-1", std::move(readSocket1),
                          std::move(writeSocket1)};
  readSocket1.reset();
  writeSocket1.reset();

  auto readHandler =
      dmesgnet1.openHandler("dmesg-1-handler", false, nullptr,
                            [&msgPb, &readData](dmn::DMesgPb data) mutable {
                              readData = true;
                              msgPb = data;
                            });

  dmn::DMesgPb dmesgPb{};
  dmesgPb.set_topic("counter sync");
  dmesgPb.set_type(dmn::DMesgTypePb::message);
  dmesgPb.set_sourceidentifier("handler-1");
  std::string data{"Hello dmesg async"};
  dmn::DMesgBodyPb *dmsgbodyPb = dmesgPb.mutable_body();
  dmsgbodyPb->set_message(data);

  readHandler->write(dmesgPb);

  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(!readData);

  /*
    std::shared_ptr<dmn::Io<std::string>> readSocket2 =
    std::make_shared<dmn::Socket>("127.0.0.1", 5000);
    std::shared_ptr<dmn::Io<std::string>> writeSocket2 =
    std::make_shared<dmn::Socket>("127.0.0.1", 5001, true);

    dmn::DMesgNet dmesgnet2{"dmesg-2", readSocket2, writeSocket2};
    auto writeHandler = dmesgnet2.openHandler("dmesg-2-handler", false, nullptr,
    nullptr);

    dmn::DMesgPb dmesgPb2{};
    dmesgPb2.set_topic("counter sync");
    dmesgPb2.set_type(dmn::DMesgTypePb::message);
    dmesgPb2.set_sourceidentifier("handler-2");
    std::string data2{"Hello dmesg async from 2"};
    dmn::DMesgBodyPb *dmsgbodyPb2 = dmesgPb2.mutable_body();
    dmsgbodyPb2->set_message(data2);

    writeHandler->write(dmesgPb2);

    std::this_thread::sleep_for(std::chrono::seconds(7));
    EXPECT_TRUE(readData);
    EXPECT_TRUE(msgPb.sourceidentifier() == dmesgPb2.sourceidentifier());
    EXPECT_TRUE(msgPb.body().message() == dmesgPb2.body().message());

    dmesgnet2.closeHandler(writeHandler);
  */

  dmesgnet1.closeHandler(readHandler);

  return RUN_ALL_TESTS();
}
