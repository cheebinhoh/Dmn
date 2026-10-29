/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dbus-runtime.hpp
 * @brief Internal process-wide libdbus initialization.
 *
 * Architecture: two independent D-Bus stacks on one runtime
 * ---------------------------------------------------------
 * Every libdbus user in Dmn calls initializeDbusRuntime() first. Above this
 * shared layer sit two stacks that do not depend on each other:
 *
 *   Dmn_DMesgNet (dmn-dmesgnet-dbus.cpp)    Dmn_BluezClient, GATT client,
 *            |                              ANCS monitor
 *   Dmn_DbusInput / Dmn_DbusOutput                  |
 *   (dmn-dbus-io.hpp, dmn-dbus-config.hpp)   Dmn_DbusClient
 *            |                              (dmn-dbus-client.hpp)
 *            +---------------+---------------+
 *                    dmn-dbus-runtime.hpp
 *                            |
 *                         libdbus
 *
 * - Dmn_DbusInput/Dmn_DbusOutput are Dmn's own message transport: opaque byte
 *   payloads broadcast as one signal tuple (default path /org/dmn/DMesg1,
 *   interface org.dmn.DMesg1.Transport, member Message, body `ay`) routed by
 *   the bus daemon between matching Dmn participants. Only Dmn_DMesgNet uses
 *   them.
 * - Dmn_DbusClient is a general D-Bus client for method calls, property
 *   access and signal matching against other services. The BlueZ and GATT
 *   clients use it to talk to org.bluez (org.freedesktop.DBus.Properties,
 *   ObjectManager, org.bluez.Adapter1/Device1/GattService1/
 *   GattCharacteristic1, PropertiesChanged, InterfacesAdded). BlueZ traffic
 *   never uses the Dmn signal tuple, and Dmn_DbusClient does not use
 *   Dmn_DbusInput/Dmn_DbusOutput.
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
