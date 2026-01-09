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
#include <string>
#include <thread>
#include <utility>

#include "dmn-dmesgnet.hpp"
#include "dmn-io.hpp"
#include "dmn-proc.hpp"
#include "dmn-socket.hpp"

#include "proto/dmn-dmesg-body.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::DMesgPb dmesgpb_last{};
  std::shared_ptr<dmn::Dmn_Io<std::string>> read_socket_2 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000);
  dmn::Dmn_Proc read_proc{
      "read_socket_2", [&read_socket_2, &dmesgpb_last]() mutable -> void {
        while (true) {
          auto data = read_socket_2->read();
          if (data) {
            dmn::DMesgPb dmesgpb_read{};

            dmesgpb_read.ParseFromString(*data);
            dmesgpb_last = dmesgpb_read;
            std::cout << "DMesgPb: " << dmesgpb_read.ShortDebugString() << "\n";
          } else {
            break;
          }
        }
      }};

  read_proc.exec();
  dmn::Dmn_Proc::yield();

  std::shared_ptr<dmn::Dmn_Io<std::string>> write_socket_1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000, true);

  std::shared_ptr<dmn::Dmn_Io<std::string>> read_socket_1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5001);
  std::shared_ptr<dmn::Dmn_DMesgNet> dmesgnet1 =
      std::make_unique<dmn::Dmn_DMesgNet>("dmesg1", std::move(read_socket_1),
                                          std::move(write_socket_1));
  read_socket_1.reset();
  write_socket_1.reset();
  std::this_thread::sleep_for(std::chrono::seconds(10));
  dmesgnet1->waitForEmpty();
  EXPECT_TRUE(!dmesgpb_last.body().sys().self().masteridentifier().empty());
  EXPECT_TRUE(dmesgpb_last.body().sys().self().state() ==
              dmn::DMesgStatePb::Ready);

  dmesgnet1 = {};
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(dmesgpb_last.body().sys().self().masteridentifier().empty());
  EXPECT_TRUE(dmesgpb_last.body().sys().self().state() ==
              dmn::DMesgStatePb::Destroyed);

  return RUN_ALL_TESTS();
}
