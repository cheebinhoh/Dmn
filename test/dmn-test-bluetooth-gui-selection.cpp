/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluetooth-gui-selection.cpp
 * @brief GTK-independent tests for Bluetooth GUI selection and event state.
 */

#include "device-monitor-event-store.hpp"
#include "device-selection-model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using dmn::gui::Dmn_DeviceActionContext;
using dmn::gui::Dmn_DeviceEntry;
using dmn::gui::Dmn_DeviceIdentity;
using dmn::gui::Dmn_DeviceMonitorEvent;
using dmn::gui::Dmn_DeviceMonitorEventStore;
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

TEST(DmnBluetoothGuiDeviceSelection,
     SavedConnectedDeviceOffersDisconnectAndMonitoring) {
  auto connected = device("AA:00", "BB:01", "iPhone One");
  connected.m_connected = true;
  const auto actions = dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true});

  EXPECT_FALSE(actions.m_can_connect);
  EXPECT_TRUE(actions.m_can_disconnect);
  EXPECT_TRUE(actions.m_can_start_monitoring);
  EXPECT_FALSE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     SavedMonitoringIntentCanBeStoppedWithoutActiveMonitor) {
  auto disconnected = device("AA:00", "BB:01", "iPhone One");
  disconnected.m_connected = false;
  const auto actions = dmn::gui::deviceActionAvailability(
      disconnected,
      Dmn_DeviceActionContext{.m_saved = true, .m_monitor_requested = true});

  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_TRUE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     ConnectionActionsRequirePairingAndMonitoringRequiresConnection) {
  auto unpaired = device("AA:00", "BB:01", "iPhone One", false);
  unpaired.m_connected = true;
  auto disconnected = device("AA:00", "BB:02", "iPhone Two");
  disconnected.m_connected = false;
  const Dmn_DeviceActionContext saved{.m_saved = true};

  EXPECT_FALSE(dmn::gui::deviceActionAvailability(unpaired, saved)
                   .m_can_start_monitoring);
  EXPECT_FALSE(
      dmn::gui::deviceActionAvailability(unpaired, saved).m_can_connect);
  EXPECT_FALSE(dmn::gui::deviceActionAvailability(disconnected, saved)
                   .m_can_start_monitoring);
  EXPECT_TRUE(
      dmn::gui::deviceActionAvailability(disconnected, saved).m_can_connect);
}

TEST(DmnBluetoothGuiDeviceSelection,
     MonitoringRequiresConfirmedConnectedStatus) {
  auto deviceEntry = device("AA:00", "BB:01", "iPhone One");
  deviceEntry.m_connected.reset();
  const Dmn_DeviceActionContext saved{.m_saved = true};

  EXPECT_FALSE(dmn::gui::deviceActionAvailability(deviceEntry, saved)
                   .m_can_start_monitoring);

  deviceEntry.m_connected = true;
  EXPECT_TRUE(dmn::gui::deviceActionAvailability(deviceEntry, saved)
                  .m_can_start_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     BusyOrClosingRowsDoNotOfferConnectionOrMonitorActions) {
  auto connected = device("AA:00", "BB:01", "iPhone One");
  connected.m_connected = true;
  const auto busy = dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_operation_busy = true});
  const auto closing = dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_close_requested = true});

  EXPECT_FALSE(busy.m_can_disconnect);
  EXPECT_FALSE(busy.m_can_start_monitoring);
  EXPECT_FALSE(busy.m_can_stop_monitoring);
  EXPECT_FALSE(closing.m_can_disconnect);
  EXPECT_FALSE(closing.m_can_start_monitoring);
  EXPECT_FALSE(closing.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     DirtySelectionBlocksStartsButAllowsActiveMonitorStop) {
  auto connected = device("AA:00", "BB:01", "iPhone One");
  connected.m_connected = true;
  const auto actions = dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_selection_dirty = true,
                                         .m_monitor_active = true});

  EXPECT_FALSE(actions.m_can_connect);
  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_TRUE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiMonitorEventStore,
     FiltersIndependentDeviceFeedsAndEvictsOldestEvents) {
  const Dmn_DeviceIdentity first{"AA:00", "BB:01"};
  const Dmn_DeviceIdentity second{"AA:00", "BB:02"};
  Dmn_DeviceMonitorEventStore store{3};
  const auto received = std::chrono::system_clock::time_point{};

  auto makeEvent = [received](const Dmn_DeviceIdentity &identity,
                              const std::uint32_t uid) {
    dmn::Dmn_AncsEvent event;
    event.kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
    dmn::Dmn_AncsNotification notification;
    notification.kind = dmn::Dmn_AncsNotificationKind::Added;
    notification.uid = uid;
    event.notification = std::move(notification);

    return Dmn_DeviceMonitorEvent{identity, received, std::move(event)};
  };
  store.append(makeEvent(first, 1));
  store.append(makeEvent(second, 2));
  store.append(makeEvent(first, 3));

  auto firstEvents = store.eventsFor(first);
  ASSERT_EQ(firstEvents.size(), 2U);
  EXPECT_EQ(firstEvents[0]->m_event.notification->uid, 1U);
  EXPECT_EQ(firstEvents[1]->m_event.notification->uid, 3U);

  store.append(makeEvent(second, 4));
  EXPECT_EQ(store.size(), 3U);
  firstEvents = store.eventsFor(first);
  ASSERT_EQ(firstEvents.size(), 1U);
  EXPECT_EQ(firstEvents.front()->m_event.notification->uid, 3U);
  EXPECT_EQ(store.eventsFor(second).size(), 2U);
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

TEST(DmnBluetoothGuiDeviceSelection, SavesMultipleSelectedDevices) {
  Dmn_DeviceSelectionModel model;
  const Dmn_DeviceIdentity first{"AA:00", "BB:01"};
  const Dmn_DeviceIdentity second{"AA:00", "BB:02"};
  model.replaceInventory({device("AA:00", "BB:01", "iPhone One"),
                          device("AA:00", "BB:02", "iPhone Two")});

  EXPECT_TRUE(model.setDraftSelected(first, true));
  EXPECT_TRUE(model.setDraftSelected(second, true));
  model.saveSelection();

  EXPECT_EQ(model.savedSelection(), (std::vector{first, second}));
  EXPECT_TRUE(model.canStartMonitoring());
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
