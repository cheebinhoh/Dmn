/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * Integration-style unit test for Dmn_DMesgNet.
 *
 * This test verifies a small peer-discovery / messaging scenario between two
 * Dmn_DMesgNet instances using in-memory pipes instead of real network sockets.
 *
 * Scenario and intent:
 *  - Two in-memory Dmn_Pipe<string> channels are created to simulate two ends
 *    of a network link.
 *  - Two forwarding Dmn_Proc tasks shuttle protobuf-encoded DMesg messages
 *    between the pipes so that the two Dmn_DMesgNet objects can communicate.
 *  - dmesgnet1 is created first, then dmesgnet2 is created after a short delay.
 *  - After allowing time for discovery and handshakes, the test asserts:
 *      * both nodes reach the Ready state
 *      * both nodes see the other in their nodelist
 *      * the masteridentifier fields are set appropriately
 *  - dmesgnet1 is destroyed and, after a delay, the test asserts:
 *      * dmesgnet2 remains Ready and becomes the master (its masteridentifier)
 *      * dmesgnet2's nodelist no longer contains dmesg1
 *  - dmesgnet2 is destroyed and, after a final delay, the test asserts:
 *      * the last-known state becomes Destroyed and masteridentifier is cleared
 *
 * Notes:
 *  - Timing in this test is driven by std::this_thread::sleep_for to allow
 *    background threads and message propagation time; these values are
 *    intentionally generous to reduce flakiness in CI.
 *  - The test operates on protobuf DMesg messages (DMesgPb). It inspects
 *    the parsed message bodies for state, masteridentifier, and nodelist
 *    contents to validate behavior.
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
  dmn::Dmn_Proc dmesg1_to_dmesg2{
      "dmesg1_to_dmesg2", [read_from_write_1, write_to_read_2, &dmesgpb1]() {
        while (true) {
          auto data = read_from_write_1->read();
          if (!data) {
            break;
          }

          dmesgpb1.ParseFromString(*data);
          //       std::cout << "dmesg1 to dmesg2: " <<
          //       dmesgpb1.ShortDebugString()
          //                 << "\n";

          write_to_read_2->write(*data);
        }
      }};

  dmn::DMesgPb dmesgpb2{};
  dmn::Dmn_Proc dmesg2_to_dmesg1{
      "dmesg2_to_dmesg1", [read_from_write_2, write_to_read_1, &dmesgpb2]() {
        while (true) {
          auto data = read_from_write_2->read();
          if (!data) {
            break;
          }

          dmesgpb2.ParseFromString(*data);
          //       std::cout << "dmesg2 to dmesg1: " <<
          //       dmesgpb2.ShortDebugString()
          //                 << "\n";

          write_to_read_1->write(*data);
        }
      }};

  dmesg1_to_dmesg2.exec();
  dmesg2_to_dmesg1.exec();

  auto dmesgnet1 = std::make_unique<dmn::Dmn_DMesgNet>(
      "dmesg1", std::move(read_1), std::move(write_1));

  std::this_thread::sleep_for(std::chrono::seconds(2));

  auto dmesgnet2 = std::make_unique<dmn::Dmn_DMesgNet>(
      "dmesg2", std::move(read_2), std::move(write_2));

  std::this_thread::sleep_for(std::chrono::seconds(15));
  auto sys1 = dmesgpb1.body().sys().self();
  auto sys2 = dmesgpb2.body().sys().self();

  EXPECT_TRUE((sys1.state() == dmn::DMesgStatePb::Ready));
  EXPECT_TRUE((sys2.state() == dmn::DMesgStatePb::Ready));
  EXPECT_TRUE(("dmesg1" == sys1.masteridentifier()));
  EXPECT_TRUE(("dmesg1" == sys2.masteridentifier()));
  EXPECT_TRUE((1 == dmesgpb1.body().sys().nodelist().size()));
  EXPECT_TRUE(
      ("dmesg2" == dmesgpb1.body().sys().nodelist().Get(0).identifier()));
  EXPECT_TRUE((1 == dmesgpb2.body().sys().nodelist().size()));
  EXPECT_TRUE(
      ("dmesg1" == dmesgpb2.body().sys().nodelist().Get(0).identifier()));

  dmesgnet1 = {};
  std::cout << "after destroying 1\n";
  std::this_thread::sleep_for(std::chrono::seconds(10));

  sys2 = dmesgpb2.body().sys().self();

  EXPECT_TRUE((sys2.state() == dmn::DMesgStatePb::Ready));
  EXPECT_TRUE(("dmesg2" == sys2.masteridentifier()));
  EXPECT_TRUE((0 == dmesgpb2.body().sys().nodelist().size()));

  dmesgnet2 = {};
  std::cout << "after destroying 2\n";
  std::this_thread::sleep_for(std::chrono::seconds(10));

  sys2 = dmesgpb2.body().sys().self();

  EXPECT_TRUE((sys2.state() == dmn::DMesgStatePb::Destroyed));
  EXPECT_TRUE(("" == sys2.masteridentifier()));
  EXPECT_TRUE((0 == dmesgpb2.body().sys().nodelist().size()));

  return RUN_ALL_TESTS();
}
