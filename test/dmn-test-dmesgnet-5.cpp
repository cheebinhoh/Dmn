/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * Unit test: dmn-test-dmesgnet-4.cpp
 *
 * Purpose
 * -------
 * This test verifies that two Dmn_DMesgNet instances can communicate over a
 * simulated network (using Dmn_Pipe and Dmn_Proc to forward serialized
 * messages). It checks:
 *  - discovery and system message exchange (node join, Ready state),
 *  - master selection (which node becomes master),
 *  - application-level message delivery via handlers, and
 *  - correct behavior when nodes are destroyed (node leaves and final
 *    destruction).
 *
 * Test setup and components
 * ------------------------
 * - Four Dmn_Pipe<std::string> objects emulate bidirectional socket pairs
 *   (read/write for each node).
 * - Two forwarding Dmn_Proc threads (dmesg1_to_dmesg2, dmesg2_to_dmesg1)
 *   continuously read serialized DMesgPb strings from one pipe, parse a copy
 *   into DMesgPb variables for assertions, and write the same serialized data
 *   into the opposite pipe. These act like simple network links between the
 *   two Dmn_DMesgNet instances.
 * - Two Dmn_DMesgNet instances ("dmesg1" then "dmesg2") are created and wired
 *   to the pipes; the test waits for discovery/handshake messages to be
 *   exchanged.
 *
 * Assertions and sequence
 * -----------------------
 * 1) After an initial discovery period, both nodes should reach the Ready
 *    state. "dmesg1" is expected to be the master; each node's system message
 *    should list the other node in its nodelist.
 *
 * 2) Application message delivery:
 *    - dmesg1 opens a handler and writes a DMesgPb with topic "counter sync".
 *    - dmesg2 opens a handler for the same topic and should receive the
 *      message via the simulated network.
 *
 * 3) Node teardown behavior:
 *    - Destroying dmesgnet1 should cause dmesg2 to be the remaining master and
 *      have an empty nodelist.
 *    - Destroying dmesgnet2 should result in the final sys state being
 *      Destroyed and the masteridentifier cleared.
 *
 * Notes and caveats
 * -----------------
 * - This test relies on sleeps to allow asynchronous discovery, message
 *   propagation and state transitions. That makes it timing-sensitive and it
 *   can be flaky on heavily loaded or slow CI hosts. If flakiness is observed,
 *   consider replacing sleeps with explicit synchronization (condition
 *   variables, polling with timeouts, or explicit handshakes).
 * - The forwarding procs capture the last-seen system and body messages into
 *   local DMesgPb variables that are later used for assertions.
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
  dmn::Dmn_Proc dmesg1_to_dmesg2{
      "dmesg1_to_dmesg2", [read_from_write_1, write_to_read_2, &dmesgpb1,
                           &dmesgpb1_body, &dmesgpb1_sys]() {
        while (true) {
          auto data = read_from_write_1->read();
          if (!data) {
            break;
          }

          dmesgpb1.ParseFromString(*data);
          if (dmesgpb1.type() == dmn::DMesgTypePb::sys) {
            dmesgpb1_sys = dmesgpb1;
          } else {
            dmesgpb1_body = dmesgpb1;
          }

          write_to_read_2->write(*data);
        }
      }};

  dmesg1_to_dmesg2.exec();

  auto dmesgnet1 = std::make_unique<dmn::Dmn_DMesgNet>(
      "dmesg1", std::move(read_1), std::move(write_1));

  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto dmesgnet2 = std::make_unique<dmn::Dmn_DMesgNet>(
      "dmesg2", std::move(read_2), std::move(write_2));

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

  auto dmesg_handle = dmesgnet1->openHandler("writeHandler");
  EXPECT_TRUE(dmesg_handle);
  dmesg_handle->write(dmesgpb);

  std::this_thread::sleep_for(std::chrono::seconds(10));
  EXPECT_TRUE(("counter sync" == dmesgpb1_body.topic()));
  EXPECT_TRUE((dmn::DMesgTypePb::message == dmesgpb1_body.type()));

  auto dmesgpbRead = dmesg_handleRead->read();
  EXPECT_TRUE((dmesgpbRead));
  EXPECT_TRUE(("counter sync" == dmesgpbRead->topic()));

  std::cout << "after read\n";

  std::this_thread::sleep_for(std::chrono::seconds(5));
  auto inConflict = dmesg_handleRead->isInConflict();
  EXPECT_TRUE((!inConflict));

  dmesgnet1 = {};
  std::cout << "after destroying 1\n";
  std::this_thread::sleep_for(std::chrono::seconds(10));

  return RUN_ALL_TESTS();
}
