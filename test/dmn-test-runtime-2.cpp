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

  auto inst = dmn::Dmn_Runtime_Manager::createInstance();

  dmn::Dmn_Runtime_Job::FncType timedJob{};
  timedJob = [&inst](const auto &) -> dmn::Dmn_Runtime_Task {
    std::cout << "exit main loop after 25seconds\n";
    inst->exitMainLoop();

    co_return;
  };

  inst->addTimedJob(timedJob, std::chrono::seconds(30),
                    dmn::Dmn_Runtime_Job::Priority::kHigh);

  int lowCount{};
  int midCount{};
  int highCount{};

  dmn::Dmn_Proc proc{
      "run low priority job",
      [&inst, &lowCount, &midCount, &highCount]() -> void {
        inst->addJob(
            [&inst, &lowCount, &midCount, &highCount](
                [[maybe_unused]] const auto &j) -> dmn::Dmn_Runtime_Task {
              for (int count = 0; count < 15; count++) {
                if (5 == count) {
                  inst->addJob(
                      [&inst, &lowCount, &midCount,
                       &highCount]([[maybe_unused]] const auto &j)
                          -> dmn::Dmn_Runtime_Task {
                        EXPECT_TRUE((8 == lowCount));

                        for (int count = 0; count < 5; count++) {
                          if (1 == count) {
                            inst->addJob(
                                [&lowCount, &midCount, &highCount](
                                    const auto &) -> dmn::Dmn_Runtime_Task {
                                  EXPECT_TRUE((8 == lowCount));
                                  EXPECT_TRUE((3 == midCount));

                                  for (int count = 0; count < 3; count++) {
                                    std::cout
                                        << "*** high job: " << count
                                        << ", pt: " << (void *)pthread_self()
                                        << "\n";
                                    highCount++;
                                    std::this_thread::sleep_for(
                                        std::chrono::seconds(1));
                                  }

                                  co_return;
                                },
                                dmn::Dmn_Runtime_Job::Priority::kHigh);
                          } else if (3 == count) {
                            EXPECT_TRUE((0 == highCount));
                            co_await std::suspend_always{};
                            EXPECT_TRUE((3 == highCount));
                          }

                          std::cout << "*** medium job: " << count
                                    << ", pt: " << (void *)pthread_self()
                                    << "\n";

                          midCount++;
                          std::this_thread::sleep_for(std::chrono::seconds(1));
                        }

                        co_return;
                      },
                      dmn::Dmn_Runtime_Job::Priority::kMedium);
                } else if (8 == count) {
                  EXPECT_TRUE((0 == midCount));
                  co_await std::suspend_always{};
                  EXPECT_TRUE((5 == midCount));
                }

                std::cout << "*** low job: " << count
                          << ", pt: " << (void *)pthread_self() << "\n";
                lowCount++;

                std::this_thread::sleep_for(std::chrono::seconds(1));
              }

              co_return;
            },
            dmn::Dmn_Runtime_Job::Priority::kLow);
      }};

  proc.exec();

  inst->enterMainLoop();
  EXPECT_TRUE((15 == lowCount));
  EXPECT_TRUE((5 == midCount));
  EXPECT_TRUE((3 == highCount));

  return RUN_ALL_TESTS();
}
