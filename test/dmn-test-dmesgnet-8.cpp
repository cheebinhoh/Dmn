/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that two Dmn_DMesgNet objects that
 * participates in the same network through its inbound and outbound
 * Dmn_Io objects are in sync in the Dmn network, and message sent priorly
 * in one Dmn_DMesgNet will be resent by the master when a new
 * Dmn_DMesgNet object joins the same Dmn network.
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

  std::shared_ptr<Dmn::Dmn_Io<std::string>> readSocket1 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5001);
  std::shared_ptr<Dmn::Dmn_Io<std::string>> writeSocket1 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5000, true);

  bool readData{};
  Dmn::DMesgPb msgPb{};
  Dmn::Dmn_DMesgNet dmesgnet1{"dmesg-1", readSocket1, writeSocket1};

  auto writeHandler1 =
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

  writeHandler1->write(dmesgPb);

  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(!readData);

  std::shared_ptr<Dmn::Dmn_Io<std::string>> readSocket2 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5000);
  std::shared_ptr<Dmn::Dmn_Io<std::string>> writeSocket2 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5001, true);

  Dmn::Dmn_DMesgNet dmesgnet2{"dmesg-2", readSocket2, writeSocket2};
  Dmn::DMesgPb msgPb2{};
  auto readHandler2 =
      dmesgnet2.openHandler("dmesg-2-handler", false, nullptr,
                            [&msgPb2, &readData](Dmn::DMesgPb data) mutable {
                              readData = true;
                              msgPb2 = data;
                            });

  std::this_thread::sleep_for(std::chrono::seconds(10));
  EXPECT_TRUE(readData);
  EXPECT_TRUE(msgPb2.body().message() == dmesgPb.body().message());

  dmesgnet2.closeHandler(readHandler2);
  dmesgnet1.closeHandler(writeHandler1);

  return RUN_ALL_TESTS();
}
