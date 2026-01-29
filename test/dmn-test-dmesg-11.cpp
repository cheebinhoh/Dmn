/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesg-1.cpp
 * @brief The unit test that asserts that the dmn-dmesg with one publisher and
 *        two subscribers work.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg.pb.h"

#include "dmn-dmesg.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_DMesg dmesg{"dmesg"};

  auto dmesg_write_handle = dmesg.openHandler("writeHandler");
  EXPECT_TRUE(dmesg_write_handle);

  auto dmesg_read_handle1 = dmesg.openHandler("readHandler1");

  auto dmesg_read_handle2 = dmesg.openHandler(
      "readHandler2", [](const dmn::DMesgPb &dmesgpb) -> bool {
        return dmesgpb.body().message() != "message string 1";
      });

  auto dmesg_read_handle_id2 = dmesg.openHandler("readHandler1", "id2");

  dmn::DMesgPb dmesgpb1{};
  dmesgpb1.set_topic("id1");
  dmesgpb1.set_runningcounter(99);
  dmesgpb1.set_sourceidentifier("unknown");
  dmesgpb1.set_type(dmn::DMesgTypePb::message);

  dmn::DMesgBodyPb *dmesgpb_body1 = dmesgpb1.mutable_body();
  dmesgpb_body1->set_message("message string 1");

  dmn::DMesgPb dmesgpb2{};
  dmesgpb2.set_topic("id1");
  dmesgpb2.set_runningcounter(99);
  dmesgpb2.set_sourceidentifier("unknown");
  dmesgpb2.set_type(dmn::DMesgTypePb::message);

  dmn::DMesgBodyPb *dmesgpb_body2 = dmesgpb2.mutable_body();
  dmesgpb_body2->set_message("message string 2");

  dmn::DMesgPb dmesgpb3 = dmesgpb2;
  dmn::DMesgPb dmesgpb4 = dmesgpb3;

  dmn::Dmn_Proc proc_read1{
      "read1", [&dmesg_read_handle1, &dmesgpb1]() -> void {
        std::cout << "before read1\n";
        auto dmesgpb_read = dmesg_read_handle1->read();
        std::cout << "after read1, and proceed to validate read\n";
        EXPECT_TRUE(dmesgpb_read->topic() == dmesgpb1.topic());
        EXPECT_TRUE(dmesgpb_read->sourceidentifier() ==
                    dmesgpb1.sourceidentifier());
        EXPECT_TRUE(dmesgpb_read->runningcounter() ==
                    dmesgpb1.runningcounter());
        EXPECT_TRUE(dmesgpb_read->type() == dmesgpb1.type());
        EXPECT_TRUE(dmesgpb_read->body().message() ==
                    dmesgpb1.body().message());
      }};

  dmn::Dmn_Proc proc_read2{
      "read2", [&dmesg_read_handle2, &dmesgpb2]() -> void {
        std::cout << "before read2\n";
        auto dmesgpb_read = dmesg_read_handle2->read();
        std::cout << "after read2, and proceed to validate read\n";
        EXPECT_TRUE(dmesgpb_read->topic() == dmesgpb2.topic());
        EXPECT_TRUE(dmesgpb_read->sourceidentifier() ==
                    dmesgpb2.sourceidentifier());
        EXPECT_TRUE(dmesgpb_read->runningcounter() ==
                    dmesgpb2.runningcounter());
        EXPECT_TRUE(dmesgpb_read->type() == dmesgpb2.type());
        EXPECT_TRUE(dmesgpb_read->body().message() ==
                    dmesgpb2.body().message());
      }};

  proc_read1.exec();
  proc_read2.exec();
  dmn::Dmn_Proc::yield();

  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::cout << "after sleep 3 seconds\n";

  dmesg_write_handle->write(dmesgpb1);
  dmesg_write_handle->write(dmesgpb2);

  dmesg.waitForEmpty();
  dmesg_read_handle1->waitForEmpty();
  dmesg_read_handle2->waitForEmpty();

  std::cout << "after sleep 5 seconds\n";
  std::this_thread::sleep_for(std::chrono::seconds(5));

  proc_read1.wait();
  proc_read2.wait();

  auto runningCounter = dmesg_write_handle->getTopicRunningCounter("id1");

  std::cout << "topic runningCounter: " << runningCounter << "\n";

  runningCounter--;
  dmesg_write_handle->setTopicRunningCounter("id1", runningCounter);

  runningCounter = dmesg_write_handle->getTopicRunningCounter("id1");
  std::cout << "topic runningCounter: " << runningCounter << "\n";

  auto ok = dmesg_write_handle->writeAndCheckConflict(dmesgpb3);
  EXPECT_TRUE((!ok));
  std::cout << "write without conflict: " << ok << "\n";

  auto inConflict = dmesg_write_handle->isInConflict();
  EXPECT_TRUE((inConflict));

  inConflict = dmesg_read_handle1->isInConflict("id2");
  EXPECT_TRUE((!inConflict));

  inConflict = dmesg_read_handle1->isInConflict("id1");
  EXPECT_TRUE((inConflict));

  inConflict = dmesg_read_handle1->isInConflict("id2");
  EXPECT_TRUE((!inConflict));

  std::cout << "****** reset\n";
  dmesg.resetConflictStateWithLastTopicMessage("id1");

  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::cout << "****** after reset\n";

  inConflict = dmesg_write_handle->isInConflict();
  EXPECT_TRUE((!inConflict));

  std::this_thread::sleep_for(std::chrono::seconds(2));

  inConflict = dmesg_read_handle1->isInConflict();
  EXPECT_TRUE((!inConflict));

  auto dmesg_read_handle3 = dmesg.openHandler("readHandler3");

  runningCounter = dmesg_read_handle3->getTopicRunningCounter("id1");
  std::cout << "Running counter for read_handle3: " << runningCounter << "\n";
  EXPECT_TRUE((2 == runningCounter));

  dmesg.closeHandler(dmesg_write_handle);
  dmesg.closeHandler(dmesg_read_handle1);
  dmesg.closeHandler(dmesg_read_handle2);
  dmesg.closeHandler(dmesg_read_handle3);
  dmesg.closeHandler(dmesg_read_handle_id2);

  return RUN_ALL_TESTS();
}
