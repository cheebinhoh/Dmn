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
