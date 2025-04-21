/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that Dmn_DMesgNet object will self
 * proclaim as the master node if no Dmn_DMesgNet object in its
 * network, and when the object is destroyed, it will relinquish
 * its master node status and inform others that it is in destroyed
 * state in its last heartbeat message.
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

  Dmn::DMesgPb dmesgPbLast{};
  std::shared_ptr<Dmn::Dmn_Io<std::string>> readSocket2 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5000);
  Dmn::Dmn_Proc readProc{
      "readSocket2", [&readSocket2, &dmesgPbLast]() mutable {
        while (true) {
          auto data = readSocket2->read();
          if (data) {
            Dmn::DMesgPb dmesgPbRead{};

            dmesgPbRead.ParseFromString(*data);
            dmesgPbLast = dmesgPbRead;
            std::cout << "DMesgPb: " << dmesgPbRead.ShortDebugString() << "\n";
          } else {
            break;
          }
        }
      }};

  readProc.exec();
  Dmn::Dmn_Proc::yield();

  std::shared_ptr<Dmn::Dmn_Io<std::string>> writeSocket1 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5000, true);

  std::shared_ptr<Dmn::Dmn_Io<std::string>> readSocket1 =
      std::make_shared<Dmn::Dmn_Socket>("127.0.0.1", 5001);
  std::unique_ptr<Dmn::Dmn_DMesgNet> dmesgnet1 =
      std::make_unique<Dmn::Dmn_DMesgNet>("dmesg1", readSocket1, writeSocket1);
  readSocket1.reset();
  writeSocket1.reset();
  std::this_thread::sleep_for(std::chrono::seconds(10));
  dmesgnet1->waitForEmpty();
  EXPECT_TRUE(dmesgPbLast.body().sys().self().masteridentifier() != "");
  EXPECT_TRUE(dmesgPbLast.body().sys().self().state() ==
              Dmn::DMesgStatePb::Ready);

  dmesgnet1 = {};
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(dmesgPbLast.body().sys().self().masteridentifier() == "");
  EXPECT_TRUE(dmesgPbLast.body().sys().self().state() ==
              Dmn::DMesgStatePb::Destroyed);

  return RUN_ALL_TESTS();
}
