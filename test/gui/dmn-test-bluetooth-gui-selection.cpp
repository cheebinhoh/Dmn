/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluetooth-gui-selection.cpp
 * @brief GTK-independent tests for Bluetooth device selection state.
 */

#include "device-selection-model.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using dmn::gui::Dmn_DeviceEntry;
using dmn::gui::Dmn_DeviceIdentity;
using dmn::gui::Dmn_DeviceSelectionModel;
using dmn::gui::Dmn_DeviceSortColumn;

auto device(std::string adapter, std::string address, std::string name,
            const bool paired = true) -> Dmn_DeviceEntry {
  return {{std::move(adapter), std::move(address)},
          std::move(name),
          paired,
          false,
          true,
          false};
}

TEST(DmnBluetoothGuiDeviceSelection, RequiresPositiveLimit) {
  EXPECT_THROW((Dmn_DeviceSelectionModel{0}), std::invalid_argument);
}

TEST(DmnBluetoothGuiDeviceSelection, SavesAndDiscardsDraftSelection) {
  Dmn_DeviceSelectionModel model;
  const Dmn_DeviceIdentity first{"AA:00", "BB:01"};
  const Dmn_DeviceIdentity second{"AA:00", "BB:02"};
  model.replaceInventory({device("aa:00", "bb:01", "iPhone One"),
                          device("aa:00", "bb:02", "iPhone Two")});

  EXPECT_TRUE(model.setDraftSelected(first, true));
  EXPECT_TRUE(model.hasUnsavedChanges());
  EXPECT_FALSE(model.canStartMonitoring());

  model.saveSelection();
  EXPECT_FALSE(model.hasUnsavedChanges());
  EXPECT_TRUE(model.canStartMonitoring());
  EXPECT_EQ(model.savedSelection(),
            (std::vector{Dmn_DeviceIdentity{"AA:00", "BB:01"}}));

  EXPECT_TRUE(model.setDraftSelected(first, false));
  EXPECT_TRUE(model.setDraftSelected(second, true));
  model.discardChanges();

  EXPECT_EQ(model.draftSelection(), model.savedSelection());
  EXPECT_TRUE(model.canStartMonitoring());
}

TEST(DmnBluetoothGuiDeviceSelection, EnforcesConfiguredLimit) {
  Dmn_DeviceSelectionModel model{1};
  const Dmn_DeviceIdentity first{"AA:00", "BB:01"};
  const Dmn_DeviceIdentity second{"AA:00", "BB:02"};
  model.replaceInventory({device("AA:00", "BB:01", "iPhone One"),
                          device("AA:00", "BB:02", "iPhone Two")});

  EXPECT_TRUE(model.setDraftSelected(first, true));
  EXPECT_FALSE(model.setDraftSelected(second, true));
  EXPECT_EQ(model.draftSelection().size(), 1U);
}

TEST(DmnBluetoothGuiDeviceSelection, SortsByDisplayNameInBothDirections) {
  Dmn_DeviceSelectionModel model;
  model.replaceInventory({device("AA:00", "BB:03", "zulu"),
                          device("AA:00", "BB:02", "Alpha"),
                          device("AA:00", "BB:01", "")});

  ASSERT_EQ(model.devices().size(), 3U);
  EXPECT_EQ(dmn::gui::deviceDisplayName(model.devices()[0]), "Alpha");
  EXPECT_EQ(dmn::gui::deviceDisplayName(model.devices()[1]), "BB:01");
  EXPECT_EQ(dmn::gui::deviceDisplayName(model.devices()[2]), "zulu");

  model.sortDevices(Dmn_DeviceSortColumn::kName, false);
  EXPECT_EQ(dmn::gui::deviceDisplayName(model.devices()[0]), "zulu");
  EXPECT_EQ(dmn::gui::deviceDisplayName(model.devices()[1]), "BB:01");
  EXPECT_EQ(dmn::gui::deviceDisplayName(model.devices()[2]), "Alpha");
}

TEST(DmnBluetoothGuiDeviceSelection, SortsByVisibleStatus) {
  Dmn_DeviceSelectionModel model;
  auto connected = device("AA:00", "BB:01", "Connected");
  connected.m_connected = true;
  auto disconnected = device("AA:00", "BB:02", "Disconnected");
  disconnected.m_connected = false;
  model.loadSavedSelection({{"AA:00", "BB:03"}});
  model.replaceInventory({connected, disconnected});

  model.sortDevices(Dmn_DeviceSortColumn::kStatus, true);

  ASSERT_EQ(model.devices().size(), 3U);
  EXPECT_EQ(dmn::gui::deviceDisplayStatus(model.devices()[0]),
            "Paired · Connected");
  EXPECT_EQ(dmn::gui::deviceDisplayStatus(model.devices()[1]),
            "Paired · Disconnected");
  EXPECT_EQ(dmn::gui::deviceDisplayStatus(model.devices()[2]), "Unavailable");
}

TEST(DmnBluetoothGuiDeviceSelection, MissingSavedTargetRemainsUnavailable) {
  Dmn_DeviceSelectionModel model;
  const Dmn_DeviceIdentity saved{"AA:00", "BB:01"};
  model.loadSavedSelection({saved});

  ASSERT_EQ(model.devices().size(), 1U);
  EXPECT_FALSE(model.devices().front().m_present);
  EXPECT_EQ(model.devices().front().m_display_name, saved.m_device_address);
  EXPECT_FALSE(model.canStartMonitoring());

  model.replaceInventory({device("aa:00", "bb:01", "iPhone One")});
  ASSERT_EQ(model.devices().size(), 1U);
  EXPECT_TRUE(model.devices().front().m_present);
  EXPECT_TRUE(model.canStartMonitoring());
}

TEST(DmnBluetoothGuiDeviceSelection,
     InventoryRemovalMakesSavedTargetUnavailableUntilRediscovered) {
  Dmn_DeviceSelectionModel model;
  const Dmn_DeviceIdentity saved{"AA:00", "BB:01"};
  model.loadSavedSelection({saved});
  model.replaceInventory({device("AA:00", "BB:01", "iPhone One")});
  ASSERT_TRUE(model.canStartMonitoring());

  model.replaceInventory({});

  ASSERT_EQ(model.devices().size(), 1U);
  EXPECT_FALSE(model.devices().front().m_present);
  EXPECT_FALSE(model.canStartMonitoring());

  model.replaceInventory({device("aa:00", "bb:01", "iPhone One")});
  EXPECT_TRUE(model.canStartMonitoring());
}

TEST(DmnBluetoothGuiDeviceSelection,
     AdapterAddressDisambiguatesSameDeviceName) {
  Dmn_DeviceSelectionModel model{2};
  const Dmn_DeviceIdentity first{"AA:01", "BB:01"};
  const Dmn_DeviceIdentity second{"AA:02", "BB:01"};
  model.replaceInventory(
      {device("AA:01", "BB:01", "iPhone"), device("AA:02", "BB:01", "iPhone")});

  EXPECT_TRUE(model.setDraftSelected(first, true));
  EXPECT_TRUE(model.setDraftSelected(second, true));
  model.saveSelection();

  EXPECT_EQ(model.savedSelection().size(), 2U);
  EXPECT_NE(model.savedSelection()[0].m_adapter_address,
            model.savedSelection()[1].m_adapter_address);
}

TEST(DmnBluetoothGuiDeviceSelection,
     SavedIdentitiesAreNormalizedAndDeduplicated) {
  Dmn_DeviceSelectionModel model;
  model.loadSavedSelection(
      {{"aa:00", "bb:01"}, {"AA:00", "BB:01"}, {"", "BB:02"}});

  EXPECT_EQ(model.savedSelection(),
            (std::vector{Dmn_DeviceIdentity{"AA:00", "BB:01"}}));

  model.replaceInventory({device("aa:00", "bb:01", "iPhone One")});
  EXPECT_TRUE(model.canStartMonitoring());
}

TEST(DmnBluetoothGuiDeviceSelection,
     RejectsUnavailableSelectionButAllowsClearingIt) {
  Dmn_DeviceSelectionModel model;
  const Dmn_DeviceIdentity missing{"AA:00", "BB:01"};
  EXPECT_FALSE(model.setDraftSelected(missing, true));

  model.loadSavedSelection({missing});
  EXPECT_TRUE(model.setDraftSelected(missing, true));
  EXPECT_TRUE(model.setDraftSelected(missing, false));
  EXPECT_TRUE(model.hasUnsavedChanges());
  EXPECT_TRUE(model.draftSelection().empty());

  model.discardChanges();
  ASSERT_EQ(model.draftSelection().size(), 1U);

  EXPECT_TRUE(model.setDraftSelected(missing, false));
  EXPECT_TRUE(model.setDraftSelected(missing, true));
  EXPECT_EQ(model.draftSelection(),
            (std::vector{Dmn_DeviceIdentity{"AA:00", "BB:01"}}));
}

} // namespace
