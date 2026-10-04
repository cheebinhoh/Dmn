/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesgnet-7.cpp
 * @brief Unit test asserting that a local handler does not receive its own
 * write.
 *
 * This single-node test checks local self-delivery suppression. It does not
 * test delivery between multiple Dmn_DMesgNet nodes.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "dmn-dmesgnet.hpp"
#include "dmn-io.hpp"
#include "dmn-socket.hpp"

#include "proto/dmn-dmesg-body.pb.h"
#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  {
    std::shared_ptr<dmn::Dmn_Io<std::string>> readSocket1 =
        std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5001);
    std::shared_ptr<dmn::Dmn_Io<std::string>> writeSocket1 =
        std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000, true);

    bool readData{};
    dmn::DMesgPb msgPb{};
    dmn::Dmn_DMesgNet dmesgnet1{"dmesg-1", std::move(readSocket1),
                                std::move(writeSocket1)};
    readSocket1.reset();
    writeSocket1.reset();

    auto readHandler = dmesgnet1.openHandler(
        "dmesg-1-handler", nullptr,
        [&msgPb, &readData](dmn::DMesgPb data) mutable -> void {
          readData = true;
          msgPb = std::move(data);
        });

    dmn::DMesgPb dmesgpb{};
    dmesgpb.set_topic("counter sync");
    dmesgpb.set_type(dmn::DMesgTypePb::message);
    dmesgpb.set_sourceidentifier("handler-1");
    std::string data{"Hello dmesg async"};
    dmn::DMesgBodyPb *dmesgpb_body = dmesgpb.mutable_body();
    dmesgpb_body->set_message(data);

    readHandler->write(dmesgpb);

    std::this_thread::sleep_for(std::chrono::seconds(3));
    EXPECT_TRUE(!readData);

    dmesgnet1.closeHandler(readHandler);
  }

  google::protobuf::ShutdownProtobufLibrary();

  return RUN_ALL_TESTS();
}
