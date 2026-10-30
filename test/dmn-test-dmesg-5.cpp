/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesg-5.cpp
 * @brief The unit test that asserts that the dmn-dmesg' subscriber can
 * subscribe to certain topic of the Dmn_DMesg object.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "dmn-dmesg.hpp"

#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg.pb.h"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  {
    dmn::Dmn_DMesg dmesg{"dmesg"};

    std::vector<std::string> topics{"counter sync 1", "counter sync 2"};
    const std::string subscribed_topic{"counter sync 1"};

    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    int callback_count{0};
    auto dmesg_handler{
        dmesg.openHandler("handler", subscribed_topic, nullptr,
                          [&callback_mutex, &callback_cv,
                           &callback_count](const dmn::DMesgPb &msg) -> void {
                            EXPECT_TRUE("counter sync 1" == msg.topic());
                            {
                              std::lock_guard lock{callback_mutex};
                              callback_count++;
                            }
                            callback_cv.notify_one();
                          })};
    EXPECT_TRUE(dmesg_handler);

    auto dmesg_write_handler1{dmesg.openHandler("writeHandler", topics[0])};
    EXPECT_TRUE(dmesg_write_handler1);

    auto dmesg_write_handler2{dmesg.openHandler("writeHandler", topics[1])};
    EXPECT_TRUE(dmesg_write_handler2);

    for (int n = 0; n < 6; n++) {
      dmn::DMesgPb dmesgpb{};
      dmesgpb.set_type(dmn::DMesgTypePb::message);

      std::string data{"Hello dmesg async"};
      dmn::DMesgBodyPb *dmesgpb_body = dmesgpb.mutable_body();
      dmesgpb_body->set_message(data);

      if ((n % 2) == 0) {
        dmesg_write_handler1->write(dmesgpb);
      } else {
        dmesg_write_handler2->write(dmesgpb);
      }
    }

    std::unique_lock lock{callback_mutex};
    auto callbacks_completed{
        callback_cv.wait_for(lock, std::chrono::seconds(30), [&callback_count] {
          return callback_count >= 3;
        })};
    EXPECT_TRUE(callbacks_completed);
    lock.unlock();

    dmesg.closeHandler(dmesg_write_handler2);
    dmesg.closeHandler(dmesg_write_handler1);
    dmesg.closeHandler(dmesg_handler);
    EXPECT_EQ(callback_count, 3);
  }

  google::protobuf::ShutdownProtobufLibrary();

  return RUN_ALL_TESTS();
}
