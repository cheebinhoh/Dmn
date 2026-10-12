/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluez-device-list.cpp
 * @brief TDD acceptance-test scaffold for the Linux BlueZ device inventory.
 */

#include <gtest/gtest.h>

namespace {

// These cases become private-system-bus integration tests when the optional
// BlueZ inventory adapter is implemented; skipped scaffolds are not coverage.
TEST(DmnBluezDeviceListSpec, IncludesAllConnectedDevicesAcrossAdapters) {
  GTEST_SKIP() << "BlueZ device inventory implementation pending; this is the "
                  "Linux fake-system-bus integration contract for enumeration.";
}

TEST(DmnBluezDeviceListSpec, IncludesKnownDisconnectedDevices) {
  GTEST_SKIP() << "BlueZ device inventory implementation pending; the full "
                  "inventory includes known devices that are not connected.";
}

TEST(DmnBluezDeviceListSpec, TracksConnectionAndObjectChanges) {
  GTEST_SKIP() << "BlueZ device inventory implementation pending; this is the "
                  "contract for live device connection and object updates.";
}

TEST(DmnBluezDeviceListSpec, DoesNotInitiateBluetoothOperations) {
  GTEST_SKIP() << "BlueZ device inventory implementation pending; listing must "
                  "not initiate pairing, discovery, or device connections.";
}

TEST(DmnBluezDiscoverySpec, StartsAndStopsOwnedDiscoverySession) {
  GTEST_SKIP() << "BlueZ discovery implementation pending; start/stop must "
                  "release only this client's discovery session.";
}

TEST(DmnBluezDiscoverySpec, AppliesFilterAndTracksDiscoveredDevices) {
  GTEST_SKIP() << "BlueZ discovery implementation pending; this is the "
                  "filter and live discovery-result integration contract.";
}

} // namespace
