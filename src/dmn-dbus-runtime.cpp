/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-runtime.cpp
 * @brief Shared process-wide libdbus initialization.
 */

#include "dmn-dbus-runtime.hpp"

#include <dbus/dbus.h>

#include <mutex>
#include <stdexcept>

namespace dmn::detail {

void initializeDbusRuntime() {
  static std::once_flag initialized;
  static bool success{};

  std::call_once(initialized, [] { success = dbus_threads_init_default(); });
  if (!success) {
    throw std::runtime_error("Dmn_Dbus: libdbus thread initialization failed");
  }
}

} // namespace dmn::detail
