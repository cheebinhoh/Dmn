/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesg-daemon.cpp
 * @brief Entry point for the DMesg daemon process.
 *
 * This daemon is intended to run after system boot and provides a
 * Dmn_Runtime_Manager main loop that can receive, process and forward
 * messages across a network of distributed DMesg nodes.
 *
 * The current implementation contains a minimal test harness that
 * sleeps for 10 seconds and then overrides the SIGTERM handler to
 * demonstrate dynamic signal handler registration. In a production
 * deployment this stub would be replaced with the full DMesgNet
 * initialisation and I/O handler wiring.
 */

#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "dmn.hpp"

int main(int argc, char *argv[]) {
  auto inst = dmn::Dmn_Runtime_Manager::createInstance();

  // for TESTING
  dmn::Dmn_Proc proc{
      "exitMainLoop", [&inst]() {
        DMN_DEBUG_PRINT(
            std::cout
            << "sleep 10 seconds before setting handler for SIGTERM\n");
        std::this_thread::sleep_for(std::chrono::seconds(10));
        DMN_DEBUG_PRINT(std::cout << "set signal handler to respond to "
                                  << SIGTERM << "\n");

        inst->registerSignalHandler(
            SIGTERM, [&inst](int signo) { std::cout << "handling SIGTERM\n"; });
      }};

  proc.exec();

  inst->enterMainLoop();

  return 0;
}
