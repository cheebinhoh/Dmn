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
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "dmn-dmesgnet.hpp"
#include "dmn-io.hpp"
#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"
#include "dmn-socket.hpp"

#include "proto/dmn-dmesg-body.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::shared_ptr<dmn::Dmn_Io<std::string>> pipe_2 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("pipe_2");

  std::shared_ptr<dmn::Dmn_Io<std::string>> pipe_1 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>(
          "pipe_1", [pipe_2](std::string &&data) { pipe_2->write(data); });

  dmn::Dmn_DMesgNet dmesgnet1{"dmesg1", nullptr, std::move(pipe_1)};

  dmn::Dmn_DMesgNet dmesgnet2{"dmesg2", std::move(pipe_2)};

  auto readHandler2 = dmesgnet2.openHandler("dmesg2.readHandler");

  dmn::DMesgPb dmesgpb_read{};
  dmn::Dmn_Proc proc2{"dmesg2", [readHandler2, &dmesgpb_read]() -> void {
                        auto data = readHandler2->read();
                        if (data) {
                          dmesgpb_read = *data;
                        }
                      }};

  proc2.exec();
  dmn::Dmn_Proc::yield();
  std::this_thread::sleep_for(std::chrono::seconds(3));

  auto dmesg_handle = dmesgnet1.openHandler("writeHandler");
  EXPECT_TRUE(dmesg_handle);

  dmn::DMesgPb dmesgpb{};
  dmesgpb.set_topic("counter sync");
  dmesgpb.set_type(dmn::DMesgTypePb::message);
  dmesgpb.set_sourceidentifier("writehandler");

  std::string data{"Hello dmesg async"};
  dmn::DMesgBodyPb *dmesgpb_body = dmesgpb.mutable_body();
  dmesgpb_body->set_message(data);

  dmesg_handle->write(dmesgpb);

  std::this_thread::sleep_for(std::chrono::seconds(5));

  const std::string source = dmesgpb_read.sourceidentifier();
  EXPECT_TRUE(dmesgpb_read.type() == dmesgpb.type());
  EXPECT_TRUE(dmesgpb_read.sourceidentifier() ==
              dmesgpb.sourceidentifier()); // the source is the local DmesgNet
                                           // agent that read
  EXPECT_TRUE(dmesgpb_read.body().message() == dmesgpb.body().message());

  std::this_thread::sleep_for(std::chrono::seconds(5));

  return RUN_ALL_TESTS();
}
