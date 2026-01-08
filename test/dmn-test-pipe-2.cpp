/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <thread>

#include "dmn-pipe.hpp"
#include "dmn-proc.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  bool readDoneOnce{};
  int cnt{};
  dmn::Dmn_Pipe<int> pipe{"pipe",
                          [&cnt, &readDoneOnce](int val) {
                            EXPECT_TRUE(val == cnt);

                            cnt++;
                            readDoneOnce = true;
                          },
                          3};

  dmn::Dmn_Proc::yield();

  pipe.write(0);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(!readDoneOnce);

  pipe.write(1);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(!readDoneOnce);

  pipe.write(2);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(readDoneOnce);

  std::cout << "Before wait for empty\n";
  pipe.waitForEmpty();
  EXPECT_TRUE(3 == cnt);

  return RUN_ALL_TESTS();
}
