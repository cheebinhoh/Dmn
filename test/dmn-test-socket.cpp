/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include "dmn-proc.hpp"
#include "dmn-socket.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  Dmn::Dmn_Socket writeSocket{"127.0.0.1", 5000, true};
  std::unique_ptr<Dmn::Dmn_Socket> input =
      std::make_unique<Dmn::Dmn_Socket>("127.0.0.1", 5000);
  Dmn::Dmn_Io<std::string> *output{&writeSocket};

  std::string readData{};

  std::unique_ptr<Dmn::Dmn_Socket> inputHandle = std::move(input);
  Dmn::Dmn_Proc readProc{"readProc", [&inputHandle, &readData]() {
                           auto data = inputHandle->read();
                           if (data) {
                             readData = std::move_if_noexcept(*data);
                           }
                         }};

  std::string writeData{"hello socket"};
  Dmn::Dmn_Proc writeProc{"readProc",
                          [output, &writeData]() { output->write(writeData); }};

  readProc.exec();
  std::this_thread::sleep_for(std::chrono::seconds(2));

  writeProc.exec();
  std::this_thread::sleep_for(std::chrono::seconds(2));

  writeProc.wait();
  readProc.wait();

  EXPECT_TRUE(writeData == readData);

  return RUN_ALL_TESTS();
}
