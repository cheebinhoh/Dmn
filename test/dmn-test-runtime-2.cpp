/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-runtime.cpp
 * @brief The unit test for dmn-runtime module.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>

#include "dmn-async.hpp"
#include "dmn-proc.hpp"
#include "dmn-runtime.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto inst = dmn::Dmn_Singleton::createInstance<dmn::Dmn_Runtime_Manager>();

  dmn::Dmn_Runtime_Manager::RuntimeJobFncType timedJob{};
  timedJob = [&inst]() -> void {
    std::cout << "exit main loop after 25seconds\n";
    inst->exitMainLoop();
  };

  inst->addTimedJob(timedJob, std::chrono::seconds(30),
                    dmn::Dmn_Runtime_Job::kHigh);

  volatile int lowCount{};
  volatile int midCount{};

  dmn::Dmn_Proc proc{
      "run low priority job", [&inst, &lowCount, &midCount]() -> void {
        inst->addJob(
            [&inst, &lowCount, &midCount]() -> void {
              for (int count = 0; count < 15; count++) {
                if (5 == count) {
                  inst->addJob(
                      [&midCount]() -> void {
                        for (int count = 0; count < 5; count++) {
                          std::cout << "*** medium job: " << count
                                    << ", pt: " << (void *)pthread_self()
                                    << "\n";
                          midCount++;
                          std::this_thread::sleep_for(std::chrono::seconds(1));
                        }
                      },
                      dmn::Dmn_Runtime_Job::kMedium);
                } else if (8 == count) {
                  inst->yield();
                }

                std::cout << "*** low job: " << count
                          << ", pt: " << (void *)pthread_self() << "\n";
                lowCount++;

                std::this_thread::sleep_for(std::chrono::seconds(1));
              }
            },
            dmn::Dmn_Runtime_Job::kLow);
      }};

  proc.exec();

  inst->enterMainLoop();
  EXPECT_TRUE((15 == lowCount));
  EXPECT_TRUE((5 == midCount));

  return RUN_ALL_TESTS();
}
