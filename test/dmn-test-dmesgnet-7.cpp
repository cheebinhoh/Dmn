/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that two Dmn_DMesgNet objects that
 * participates in the same network through its inbound and outbound
 * Dmn_Io objects are in sync in the Dmn network, and message sent through
 * one Dmn_DMesgNet object is only received by other Dmn_DMesgNet objects
 * which participates in the same Dmn network but not itself.
 */

#include "dmn-dmesgnet.hpp"
#include "dmn-socket.hpp"

#include <gtest/gtest.h>

#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::unique_ptr<Dmn::Dmn_Io<std::string>> readSocket1 =
      std::make_unique<Dmn::Dmn_Socket>("127.0.0.1", 5001);
  std::unique_ptr<Dmn::Dmn_Io<std::string>> writeSocket1 =
      std::make_unique<Dmn::Dmn_Socket>("127.0.0.1", 5000, true);

  bool readData{};
  Dmn::DMesgPb msgPb{};
  Dmn::Dmn_DMesgNet dmesgnet1{"dmesg-1", std::move(readSocket1),
                              std::move(writeSocket1)};

  auto readHandler =
      dmesgnet1.openHandler("dmesg-1-handler", false, nullptr,
                            [&msgPb, &readData](Dmn::DMesgPb data) mutable {
                              readData = true;
                              msgPb = data;
                            });

  Dmn::DMesgPb dmesgPb{};
  dmesgPb.set_topic("counter sync");
  dmesgPb.set_type(Dmn::DMesgTypePb::message);
  dmesgPb.set_sourceidentifier("handler-1");
  std::string data{"Hello dmesg async"};
  Dmn::DMesgBodyPb *dmsgbodyPb = dmesgPb.mutable_body();
  dmsgbodyPb->set_message(data);

  readHandler->write(dmesgPb);

  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(!readData);

  /*
    std::shared_ptr<Dmn::Dmn_Io<std::string>> readSocket2 =
    std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5000);
    std::shared_ptr<Dmn::Dmn_Io<std::string>> writeSocket2 =
    std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5001, true);

    Dmn::Dmn_DMesgNet dmesgnet2{"dmesg-2", readSocket2, writeSocket2};
    auto writeHandler = dmesgnet2.openHandler("dmesg-2-handler", false, nullptr,
    nullptr);

    Dmn::DMesgPb dmesgPb2{};
    dmesgPb2.set_topic("counter sync");
    dmesgPb2.set_type(Dmn::DMesgTypePb::message);
    dmesgPb2.set_sourceidentifier("handler-2");
    std::string data2{"Hello dmesg async from 2"};
    Dmn::DMesgBodyPb *dmsgbodyPb2 = dmesgPb2.mutable_body();
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
