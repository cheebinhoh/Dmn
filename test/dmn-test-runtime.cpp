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

  // 2. Define the range [100, 500]
  std::uniform_int_distribution<> distr(100, 500);

  auto inst = dmn::Dmn_Singleton::createInstance<dmn::Dmn_Runtime_Manager>();
  int count{};

  dmn::Dmn_Runtime_Manager::RuntimeJobFncType timedJob{};
  timedJob = [&inst, &count, &timedJob](const dmn::Dmn_Runtime_Job &j) -> void {
    std::cout << "********* handle timerjob: " << count << "\n";
    if (count >= 5) {
      inst->exitMainLoop();
    } else {
      count++;
      inst->addTimedJob(timedJob, std::chrono::seconds(5),
                        dmn::Dmn_Runtime_Job::kHigh);
    }
  };

  inst->addTimedJob(timedJob, std::chrono::seconds(5),
                    dmn::Dmn_Runtime_Job::kHigh);

  int highRun{};
  int mediumRun{};
  int lowRun{};

  dmn::Dmn_Proc procHigh{
      "highjob", [&inst, &highRun, &distr, &gen, &mediumRun, &lowRun]() {
        std::cout << "procHigh to add job\n";

        for (int i = 0; i < 2; i++) {
          inst->addJob(
              [&highRun, &mediumRun, &lowRun](const dmn::Dmn_Runtime_Job &j) {
                EXPECT_TRUE(0 == lowRun);
                EXPECT_TRUE(0 == mediumRun);

                std::cout << "** high job\n";
                highRun++;
                std::this_thread::sleep_for(std::chrono::seconds(1));
              },
              dmn::Dmn_Runtime_Job::kHigh);

          int random_ms = distr(gen);
          dmn::Dmn_Proc::yield();
          std::this_thread::sleep_for(std::chrono::milliseconds(random_ms));
        }

        std::cout << "after procHigh to add job\n";
      }};

  dmn::Dmn_Proc procLow{
      "lowjob", [&inst, &lowRun, &distr, &gen, &highRun, &mediumRun]() {
        std::cout << "procLow to add job\n";

        for (int i = 0; i < 2; i++) {
          inst->addJob(
              [&lowRun, &highRun, &mediumRun](const dmn::Dmn_Runtime_Job &j) {
                EXPECT_TRUE(2 == highRun);
                EXPECT_TRUE(2 == mediumRun);
                std::cout << "** low job\n";
                lowRun++;
                std::this_thread::sleep_for(std::chrono::seconds(3));
              },
              dmn::Dmn_Runtime_Job::kLow);

          int random_ms = distr(gen);
          dmn::Dmn_Proc::yield();
          std::this_thread::sleep_for(std::chrono::milliseconds(random_ms));
        }

        std::cout << "after procLow to add job\n";
      }};

  dmn::Dmn_Proc procMedium{
      "mediumjob", [&inst, &mediumRun, &distr, &gen, &highRun, &lowRun]() {
        std::cout << "procMedium to add job\n";

        for (int i = 0; i < 2; i++) {
          inst->addJob(
              [&mediumRun, &highRun, &lowRun](const dmn::Dmn_Runtime_Job &j) {
                EXPECT_TRUE(2 == highRun);
                EXPECT_TRUE(0 == lowRun);
                std::cout << "** medium job\n";
                mediumRun++;
                std::this_thread::sleep_for(std::chrono::seconds(2));
              },
              dmn::Dmn_Runtime_Job::kMedium);

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

  // alarm(5);
  inst->enterMainLoop();

  EXPECT_TRUE(2 == lowRun);
  EXPECT_TRUE(2 == mediumRun);
  EXPECT_TRUE(2 == highRun);
  EXPECT_TRUE(5 == count);

  return RUN_ALL_TESTS();
}
