/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-bluetooth-gui-selection.cpp
 * @brief GTK-independent tests for Bluetooth GUI selection and event state.
 */

#include "app/gui/core/dmn-desktop-alert-policy.hpp"
#include "app/gui/core/dmn-device-monitor-controller.hpp"
#include "app/gui/core/dmn-device-monitor-event-store.hpp"
#include "app/gui/core/dmn-device-selection-model.hpp"

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
using dmn::gui::Dmn_DeviceMonitorController;
using dmn::gui::Dmn_DeviceMonitorEvent;
using dmn::gui::Dmn_DeviceMonitorEventStore;
using dmn::gui::Dmn_DeviceSelectionModel;
using dmn::gui::Dmn_DeviceSortColumn;
using dmn::gui::Dmn_MonitorControllerCommand;
using dmn::gui::Dmn_MonitorControllerDeviceState;

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
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  const auto actions{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true})};

  EXPECT_FALSE(actions.m_can_connect);
  EXPECT_TRUE(actions.m_can_disconnect);
  EXPECT_TRUE(actions.m_can_start_monitoring);
  EXPECT_FALSE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     DisconnectedDeviceDoesNotOfferMonitoringActionEvenWithSavedIntent) {
  auto disconnected{device("AA:00", "BB:01", "iPhone One")};
  disconnected.m_connected = false;
  const auto actions{dmn::gui::deviceActionAvailability(
      disconnected,
      Dmn_DeviceActionContext{.m_saved = true, .m_monitor_requested = true})};

  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_FALSE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     ConnectionActionsRequirePairingAndMonitoringRequiresConnection) {
  auto unpaired{device("AA:00", "BB:01", "iPhone One", false)};
  unpaired.m_connected = true;
  auto disconnected{device("AA:00", "BB:02", "iPhone Two")};
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
     ConnectionAndMonitorActionsFollowDisconnectedAndConnectedStates) {
  auto disconnected{device("AA:00", "BB:01", "iPhone One")};
  disconnected.m_connected = false;
  const auto disconnectedActions{dmn::gui::deviceActionAvailability(
      disconnected, Dmn_DeviceActionContext{.m_saved = true})};
  EXPECT_TRUE(disconnectedActions.m_can_connect);
  EXPECT_FALSE(disconnectedActions.m_can_disconnect);
  EXPECT_FALSE(disconnectedActions.m_can_start_monitoring);
  EXPECT_FALSE(disconnectedActions.m_can_stop_monitoring);

  auto connected{disconnected};
  connected.m_connected = true;
  const auto connectedActions{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true})};
  EXPECT_FALSE(connectedActions.m_can_connect);
  EXPECT_TRUE(connectedActions.m_can_disconnect);
  EXPECT_TRUE(connectedActions.m_can_start_monitoring);
  EXPECT_FALSE(connectedActions.m_can_stop_monitoring);

  const auto monitoringActions{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_monitor_requested = true})};
  EXPECT_TRUE(monitoringActions.m_can_disconnect);
  EXPECT_FALSE(monitoringActions.m_can_start_monitoring);
  EXPECT_TRUE(monitoringActions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     MonitoringRequiresConfirmedConnectedStatus) {
  auto deviceEntry{device("AA:00", "BB:01", "iPhone One")};
  deviceEntry.m_connected.reset();
  const Dmn_DeviceActionContext saved{.m_saved = true};

  EXPECT_FALSE(dmn::gui::deviceActionAvailability(deviceEntry, saved)
                   .m_can_start_monitoring);

  deviceEntry.m_connected = true;
  EXPECT_TRUE(dmn::gui::deviceActionAvailability(deviceEntry, saved)
                  .m_can_start_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     BusyRowsKeepActionsAvailableButClosingRowsDoNot) {
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  const auto busy{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_monitor_requested = true,
                                         .m_operation_busy = true})};
  const auto closing{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_monitor_requested = true,
                                         .m_close_requested = true})};

  EXPECT_TRUE(busy.m_can_disconnect);
  EXPECT_FALSE(busy.m_can_start_monitoring);
  EXPECT_TRUE(busy.m_can_stop_monitoring);
  EXPECT_FALSE(closing.m_can_disconnect);
  EXPECT_FALSE(closing.m_can_start_monitoring);
  EXPECT_FALSE(closing.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     ConnectedMonitorTransitionKeepsDisconnectAvailable) {
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  const auto actions{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_monitor_requested = true,
                                         .m_monitor_busy = true,
                                         .m_operation_busy = true})};

  EXPECT_TRUE(actions.m_can_disconnect);
  EXPECT_FALSE(actions.m_can_connect);
  EXPECT_TRUE(actions.m_can_disconnect);
  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_TRUE(actions.m_can_stop_monitoring);

  auto disconnected{connected};
  disconnected.m_connected = false;
  const auto disconnectedActions{dmn::gui::deviceActionAvailability(
      disconnected,
      Dmn_DeviceActionContext{.m_saved = true, .m_selection_dirty = true})};
  EXPECT_TRUE(disconnectedActions.m_can_connect);
  EXPECT_FALSE(disconnectedActions.m_can_disconnect);
  EXPECT_FALSE(disconnectedActions.m_can_start_monitoring);
  EXPECT_FALSE(disconnectedActions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     PendingConnectionCanBeCanceledWithoutStartingOrStoppingMonitoring) {
  auto disconnected{device("AA:00", "BB:01", "iPhone One")};
  disconnected.m_connected = false;
  const auto actions{dmn::gui::deviceActionAvailability(
      disconnected, Dmn_DeviceActionContext{.m_saved = true,
                                            .m_connection_pending = true,
                                            .m_operation_busy = true})};

  EXPECT_FALSE(actions.m_can_connect);
  EXPECT_TRUE(actions.m_can_disconnect);
  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_FALSE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     QueuedDisconnectShowsConnectAsTheAvailableConnectionAction) {
  auto disconnected{device("AA:00", "BB:01", "iPhone One")};
  disconnected.m_connected = false;
  const auto actions{dmn::gui::deviceActionAvailability(
      disconnected, Dmn_DeviceActionContext{.m_saved = true,
                                            .m_disconnect_pending = true,
                                            .m_operation_busy = true})};

  EXPECT_TRUE(actions.m_can_connect);
  EXPECT_FALSE(actions.m_can_disconnect);
  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_FALSE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     PendingMonitorStartOffersStopButDisablesItAfterCancelRequest) {
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  const auto pending{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_requested = true,
                                         .m_monitor_busy = true,
                                         .m_monitor_start_pending = true})};
  EXPECT_FALSE(pending.m_can_start_monitoring);
  EXPECT_TRUE(pending.m_can_stop_monitoring);

  const auto canceling{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_busy = true,
                                         .m_monitor_start_pending = true,
                                         .m_monitor_cancel_requested = true})};
  EXPECT_TRUE(canceling.m_can_start_monitoring);
  EXPECT_FALSE(canceling.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     DirtySelectionBlocksStartsButAllowsActiveMonitorStop) {
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  const auto actions{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_selection_dirty = true,
                                         .m_monitor_active = true,
                                         .m_monitor_requested = true})};

  EXPECT_FALSE(actions.m_can_connect);
  EXPECT_FALSE(actions.m_can_start_monitoring);
  EXPECT_TRUE(actions.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     MonitorFailuresDoNotChangeIntentDrivenActionAvailability) {
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  const auto failed{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_monitor_requested = true,
                                         .m_monitor_busy = true,
                                         .m_operation_busy = true})};
  EXPECT_TRUE(failed.m_can_disconnect);
  EXPECT_FALSE(failed.m_can_start_monitoring);
  EXPECT_TRUE(failed.m_can_stop_monitoring);

  const auto stoppedWithoutIntent{dmn::gui::deviceActionAvailability(
      connected, Dmn_DeviceActionContext{.m_saved = true,
                                         .m_monitor_active = true,
                                         .m_monitor_busy = true})};
  EXPECT_TRUE(stoppedWithoutIntent.m_can_disconnect);
  EXPECT_TRUE(stoppedWithoutIntent.m_can_start_monitoring);
  EXPECT_FALSE(stoppedWithoutIntent.m_can_stop_monitoring);
}

TEST(DmnBluetoothGuiDeviceSelection,
     ConnectedSavedRowsAlwaysExposeOneConnectionAndMonitorAction) {
  auto connected{device("AA:00", "BB:01", "iPhone One")};
  connected.m_connected = true;
  for (const bool requested : {false, true}) {
    for (const bool monitorBusy : {false, true}) {
      for (const bool operationBusy : {false, true}) {
        const auto actions{dmn::gui::deviceActionAvailability(
            connected,
            Dmn_DeviceActionContext{.m_saved = true,
                                    .m_monitor_requested = requested,
                                    .m_monitor_busy = monitorBusy,
                                    .m_operation_busy = operationBusy})};
        EXPECT_TRUE(actions.m_can_disconnect);
        EXPECT_NE(actions.m_can_start_monitoring,
                  actions.m_can_stop_monitoring);
      }
    }
  }
}

TEST(DmnBluetoothGuiMonitorController,
     StartsOnlyForSavedConnectedDevicesWithMonitoringIntent) {
  Dmn_DeviceMonitorController controller;
  const Dmn_MonitorControllerDeviceState ready{.m_identity = {"AA:00", "BB:01"},
                                               .m_saved = true,
                                               .m_monitoring_requested = true,
                                               .m_present = true,
                                               .m_paired = true,
                                               .m_connected = true};

  const auto commands{controller.reconcile(
      {ready}, true, Dmn_DeviceMonitorController::Clock::time_point{})};

  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStart);
  EXPECT_EQ(commands.front().m_identity, ready.m_identity);
  EXPECT_TRUE(controller
                  .reconcile({ready}, false,
                             Dmn_DeviceMonitorController::Clock::time_point{})
                  .empty());
  auto busy{ready};
  busy.m_monitor_busy = true;
  EXPECT_TRUE(controller
                  .reconcile({busy}, true,
                             Dmn_DeviceMonitorController::Clock::time_point{})
                  .empty());

  auto noIntent{ready};
  noIntent.m_monitoring_requested = false;
  EXPECT_TRUE(controller
                  .reconcile({noIntent}, true,
                             Dmn_DeviceMonitorController::Clock::time_point{})
                  .empty());

  auto unsaved{ready};
  unsaved.m_saved = false;
  EXPECT_TRUE(controller
                  .reconcile({unsaved}, true,
                             Dmn_DeviceMonitorController::Clock::time_point{})
                  .empty());

  auto disconnected{ready};
  disconnected.m_connected = false;
  EXPECT_TRUE(controller
                  .reconcile({disconnected}, true,
                             Dmn_DeviceMonitorController::Clock::time_point{})
                  .empty());
}

TEST(DmnBluetoothGuiMonitorController,
     StopsActiveMonitorWhenDeviceIsDisconnectedOrNoLongerSaved) {
  Dmn_DeviceMonitorController controller;
  auto active{Dmn_MonitorControllerDeviceState{.m_identity = {"AA:00", "BB:01"},
                                               .m_saved = true,
                                               .m_present = true,
                                               .m_paired = true,
                                               .m_connected = false,
                                               .m_monitor_active = true}};

  auto commands{controller.reconcile(
      {active}, true, Dmn_DeviceMonitorController::Clock::time_point{})};

  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStop);

  active.m_connected = true;
  active.m_monitoring_requested = false;
  active.m_saved = true;
  commands = controller.reconcile(
      {active}, true, Dmn_DeviceMonitorController::Clock::time_point{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStop);

  active.m_saved = false;
  commands = controller.reconcile(
      {active}, true, Dmn_DeviceMonitorController::Clock::time_point{});
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStop);

  active.m_monitor_busy = true;
  EXPECT_TRUE(controller
                  .reconcile({active}, true,
                             Dmn_DeviceMonitorController::Clock::time_point{})
                  .empty());
}

TEST(DmnBluetoothGuiMonitorController,
     RestartsStoppedMonitorWhileConnectedIntentRemainsEnabled) {
  Dmn_DeviceMonitorController controller;
  const auto now{Dmn_DeviceMonitorController::Clock::time_point{}};
  const Dmn_MonitorControllerDeviceState stopped{
      .m_identity = {"AA:00", "BB:01"},
      .m_saved = true,
      .m_monitoring_requested = true,
      .m_present = true,
      .m_paired = true,
      .m_connected = true,
      .m_monitor_active = true,
      .m_monitor_restart_required = true};

  EXPECT_EQ(controller.recordStartFailure(stopped.m_identity, now, 100),
            std::chrono::seconds{5});
  const auto cleanup{controller.reconcile({stopped}, true, now)};
  ASSERT_EQ(cleanup.size(), 1U);
  EXPECT_EQ(cleanup.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStop);

  auto stoppedSession{stopped};
  stoppedSession.m_monitor_active = false;
  stoppedSession.m_monitor_restart_required = false;
  EXPECT_TRUE(controller.reconcile({stoppedSession}, true, now).empty());
  EXPECT_EQ(
      controller
          .reconcile({stoppedSession}, true, now + std::chrono::seconds{5})
          .front()
          .m_action,
      Dmn_MonitorControllerCommand::Action::kStart);
}

TEST(DmnBluetoothGuiMonitorController,
     RetriesFailedStartsWithCappedBackoffAndResetsAfterSuccess) {
  Dmn_DeviceMonitorController controller;
  const Dmn_DeviceIdentity identity{"AA:00", "BB:01"};
  const auto start{Dmn_DeviceMonitorController::Clock::time_point{}};
  auto ready{Dmn_MonitorControllerDeviceState{.m_identity = identity,
                                              .m_saved = true,
                                              .m_monitoring_requested = true,
                                              .m_present = true,
                                              .m_paired = true,
                                              .m_connected = true}};

  EXPECT_EQ(controller.recordStartFailure(identity, start, 100),
            std::chrono::seconds{5});
  EXPECT_TRUE(
      controller.reconcile({ready}, true, start + std::chrono::seconds{4})
          .empty());
  EXPECT_EQ(controller.reconcile({ready}, true, start + std::chrono::seconds{5})
                .front()
                .m_action,
            Dmn_MonitorControllerCommand::Action::kStart);

  auto failureTime{start + std::chrono::seconds{5}};
  EXPECT_EQ(controller.recordStartFailure(identity, failureTime, 100),
            std::chrono::seconds{10});
  EXPECT_EQ(controller.recordStartFailure(identity, failureTime, 100),
            std::chrono::seconds{20});
  EXPECT_EQ(controller.recordStartFailure(identity, failureTime, 100),
            std::chrono::seconds{40});
  EXPECT_EQ(controller.recordStartFailure(identity, failureTime, 100),
            std::chrono::seconds{60});
  EXPECT_EQ(controller.recordStartFailure(identity, failureTime, 80),
            std::chrono::seconds{48});
  EXPECT_THROW(controller.recordStartFailure(identity, failureTime, 101),
               std::invalid_argument);

  controller.recordStartSuccess(identity);
  auto commands{controller.reconcile({ready}, true, start)};
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStart);

  ready.m_connected = false;
  controller.recordStartFailure(identity, failureTime, 100);
  EXPECT_TRUE(controller.reconcile({ready}, true, failureTime).empty());
  ready.m_connected = true;
  commands = controller.reconcile({ready}, true, start);
  ASSERT_EQ(commands.size(), 1U);
  EXPECT_EQ(commands.front().m_action,
            Dmn_MonitorControllerCommand::Action::kStart);
}

TEST(DmnBluetoothGuiMonitorEventStore,
     FiltersIndependentDeviceFeedsAndEvictsOldestEvents) {
  const Dmn_DeviceIdentity first{"AA:00", "BB:01"};
  const Dmn_DeviceIdentity second{"AA:00", "BB:02"};
  Dmn_DeviceMonitorEventStore store{3};
  const auto received{std::chrono::system_clock::time_point{}};

  auto makeEvent{
      [received](const Dmn_DeviceIdentity &identity, const std::uint32_t uid) {
        dmn::Dmn_AncsEvent event;
        event.kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
        dmn::Dmn_AncsNotification notification;
        notification.kind = dmn::Dmn_AncsNotificationKind::Added;
        notification.uid = uid;
        event.notification = std::move(notification);

        return Dmn_DeviceMonitorEvent{identity, 0, received, std::move(event)};
      }};
  store.append(makeEvent(first, 1));
  store.append(makeEvent(second, 2));
  store.append(makeEvent(first, 3));

  auto firstEvents{store.eventsFor(first)};
  ASSERT_EQ(firstEvents.size(), 2U);
  EXPECT_EQ(firstEvents[0]->m_event.notification->uid, 1U);
  EXPECT_EQ(firstEvents[1]->m_event.notification->uid, 3U);

  store.append(makeEvent(second, 4));
  EXPECT_EQ(store.size(), 3U);
  firstEvents = store.eventsFor(first);
  ASSERT_EQ(firstEvents.size(), 1U);
  EXPECT_EQ(firstEvents.front()->m_event.notification->uid, 3U);
  EXPECT_EQ(store.eventsFor(second).size(), 2U);
  EXPECT_EQ(store.evictedCount(), 1U);
  EXPECT_EQ(store.clear(), 3U);
  EXPECT_TRUE(store.events().empty());
  EXPECT_EQ(store.evictedCount(), 0U);
}

TEST(DmnBluetoothGuiMonitorEventStore,
     PreservesArrivalSequenceWhenWallClockMovesBackwards) {
  const Dmn_DeviceIdentity identity{"AA:00", "BB:01"};
  Dmn_DeviceMonitorEventStore store{3};
  const auto later{
      std::chrono::system_clock::time_point{std::chrono::seconds{200}}};
  const auto earlier{
      std::chrono::system_clock::time_point{std::chrono::seconds{100}}};
  dmn::Dmn_AncsEvent first;
  first.kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
  dmn::Dmn_AncsEvent second;
  second.kind = dmn::Dmn_AncsEvent::Kind::NotificationUpdated;

  store.append({identity, 0, later, std::move(first)});
  store.append({identity, 0, earlier, std::move(second)});

  const auto events{store.eventsFor(identity)};
  ASSERT_EQ(events.size(), 2U);
  EXPECT_LT(events[0]->m_sequence, events[1]->m_sequence);
  EXPECT_GT(events[0]->m_received, events[1]->m_received);
  EXPECT_EQ(store.eventForSequence(events[0]->m_sequence), events[0]);
}

TEST(DmnBluetoothGuiMonitorEventStore,
     RestoresCallbackArrivalOrderWhenMainContextDispatchIsReordered) {
  const Dmn_DeviceIdentity identity{"AA:00", "BB:01"};
  Dmn_DeviceMonitorEventStore store{3};
  dmn::Dmn_AncsEvent earlierCallback;
  earlierCallback.kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
  dmn::Dmn_AncsEvent laterCallback;
  laterCallback.kind = dmn::Dmn_AncsEvent::Kind::NotificationUpdated;

  store.append({identity, 2, {}, std::move(laterCallback)});
  store.append({identity, 1, {}, std::move(earlierCallback)});

  const auto events{store.events()};
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0]->m_sequence, 1U);
  EXPECT_EQ(events[1]->m_sequence, 2U);
  EXPECT_EQ(events[0]->m_event.kind,
            dmn::Dmn_AncsEvent::Kind::NotificationAdded);
}

TEST(DmnBluetoothGuiEventFilter,
     ComposesDeviceKindCategoryAndUnicodeCaseInsensitiveSearch) {
  const Dmn_DeviceIdentity target{"AA:00", "BB:01"};
  const Dmn_DeviceIdentity other{"AA:00", "BB:02"};
  Dmn_DeviceMonitorEvent event;
  event.m_identity = target;
  event.m_event.kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
  dmn::Dmn_AncsNotification notification;
  notification.kind = dmn::Dmn_AncsNotificationKind::Added;
  notification.category = 4;
  notification.title = "CAFÉ";
  notification.message = "A new invitation";
  event.m_event.notification = std::move(notification);

  dmn::gui::Dmn_DeviceMonitorEventFilter filter;
  filter.m_identity = target;
  filter.m_kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
  filter.m_category = 4;
  filter.m_search = "café";
  EXPECT_TRUE(dmn::gui::matchesEventFilter(event, filter, "Phone One"));

  filter.m_identity = other;
  EXPECT_FALSE(dmn::gui::matchesEventFilter(event, filter, "Phone One"));
  filter.m_identity = target;
  filter.m_kind = dmn::Dmn_AncsEvent::Kind::NotificationUpdated;
  EXPECT_FALSE(dmn::gui::matchesEventFilter(event, filter, "Phone One"));
  filter.m_kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
  filter.m_category = 2;
  EXPECT_FALSE(dmn::gui::matchesEventFilter(event, filter, "Phone One"));
  filter.m_category = 4;
  filter.m_search = "phone one";
  EXPECT_TRUE(dmn::gui::matchesEventFilter(event, filter, "Phone One"));
  filter.m_search = "not found";
  EXPECT_FALSE(dmn::gui::matchesEventFilter(event, filter, "Phone One"));
}

TEST(DmnBluetoothGuiEventFilter,
     HandlesMissingAttributesAndRemovedEventsWithoutStaleText) {
  Dmn_DeviceMonitorEvent event;
  event.m_identity = {"AA:00", "BB:01"};
  event.m_event.kind = dmn::Dmn_AncsEvent::Kind::NotificationRemoved;
  dmn::Dmn_AncsNotification removed;
  removed.kind = dmn::Dmn_AncsNotificationKind::Removed;
  removed.uid = 42;
  removed.category = 1;
  event.m_event.notification = removed;

  dmn::gui::Dmn_DeviceMonitorEventFilter filter;
  filter.m_search = "42";
  EXPECT_FALSE(dmn::gui::matchesEventFilter(event, filter));
  filter.m_search.clear();
  filter.m_category = 1;
  EXPECT_TRUE(dmn::gui::matchesEventFilter(event, filter));

  event.m_event.kind = dmn::Dmn_AncsEvent::Kind::AttributesUnavailable;
  event.m_event.notification.reset();
  EXPECT_FALSE(dmn::gui::matchesEventFilter(event, filter));
}

TEST(DmnBluetoothGuiDesktopAlertPolicy, OnlyAlertsForAddedNotifications) {
  dmn::Dmn_AncsEvent event;
  EXPECT_FALSE(dmn::gui::shouldSendDesktopAlert(event));

  event.kind = dmn::Dmn_AncsEvent::Kind::NotificationAdded;
  EXPECT_TRUE(dmn::gui::shouldSendDesktopAlert(event));

  event.kind = dmn::Dmn_AncsEvent::Kind::NotificationUpdated;
  EXPECT_FALSE(dmn::gui::shouldSendDesktopAlert(event));
  event.kind = dmn::Dmn_AncsEvent::Kind::NotificationRemoved;
  EXPECT_FALSE(dmn::gui::shouldSendDesktopAlert(event));
  event.kind = dmn::Dmn_AncsEvent::Kind::AttributesUnavailable;
  EXPECT_FALSE(dmn::gui::shouldSendDesktopAlert(event));
  event.kind = dmn::Dmn_AncsEvent::Kind::Error;
  EXPECT_FALSE(dmn::gui::shouldSendDesktopAlert(event));
  event.kind = dmn::Dmn_AncsEvent::Kind::StateChanged;
  EXPECT_FALSE(dmn::gui::shouldSendDesktopAlert(event));
}

TEST(DmnBluetoothGuiDesktopAlertPolicy,
     AggregatesWithStablePerDeviceIdAndNoDeviceOrNotificationContent) {
  const dmn::gui::Dmn_DeviceIdentity first{"ADAPTER-PRIVATE", "DEVICE-PRIVATE"};
  const dmn::gui::Dmn_DeviceIdentity second{"ADAPTER-PRIVATE", "DEVICE-OTHER"};

  const auto firstAlert{dmn::gui::makeDesktopAlert(first, 1)};
  const auto updatedAlert{dmn::gui::makeDesktopAlert(first, 3)};
  const auto otherAlert{dmn::gui::makeDesktopAlert(second, 1)};

  EXPECT_EQ(firstAlert.m_id, updatedAlert.m_id);
  EXPECT_NE(firstAlert.m_id, otherAlert.m_id);
  EXPECT_EQ(firstAlert.m_title, "New iPhone notification");
  EXPECT_EQ(firstAlert.m_body,
            "1 new notification arrived. Open Dmn Bluetooth to view it.");
  EXPECT_EQ(updatedAlert.m_body,
            "3 new notifications arrived. Open Dmn Bluetooth to view them.");
  EXPECT_EQ(firstAlert.m_title.find("DEVICE-PRIVATE"), std::string::npos);
  EXPECT_EQ(firstAlert.m_body.find("DEVICE-PRIVATE"), std::string::npos);
  EXPECT_EQ(firstAlert.m_body.find("ADAPTER-PRIVATE"), std::string::npos);
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
  auto connected{device("AA:00", "BB:01", "Connected")};
  connected.m_connected = true;
  auto disconnected{device("AA:00", "BB:02", "Disconnected")};
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
