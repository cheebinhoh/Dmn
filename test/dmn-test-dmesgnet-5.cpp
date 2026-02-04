/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * Unit test: dmn-test-dmesgnet-5.cpp
 *
 * Purpose
 * -------
 * This test verifies that Dmn_DMesgNet instance that receives a DMesgPb
 * message that the topic counter is out of sync will put all local write
 * handlers of the Dmn_DmesgNet in conflict mode.
 *
 * Test setup and components
 * ------------------------
 * - One Dmn_Pipe<std::string> objects emulate bidirectional pipe
 *   (read/write for single node).
 * - One forwarding Dmn_Proc thread (dmesg1_to_dmesg2)
 *   continuously read serialized DMesgPb strings from one pipe, parse a copy
 *   into DMesgPb variables for assertions, and write the same serialized data
 *   into the opposite pipe.
 * - If the DMesgPb message read is not a sys message, it will then feeds
 *   a message with same running counter backward to the inbound pipe to the
 *   Dmn_DMesgNet object.
 *
 * Assertions and sequence
 * -----------------------
 * - the local write handler is able to send out 1st DMesgPb message, and then
 *   put into conflict mode after the inbound DMesgPb feeding into DMesgNet
 *   put all handlers writing the same topic into conflict mode.
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
#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"

#include "proto/dmn-dmesg-body.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::shared_ptr<dmn::Dmn_Io<std::string>> write_1 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("write_1");
  std::shared_ptr<dmn::Dmn_Io<std::string>> read_1 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("read_1");

  std::shared_ptr<dmn::Dmn_Io<std::string>> write_2 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("write_2");
  std::shared_ptr<dmn::Dmn_Io<std::string>> read_2 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("read_2");

  auto read_from_write_1 = write_1;
  auto write_to_read_1 = read_1;

  auto read_from_write_2 = write_2;
  auto write_to_read_2 = read_2;

  dmn::DMesgPb dmesgpb1{};
  dmn::DMesgPb dmesgpb1_body{};
  dmn::DMesgPb dmesgpb1_sys{};
  dmn::DMesgPb dmesgpb1_body_conflict{};
  dmn::Dmn_Proc dmesg1_to_dmesg2{
      "dmesg1_to_dmesg2",
      [read_from_write_1, write_to_read_2, write_to_read_1, &dmesgpb1,
       &dmesgpb1_body, &dmesgpb1_body_conflict, &dmesgpb1_sys]() {
        while (true) {
          auto data = read_from_write_1->read();
          if (!data) {
            break;
          }

          dmesgpb1.ParseFromString(*data);
          if (dmesgpb1.type() == dmn::DMesgTypePb::sys) {
            dmesgpb1_sys = dmesgpb1;
          } else if (dmesgpb1.conflict()) {
            std::cout << "********* conflict: " << dmesgpb1.ShortDebugString()
                      << "\n";
            dmesgpb1_body_conflict = dmesgpb1;
          } else {
            dmesgpb1_body = dmesgpb1;

            dmn::DMesgPb dmesgpbWritten = dmesgpb1;
            dmesgpbWritten.set_topic("counter sync");
            dmesgpbWritten.set_runningcounter(1);
            dmesgpbWritten.set_type(dmn::DMesgTypePb::message);
            dmesgpbWritten.set_sourceidentifier("writehandler2");
            dmesgpbWritten.set_sourcewritehandleridentifier("writehandler2");

            std::string serialized_string{};
            dmesgpbWritten.SerializeToString(&serialized_string);

            write_to_read_1->write(serialized_string);
          }

          write_to_read_2->write(*data);
        }
      }};

  dmesg1_to_dmesg2.exec();

  auto dmesgnet1 = std::make_unique<dmn::Dmn_DMesgNet>(
      "dmesg1", std::move(read_1), std::move(write_1));

  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto dmesg_handleRead =
      dmesgnet1->openHandler("readHandler1", "counter sync");

  std::this_thread::sleep_for(std::chrono::seconds(15));
  auto sys1 = dmesgpb1_sys.body().sys().self();

  EXPECT_TRUE((sys1.state() == dmn::DMesgStatePb::Ready));
  EXPECT_TRUE(("dmesg1" == sys1.masteridentifier()));

  dmn::DMesgPb dmesgpb{};
  dmesgpb.set_topic("counter sync");
  dmesgpb.set_type(dmn::DMesgTypePb::message);
  dmesgpb.set_sourceidentifier("writehandler");

  std::cout << "before write\n";
  auto dmesg_handle = dmesgnet1->openHandler("writeHandler");
  EXPECT_TRUE(dmesg_handle);
  dmesg_handle->write(dmesgpb);

  std::this_thread::sleep_for(std::chrono::seconds(10));
  EXPECT_TRUE(("counter sync" == dmesgpb1_body.topic()));
  EXPECT_TRUE((dmn::DMesgTypePb::message == dmesgpb1_body.type()));

  auto dmesgpbRead = dmesg_handleRead->read();
  EXPECT_TRUE((dmesgpbRead));
  EXPECT_TRUE(("counter sync" == dmesgpbRead->topic()));

  std::cout << "after read: " << dmesgpbRead->ShortDebugString() << "\n";

  std::this_thread::sleep_for(std::chrono::seconds(5));
  auto inConflict = dmesg_handle->isInConflict();
  EXPECT_TRUE((inConflict));

  dmesgnet1 = {};
  std::cout << "after destroying 1\n";
  std::this_thread::sleep_for(std::chrono::seconds(10));

  EXPECT_TRUE((dmesgpb1_body_conflict.conflict()));
  EXPECT_TRUE((dmesgpb1_body_conflict.topic() == "counter sync"));
  EXPECT_TRUE((dmesgpb1_body_conflict.runningcounter() == 1));

  return RUN_ALL_TESTS();
}
