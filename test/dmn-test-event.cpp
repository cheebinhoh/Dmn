/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-event.cpp
 * @brief The unit test for dmn-event module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>

#include "dmn-async.hpp"
#include "dmn-event.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto inst = dmn::Dmn_Singleton::createInstance<dmn::Dmn_Event_Manager>();
  int count{};

  inst->registerSignalHandler(SIGALRM, [&inst, &count](int signno) {
    std::cout << "handle signal " << signno << "\n";
    if (count >= 2) {
      inst->exitMainLoop();
    } else {
      count++;
      alarm(5);
    }
  });

  alarm(5);
  inst->enterMainLoop();

  EXPECT_TRUE(2 == count);

  return RUN_ALL_TESTS();
}
