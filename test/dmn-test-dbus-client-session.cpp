/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dbus-client-session.cpp
 * @brief Isolated session-bus selection tests for the generic D-Bus client.
 */

#include "dmn-dbus-client.hpp"
#include "dmn-test-dbus-support.hpp"

#include <gtest/gtest.h>

namespace {

TEST(DbusClientSessionTest, DefaultSessionSelectionUsesPrivateDaemon) {
  dmn_test_dbus::PrivateBus bus;
  ASSERT_TRUE(bus.valid());
  ASSERT_TRUE(bus.setAsSessionBus());

  const dmn::Dmn_DbusClientConfig config;
  EXPECT_EQ(config.bus_kind, dmn::Dmn_DbusBusKind::Session);
  dmn::Dmn_DbusClient client{config};
  ASSERT_TRUE(client.status().connected);

  const auto id = client.call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "GetId");
  ASSERT_TRUE(id) << id.error().message;
  ASSERT_EQ(id->size(), 1U);
  EXPECT_FALSE(id->front().asString().empty());
}

TEST(DbusClientSessionTest, ExplicitAddressSelectsPrivateBusDaemon) {
  dmn_test_dbus::PrivateBus bus;
  ASSERT_TRUE(bus.valid());

  dmn::Dmn_DbusClientConfig config;
  config.bus_kind = dmn::Dmn_DbusBusKind::Address;
  config.bus_address = bus.address();
  dmn::Dmn_DbusClient client{config};
  ASSERT_TRUE(client.status().connected);

  const auto id = client.call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "GetId");
  ASSERT_TRUE(id) << id.error().message;
  ASSERT_EQ(id->size(), 1U);
  EXPECT_FALSE(id->front().asString().empty());
}

} // namespace
