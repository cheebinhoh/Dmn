/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that the Dmn_DMesg class support
 * that one publisher and one subscriber are able to continuing
 * read and write multiple messages between them.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <thread>

#include "dmn-dmesg.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_DMesg dmesg{"dmesg"};

  auto dmesgHandler1 = dmesg.openHandler("handler1");
  EXPECT_TRUE(dmesgHandler1);

  auto dmesgHandler2 = dmesg.openHandler("handler2");
  EXPECT_TRUE(dmesgHandler2);

  dmn::Dmn_Proc proc1{
      "proc1", [&dmesgHandler1]() {
        int valCheck{1};

        while (true) {
          auto dmesgpb = dmesgHandler1->read();
          if (!dmesgpb) {
            break;
          }

          assert(dmesgpb->type() == dmn::DMesgTypePb::message);
          if (dmesgpb->body().message() == "") {
            break;
          }

          std::cout << "proc1: message: " << dmesgpb->body().message() << "\n";

          std::stringstream is{dmesgpb->body().message()};
          int val{};

          is >> val;
          EXPECT_TRUE(!is.fail());
          EXPECT_TRUE(val == valCheck);

          val++;

          std::stringstream os;
          os << val;

          dmn::DMesgPb dmesgpb_ret{};
          dmesgpb_ret.set_topic("counter sync");
          dmesgpb_ret.set_type(dmn::DMesgTypePb::message);

          dmn::DMesgBodyPb *dmsgbodyPbRet = dmesgpb_ret.mutable_body();
          dmsgbodyPbRet->set_message(os.str());

          dmesgHandler1->write(dmesgpb_ret);

          valCheck += 2;
        }
      }};

  dmn::Dmn_Proc proc2{
      "proc2", [&dmesgHandler2]() {
        int val{1};

        while (true) {
          std::stringstream os{};

          if (val <= 10) {
            os << val;
          } else {
            os << "";
          }

          dmn::DMesgPb dmesgpb{};
          dmesgpb.set_topic("counter sync");
          dmesgpb.set_type(dmn::DMesgTypePb::message);

          dmn::DMesgBodyPb *dmsgbodyPb = dmesgpb.mutable_body();
          dmsgbodyPb->set_message(os.str());

          dmesgHandler2->write(dmesgpb);

          if (os.str() == "") {
            break;
          }

          auto dmesgpb_ret = dmesgHandler2->read();
          if (!dmesgpb_ret) {
            break;
          }

          assert(dmesgpb_ret->type() == dmn::DMesgTypePb::message);
          assert(dmesgpb_ret->body().message() != "");

          std::cout << "proc2: message: " << dmesgpb_ret->body().message()
                    << "\n";

          int val_ret{};

          std::stringstream is{dmesgpb_ret->body().message()};
          is >> val_ret;

          EXPECT_TRUE(val_ret == (val + 1));
          val += 2;
        }
      }};

  proc2.exec();
  proc1.exec();

  proc2.wait();
  proc1.wait();

  dmesg.closeHandler(dmesgHandler1);
  dmesg.closeHandler(dmesgHandler2);

  return RUN_ALL_TESTS();
}
