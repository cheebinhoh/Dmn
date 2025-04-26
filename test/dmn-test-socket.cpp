/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

#include "dmn-proc.hpp"
#include "dmn-socket.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Socket writeSocket{"127.0.0.1", 5000, true};
  std::unique_ptr<dmn::Socket> input =
      std::make_unique<dmn::Socket>("127.0.0.1", 5000);
  dmn::Io<std::string> *output{&writeSocket};

  std::string readData{};

  std::unique_ptr<dmn::Socket> inputHandle = std::move(input);
  dmn::Proc readProc{"readProc", [&inputHandle, &readData]() {
                       auto data = inputHandle->read();
                       if (data) {
                         readData = std::move_if_noexcept(*data);
                       }
                     }};

  std::string writeData{"hello socket"};
  dmn::Proc writeProc{"readProc",
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
