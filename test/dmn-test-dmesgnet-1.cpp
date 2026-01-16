/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * dmn-test-dmesgnet-1.cpp
 *
 * Purpose
 * -------
 * This unit test verifies the lifecycle and system-message behavior of a
 * single Dmn_DMesgNet instance when connected to in-process I/O endpoints
 * (Dmn_Pipe). Concretely, it checks that:
 *   - The Dmn_DMesgNet emits system messages of type `sys`.
 *   - The instance announces the expected state transitions in order:
 *       MasterPending -> Ready
 *   - When Ready is observed, the message identifies this instance as the
 *     master using the configured identifier ("dmesg1").
 *   - After the Dmn_DMesgNet object is destroyed, a final `Destroyed`
 *     system state is emitted.
 *
 * Test strategy
 * -------------
 * - Construct two Dmn_Pipe<std::string> objects and use them as the read
 *   and write endpoints for Dmn_DMesgNet. These pipes simulate a local
 *   socket-like transport, allowing the test to observe messages emitted
 *   by the Dmn_DMesgNet.
 * - Create a Dmn_DMesgNet named "dmesg1" with the pipe endpoints.
 * - Sleep briefly to allow asynchronous startup and message emission.
 * - Read messages from the pipe, parse them as dmn::DMesgPb protobuf
 *   messages and assert:
 *     - Each observed message is of type `sys`.
 *     - A `MasterPending` system state appears before a `Ready` state.
 *     - The `Ready` state's masteridentifier equals "dmesg1".
 * - Destroy the Dmn_DMesgNet instance, read remaining messages, and assert
 *   that the final system state observed is `Destroyed`.
 *
 * Important notes and suggestions
 * ------------------------------
 * - Timing sensitivity: this test relies on sleeps and read timeouts. On
 *   slower or heavily-loaded CI runners these timings may need to be
 *   increased to reduce flakiness. If flaky, increase the initial
 *   sleep (currently 10s) or the read timeouts passed to read().
 * - Determinism: for more deterministic behavior consider replacing sleeps
 *   with explicit synchronization primitives or hooks in Dmn_DMesgNet to
 *   report readiness in tests.
 * - Message ordering: the test assumes messages are delivered in the order
 *   emitted by Dmn_DMesgNet. If the transport or implementation buffers
 *   messages differently, adjust assertions accordingly.
 * - Protobuf enums: the test uses dmn::DMesgTypePb and dmn::DMesgStatePb
 *   defined in proto/dmn-dmesg*.pb.h. The expected states are:
 *     MasterPending, Ready, Destroyed
 *
 * Expected outcome
 * ----------------
 * The reader should observe the system-state sequence:
 *   MasterPending -> Ready (masteridentifier == "dmesg1") -> Destroyed
 *
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

  std::this_thread::sleep_for(std::chrono::seconds(10));

  bool masterpending{};
  bool ready{};

  auto dataList = read_from_write_1->read(50, 10000000L);
  for (auto &data : dataList) {
    dmn::DMesgPb dmesgpb{};

    dmesgpb.ParseFromString(data);

    EXPECT_TRUE((dmesgpb.type() == dmn::DMesgTypePb::sys));

    auto sys = dmesgpb.body().sys().self();
    if (sys.state() == dmn::DMesgStatePb::MasterPending) {
      EXPECT_TRUE((!ready));
      masterpending = true;
    } else if (sys.state() == dmn::DMesgStatePb::Ready) {
      EXPECT_TRUE((masterpending));
      EXPECT_TRUE(("dmesg1" == sys.masteridentifier()));
      ready = true;
    }
  }

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
