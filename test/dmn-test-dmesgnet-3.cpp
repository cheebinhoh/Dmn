/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * Unit test for Dmn_DMesgNet message sending/receiving and lifecycle events.
 *
 * This test verifies that a single Dmn_DMesgNet instance can:
 * - Open a handler and send DMesg application messages.
 * - Emit the expected system (sys) state messages in order:
 *     MasterPending -> Ready -> Destroyed.
 * - Deliver application messages through an in-process transport (Dmn_Pipe)
 *   and accept a subsequent message sent in response to the first one.
 * - Properly close a handler and signal destruction when the Dmn_DMesgNet
 *   object is destroyed.
 *
 * Test approach:
 * 1. Create a pair of Dmn_Pipe objects (used here as an in-process socket
 * loopback) and construct a Dmn_DMesgNet using them.
 * 2. Open a write handler and send two application messages, with the second
 *    one triggered after the first application message is received.
 * 3. Read from the pipe and assert the sequence and contents of messages:
 *    - Observe a MasterPending sys state before Ready.
 *    - Confirm Ready reports the expected master identifier ("dmesg1").
 *    - Confirm two application messages are received in total.
 * 4. Close the handler, destroy the Dmn_DMesgNet instance, then confirm a final
 *    sys message with state Destroyed is emitted.
 *
 * Notes:
 * - The test uses sleeps and blocking reads with timeouts to allow asynchronous
 *   operations to complete. These values may be adjusted if timing variability
 *   causes intermittent failures.
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

#include "proto/dmn-dmesg-body.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::shared_ptr<dmn::Dmn_Io<std::string>> write_1 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("127.0.0.1");
  std::shared_ptr<dmn::Dmn_Io<std::string>> read_1 =
      std::make_unique<dmn::Dmn_Pipe<std::string>>("127.0.0.1");

  auto read_from_write_1 = write_1;

  auto dmesgnet1 = std::make_unique<dmn::Dmn_DMesgNet>(
      "dmesg1", std::move(read_1), std::move(write_1));

  auto dmesg_handle = dmesgnet1->openHandler("writeHandler");
  EXPECT_TRUE(dmesg_handle);

  dmn::DMesgPb dmesgpb{};
  dmesgpb.set_topic("counter sync");
  dmesgpb.set_type(dmn::DMesgTypePb::message);
  dmesgpb.set_sourceidentifier("writehandler");

  std::string data{"Hello dmesg async"};
  dmn::DMesgBodyPb *dmesgpb_body = dmesgpb.mutable_body();
  dmesgpb_body->set_message(data);

  auto dmesgpb2 = dmesgpb;

  auto dmesgpb3 = dmesgpb;

  dmesg_handle->write(dmesgpb);
  dmesg_handle->write(dmesgpb2);

  std::this_thread::sleep_for(std::chrono::seconds(10));

  bool masterpending{};
  bool ready{};
  bool hasData{};
  size_t dataCount{};

  auto dataList = read_from_write_1->read(500, 10000000L);
  for (auto &data : dataList) {
    dmn::DMesgPb dmesgpb{};

    dmesgpb.ParseFromString(data);

    if (dmesgpb.type() == dmn::DMesgTypePb::sys) {
      auto sys = dmesgpb.body().sys().self();
      if (sys.state() == dmn::DMesgStatePb::MasterPending) {
        EXPECT_TRUE((!ready));
        masterpending = true;
      } else if (sys.state() == dmn::DMesgStatePb::Ready) {
        EXPECT_TRUE((masterpending));
        EXPECT_TRUE(("dmesg1" == sys.masteridentifier()));
        ready = true;
      }
    } else {
      EXPECT_TRUE(ready);
      dataCount++;
      hasData = true;

      if (1 == dataCount) {
        dmesg_handle->write(dmesgpb3);
      }
    }
  }

  EXPECT_TRUE(hasData);
  EXPECT_TRUE(2 == dataCount);

  hasData = false;
  dataCount = 0;

  dataList = read_from_write_1->read(100, 5000000L);
  for (auto &data : dataList) {
    dmn::DMesgPb dmesgpb{};

    dmesgpb.ParseFromString(data);

    if (dmesgpb.type() == dmn::DMesgTypePb::sys) {
      auto sys = dmesgpb.body().sys().self();

      EXPECT_TRUE((sys.state() == dmn::DMesgStatePb::Ready));
    } else {
      dataCount++;
      hasData = true;
    }
  }

  EXPECT_TRUE((hasData));
  EXPECT_TRUE((1 == dataCount));

  dmesgnet1->closeHandler(dmesg_handle);

  dmesgnet1 = {};
  size_t count{};
  dataList = read_from_write_1->read(100, 20000000L);
  for (auto &data : dataList) {
    dmn::DMesgPb dmesgpb{};

    dmesgpb.ParseFromString(data);
    if (count == (dataList.size() - 1)) {
      auto sys = dmesgpb.body().sys().self();

      EXPECT_TRUE((dmn::DMesgStatePb::Destroyed == sys.state()));
    }

    count++;
  }

  return RUN_ALL_TESTS();
}
