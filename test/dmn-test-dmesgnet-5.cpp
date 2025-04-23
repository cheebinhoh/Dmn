/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that when two Dmn_DMesgNet objects
 * are existence at the same time, the first created object
 * is the master, and when the first object exits, the 2nd object
 * becomes master.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <thread>

#include "dmn-dmesgnet.hpp"
#include "dmn-socket.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::unique_ptr<dmn::Dmn_Io<std::string>> readSocket1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5001);
  std::unique_ptr<dmn::Dmn_Io<std::string>> writeSocket1 =
      std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000, true);

  dmn::DMesgPb sysPb_3{};
  std::unique_ptr<dmn::Dmn_DMesgNet> dmesgnet1 =
      std::make_unique<dmn::Dmn_DMesgNet>("dmesg-3", std::move(readSocket1),
                                          std::move(writeSocket1));
  readSocket1.reset();
  writeSocket1.reset();

  auto listenHandler3 = dmesgnet1->openHandler(
      "dmesg-3-listen", true, nullptr, [&sysPb_3](dmn::DMesgPb data) mutable {
        if (data.type() == dmn::DMesgTypePb::sys) {
          sysPb_3 = data;
        }
      });

  dmn::DMesgPb sysPb_4{};

  std::this_thread::sleep_for(std::chrono::seconds(3));
  dmn::Dmn_Proc dmesg_4_Proc{
      "dmesg_4_Proc", [&sysPb_4]() mutable {
        std::unique_ptr<dmn::Dmn_Io<std::string>> readSocket1 =
            std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5000);
        std::unique_ptr<dmn::Dmn_Io<std::string>> writeSocket1 =
            std::make_unique<dmn::Dmn_Socket>("127.0.0.1", 5001, true);

        dmn::Dmn_DMesgNet dmesgnet1{"dmesg-4", std::move(readSocket1),
                                    std::move(writeSocket1)};
        readSocket1.reset();
        writeSocket1.reset();

        auto listenHandler4 =
            dmesgnet1.openHandler("dmesg-4-listen", true, nullptr,
                                  [&sysPb_4](dmn::DMesgPb data) mutable {
                                    if (data.type() == dmn::DMesgTypePb::sys) {
                                      sysPb_4 = data;
                                    }
                                  });

        std::this_thread::sleep_for(std::chrono::seconds(10));

        EXPECT_TRUE(sysPb_4.body().sys().self().identifier() == "dmesg-4");
        EXPECT_TRUE(sysPb_4.body().sys().self().masteridentifier() ==
                    "dmesg-3");

        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::cout << "exit dmesg_4_proc\n";

        dmesgnet1.closeHandler(listenHandler4);
        std::cout << "end dmesg4_Proc\n";
      }};

  dmesg_4_Proc.exec();

  std::this_thread::sleep_for(std::chrono::seconds(15));

  dmesgnet1->closeHandler(listenHandler3);
  std::cout << "close listenHandler3\n";
  dmesgnet1 = {};

  std::cout << "before wait for dmesg_4_Proc to end\n";
  dmesg_4_Proc.wait();
  EXPECT_TRUE(sysPb_4.body().sys().self().identifier() == "dmesg-4");
  EXPECT_TRUE(sysPb_4.body().sys().self().masteridentifier() == "dmesg-4");

  return RUN_ALL_TESTS();
}
