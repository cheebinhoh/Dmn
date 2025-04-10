/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * This is the dmn daemon that runs as after a system bootup,
 * and receive and read, write and process message from network
 * of distributed dmn nodes.
 */

#include "dmn.hpp"

#include <iostream>
#include <thread>

int main(int argc, char *argv[]) {
  auto inst = Dmn::Dmn_Singleton::createInstance<Dmn::Dmn_Event_Manager>();

  // for TESTING
  Dmn::Dmn_Proc proc{
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
