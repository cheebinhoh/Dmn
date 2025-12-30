/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-runtime.cpp
 * @brief The unit test for dmn-runtime module.
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
#include "dmn-runtime.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto inst = dmn::Dmn_Singleton::createInstance<dmn::Dmn_Runtime_Manager>();
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

  bool lowRun{};
  dmn::Dmn_Proc procLow{"lowjob", [&inst, &lowRun]() {
                          std::cout << "procLow to add job\n";
                          inst->addJob(dmn::Dmn_Runtime_Job::kLow, [&lowRun]() {
                            std::cout << "** low job\n";
                            lowRun = true;
                          });
                          std::cout << "after procLow to add job\n";
                        }};
  procLow.exec();

  bool mediumRun{};
  dmn::Dmn_Proc procMedium{"mediumjob", [&inst, &mediumRun]() {
                             std::cout << "procMedium to add job\n";
                             inst->addJob(dmn::Dmn_Runtime_Job::kMedium,
                                          [&mediumRun]() {
                                            std::cout << "** medium job\n";
                                            mediumRun = true;
                                          });
                             std::cout << "after procMedium to add job\n";
                           }};
  procMedium.exec();

  bool highRun{};
  dmn::Dmn_Proc procHigh{"highjob", [&inst, &highRun]() {
                           std::cout << "procHigh to add job\n";
                           inst->addJob(dmn::Dmn_Runtime_Job::kHigh,
                                        [&highRun]() {
                                          std::cout << "** high job\n";
                                          highRun = true;
                                        });
                           std::cout << "after procHigh to add job\n";
                         }};
  procHigh.exec();

  std::cout << "wait for 5 seconds\n";
  std::this_thread::sleep_for(std::chrono::seconds(5));
  std::cout << "after for 5 seconds\n";

  alarm(5);
  inst->enterMainLoop();

  EXPECT_TRUE(lowRun);
  EXPECT_TRUE(mediumRun);
  EXPECT_TRUE(highRun);
  EXPECT_TRUE(2 == count);

  return RUN_ALL_TESTS();
}
