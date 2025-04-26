/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that the subscriber can subscribe to
 * certain topic of the DMesg object.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "dmn-dmesg.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::DMesg dmesg{"dmesg"};

  std::vector<std::string> topics{"counter sync 1", "counter sync 2"};
  std::vector<std::string> subscribedTopics{"counter sync 1"};

  int cnt{0};
  std::shared_ptr<dmn::DMesg::DMesgHandler> dmesgHandler =
      dmesg.openHandler(subscribedTopics, "handler", false, nullptr,
                        [&cnt](const dmn::DMesgPb &msg) mutable {
                          EXPECT_TRUE("counter sync 1" == msg.topic());
                          cnt++;
                        });
  EXPECT_TRUE(dmesgHandler);

  auto dmesgWriteHandler = dmesg.openHandler("writeHandler");
  EXPECT_TRUE(dmesgWriteHandler);

  std::this_thread::sleep_for(std::chrono::seconds(3));

  for (int n = 0; n < 6; n++) {
    dmn::DMesgPb dmesgPb{};
    dmesgPb.set_topic(topics[n % 2]);
    dmesgPb.set_type(dmn::DMesgTypePb::message);

    std::string data{"Hello dmesg async"};
    dmn::DMesgBodyPb *dmsgbodyPb = dmesgPb.mutable_body();
    dmsgbodyPb->set_message(data);

    dmesgWriteHandler->write(dmesgPb);
  }

  std::this_thread::sleep_for(std::chrono::seconds(8));

  dmesg.closeHandler(dmesgWriteHandler);
  dmesg.closeHandler(dmesgHandler);
  EXPECT_TRUE(3 == cnt);

  return RUN_ALL_TESTS();
}
