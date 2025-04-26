/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test programs asserts that the DMesgNet object does
 * send out sys message and self-proclaim as a master node by
 * set up another Socket that receives data at the particular
 * ip and port that the DMesgNet object send out message through
 * its outbound Io object.
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
  std::unique_ptr<dmn::Io<std::string>> readSocket2 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5000);
  dmn::Proc readProc{"readSocket2", [&readSocket2, &dmesgPbLast]() mutable {
                       while (true) {
                         auto data = readSocket2->read();
                         if (data) {
                           dmn::DMesgPb dmesgPbRead{};

                           dmesgPbRead.ParseFromString(*data);
                           dmesgPbLast = dmesgPbRead;
                           std::cout
                               << "DMesgPb: " << dmesgPbRead.ShortDebugString()
                               << "\n";
                         } else {
                           break;
                         }
                       }
                     }};

  readProc.exec();
  dmn::Proc::yield();

  std::unique_ptr<dmn::Io<std::string>> writeSocket1 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5000, true);

  std::unique_ptr<dmn::Io<std::string>> readSocket1 =
      std::make_unique<dmn::Socket>("127.0.0.1", 5001);
  dmn::DMesgNet dmesgnet1{"dmesg1", std::move(readSocket1),
                          std::move(writeSocket1)};
  readSocket1.reset();
  writeSocket1.reset();

  std::this_thread::sleep_for(std::chrono::seconds(10));
  dmesgnet1.waitForEmpty();
  EXPECT_TRUE(dmesgPbLast.body().sys().self().masteridentifier() != "");
  EXPECT_TRUE(dmesgPbLast.body().sys().self().state() ==
              dmn::DMesgStatePb::Ready);

  return RUN_ALL_TESTS();
}
