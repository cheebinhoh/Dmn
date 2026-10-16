/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-runtime.hpp
 * @brief Internal process-wide libdbus initialization.
 */

#ifndef DMN_DBUS_RUNTIME_HPP_
#define DMN_DBUS_RUNTIME_HPP_

namespace dmn::detail {

/**
 * @brief Initialize libdbus threading once for this process.
 *
 * Call before using any other libdbus API. This setup is independent of
 * whether clients later connect to the standard system or session bus, or use
 * an explicit address for either one or a separately started private bus.
 */
void initializeDbusRuntime();

} // namespace dmn::detail

#endif // DMN_DBUS_RUNTIME_HPP_
