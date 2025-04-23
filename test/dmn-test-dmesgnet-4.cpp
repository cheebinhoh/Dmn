/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that Dmn_DMesgNet object will self
 * proclaim as the master node if no Dmn_DMesgNet object in its
 * network, and when the object is destroyed, it will relinquish
 * its master node status and inform others that it is in destroyed
 * state in its last heartbeat message.
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

  dmn::DMesgPb dmesgPbLast{};
  std::unique_ptr<dmn::Dmn_Io<std::string>> readSocket2 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000);
  dmn::Dmn_Proc readProc{
      "readSocket2", [&readSocket2, &dmesgPbLast]() mutable {
        while (true) {
          auto data = readSocket2->read();
          if (data) {
            dmn::DMesgPb dmesgPbRead{};

            dmesgPbRead.ParseFromString(*data);
            dmesgPbLast = dmesgPbRead;
            std::cout << "DMesgPb: " << dmesgPbRead.ShortDebugString() << "\n";
          } else {
            break;
          }
        }
      }};

  readProc.exec();
  dmn::Dmn_Proc::yield();

  std::unique_ptr<dmn::Dmn_Io<std::string>> writeSocket1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000, true);

  std::unique_ptr<dmn::Dmn_Io<std::string>> readSocket1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5001);
  std::unique_ptr<dmn::Dmn_DMesgNet> dmesgnet1 =
      std::make_unique<dmn::Dmn_DMesgNet>("dmesg1", std::move(readSocket1),
                                          std::move(writeSocket1));
  readSocket1.reset();
  writeSocket1.reset();
  std::this_thread::sleep_for(std::chrono::seconds(10));
  dmesgnet1->waitForEmpty();
  EXPECT_TRUE(dmesgPbLast.body().sys().self().masteridentifier() != "");
  EXPECT_TRUE(dmesgPbLast.body().sys().self().state() ==
              dmn::DMesgStatePb::Ready);

  dmesgnet1 = {};
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(dmesgPbLast.body().sys().self().masteridentifier() == "");
  EXPECT_TRUE(dmesgPbLast.body().sys().self().state() ==
              dmn::DMesgStatePb::Destroyed);

  return RUN_ALL_TESTS();
}
