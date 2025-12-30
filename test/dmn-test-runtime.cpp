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
#include "dmn-runtime.hpp"

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  std::random_device rd;
  std::mt19937 gen(rd());

  // 2. Define the range [1, 1000]
  std::uniform_int_distribution<> distr(100, 1000);

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

  bool highRun{};
  dmn::Dmn_Proc procHigh{
      "highjob", [&inst, &highRun, &distr, &gen]() {
        std::cout << "procHigh to add job\n";

        for (int i = 0; i < 2; i++) {
          inst->addJob(dmn::Dmn_Runtime_Job::kHigh, [&highRun]() {
            std::cout << "** high job\n";
            highRun = true;
          });

          int random_ms = distr(gen);
          dmn::Dmn_Proc::yield();
          std::this_thread::sleep_for(std::chrono::milliseconds(random_ms));
        }

        std::cout << "after procHigh to add job\n";
      }};

  bool lowRun{};
  dmn::Dmn_Proc procLow{
      "lowjob", [&inst, &lowRun, &distr, &gen]() {
        std::cout << "procLow to add job\n";

        for (int i = 0; i < 2; i++) {
          inst->addJob(dmn::Dmn_Runtime_Job::kLow, [&lowRun]() {
            std::cout << "** low job\n";
            lowRun = true;
          });

          int random_ms = distr(gen);
          dmn::Dmn_Proc::yield();
          std::this_thread::sleep_for(std::chrono::milliseconds(random_ms));
        }

        std::cout << "after procLow to add job\n";
      }};

  bool mediumRun{};
  dmn::Dmn_Proc procMedium{
      "mediumjob", [&inst, &mediumRun, &distr, &gen]() {
        std::cout << "procMedium to add job\n";

        for (int i = 0; i < 2; i++) {
          inst->addJob(dmn::Dmn_Runtime_Job::kMedium, [&mediumRun]() {
            std::cout << "** medium job\n";
            mediumRun = true;
          });

          int random_ms = distr(gen);
          dmn::Dmn_Proc::yield();
          std::this_thread::sleep_for(std::chrono::milliseconds(random_ms));
        }

        std::cout << "after procMedium to add job\n";
      }};
  procHigh.exec();
  procLow.exec();
  procMedium.exec();

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
