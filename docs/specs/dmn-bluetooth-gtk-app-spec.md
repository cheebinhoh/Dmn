# Dmn Bluetooth GTK Application and Desktop Alerts Specification

**Status:** Partially implemented. The optional GTK application supports BlueZ
inventory, explicit discovery, multiple saved devices, sorting, a draft-based
Selected only filter, explicit per-device connect/disconnect and monitor
actions, and a process-local event feed. Monitoring is enabled only by an
explicit per-device user action; that intent is saved and restored at the next
launch. Restored monitors start only after BlueZ confirms that the saved,
paired device is connected. Active monitors stop when their device
disconnects and resume after it reconnects while the saved monitoring intent
remains enabled. The live event feed supports composed device, event-kind,
category, and case-insensitive text filters, row details, clearing, stable
arrival ordering, and bounded retention. The preferences window, close
confirmation/hidden-window mode, and desktop alerts remain unimplemented.
Concurrent multi-device operation is implemented but still requires
qualification with multiple physical devices.

This document separates code-derived current behavior from proposed product
requirements below. The optional GIO `GNotification` design is a proposal
only; the application currently owns a live in-process event feed and does
not send desktop alerts.

## 0. Implementation status

This table records the current implementation. The remaining sections
separately describe proposed product behavior.

| Capability | Current implementation |
| --- | --- |
| Optional GTK build and desktop application | Implemented under `ENABLE_APP_BLUETOOTH_GTK`; GTK 4 and libadwaita are optional dependencies. |
| BlueZ inventory and explicit discovery | Implemented with worker-thread BlueZ requests, powered-adapter selection, a finite scan, and explicit scan stop. |
| Saved device selection | Implemented with stable adapter/device addresses, multiple draft/save/discard entries, and unavailable saved devices. |
| Device inventory view | Implemented with Name/Status sorting, a Selected only filter over the current draft selection (its on/off preference is restored across launches), separate paired/connected/monitor states, and per-row Connect/Disconnect and Start/Stop monitoring actions. |
| Adjustable device pane | The window opens maximized. Its draggable GTK divider defaults to an even 50/50 split; user-adjusted pane proportion is saved in GSettings and restored independently of window size. Existing saved absolute pane positions are migrated to a proportion on first launch after the update. |
| Connect/disconnect actions | Implemented per saved device. Unpaired devices must be paired in Bluetooth Settings; disconnect stops that device's monitor first when needed. |
| ANCS monitor lifecycle | Implemented per saved, paired, already-connected device with progress and explicit start/stop. Monitor start requires an existing connection and never pairs or establishes the initial link; like the CLI, it asks BlueZ to connect any remaining auto-connectable profiles before ANCS startup. |
| Remembered monitoring intent | Implemented in GSettings using stable adapter/device addresses. An explicit Start monitoring choice is restored on launch; startup waits for a saved, paired device to be connected. Stop monitoring clears the intent. Disconnect or app exit stops the live monitor but preserves the intent. |
| Adaptive layout | Implemented as a two-pane window at wide sizes and a device-list/device-detail navigation flow below the libadwaita breakpoint. The narrow layout stacks scan and selection controls. |
| In-session event feed | Implemented using `GListStore`, `GtkFilterListModel`, `GtkSortListModel`, and `GtkColumnView`. Composable filters cover the viewed device or all devices, event kind, category, and Unicode case-insensitive text search. Rows show device, host receipt time, kind/category, and an event preview; row activation opens available details, including phone-provided date when present. Hovering notification text shows its full value wrapped within the feed-pane width. Arrival sequence—not wall-clock time—orders events newest-first. The process-local store retains at most 1,000 events and announces evictions. Clear notifications removes the in-memory feed; Clear filters restores defaults. Removed events show no stale attributes. Notification text remains selectable/copyable. |
| Desktop metadata and icon | Implemented with an AppStream `.metainfo.xml`, a lowercase desktop/application ID, a scalable 128×128 full-color SVG icon, and a matching symbolic icon installed in hicolor. The previous GSettings schema ID is retained to preserve saved preferences. |
| Preferences window, close confirmation/hidden-window mode, desktop alerts | Not implemented. Monitoring intent is controlled by per-device Start/Stop actions, not a separate preference screen. |

The current GTK window has a **Devices** inventory/sidebar and a
**Notifications** pane with per-device status and the live event feed. Checking
a device changes its draft selection but does not navigate away from the
inventory; activating its name opens the detail/feed pane. Event filters and
the detail view are part of the live feed; Preferences, desktop alerts, and
close/hidden-window options remain proposed. Simultaneous monitors have not
yet been hardware-qualified.

## 1. Goals, non-goals, and build boundary

### Goals

- Let a user inspect BlueZ-known devices, explicitly choose monitoring
  targets within the qualified concurrency limit, save that choice, and
  start/stop each selected phone.
- Display per-device setup/readiness/errors and a searchable, filterable,
  newest-first in-session ANCS event list.
- Keep the UI responsive during device discovery, pairing, connection, GATT,
  startup, and stop.
- Optionally alert the user when an ANCS event arrives while the app window is
  hidden or behind another window, provided the app process and monitor remain
  running.
- Use an ordinary desktop launcher entry; do not require a tray, extension,
  applet, or background service for the first release.

### Non-goals

- Reading, importing, or mirroring the GNOME/COSMIC notification-center
  history.
- Pairing, connecting, or monitoring every BlueZ-connected device on launch;
  automatic repair/reconnect; or automatic disconnection/unpairing on monitor
  stop.
- Continuing monitoring after the process exits, logout, or reboot.
- Sending notification bodies to desktop alerts by default.
- Claiming that ANCS provides complete iPhone notification history.

### Build integration

The separate `ENABLE_APP_BLUETOOTH_GTK` option defaults to `OFF`; the existing
`ENABLE_APP_BLUETOOTH` builds
`dmn-bluetooth-cli` and must remain unchanged. Enabling the GTK option must
enable the required ANCS, BlueZ, and D-Bus library layers using the project's
existing dependency pattern, but must not implicitly enable the CLI app
option or build the CLI unless the user separately requests it. When enabled,
require GTK 4, libadwaita, and GIO development/runtime dependencies. This
specification deliberately targets the GNOME HIG baseline: the current GNOME
HIG describes GTK 4 together with libadwaita as the GNOME platform for
application design. Keep the dependency optional and isolated to this target.
If a required system dependency is missing, CMake must fail at configure time
with its name and installation guidance. Do not let GTK discovery affect a
default/headless build. Name the executable `dmn-bluetooth-gtk`; do not rename
or replace the CLI target. Confirm Linux platform gating and document whether
the GTK frontend requires the same supported-platform policy as the BlueZ
backend.

### Proposed GTK and GNOME toolkit requirements

Use GTK 4 and libadwaita through their upstream C APIs from the C++
application. This avoids mixing gtkmm wrapper ownership with libadwaita's
GObject types, while retaining a C++ core and C++ RAII ownership at the view
boundary. Keep all GTK/libadwaita types out of `core/`. Use `AdwApplication`
and standard GTK/libadwaita widgets and patterns where appropriate; use
`GtkColumnView` or other GTK list models for the event feed, not a
custom-drawn table. Do not combine GTK 3 and GTK 4 widgets or replace platform
widgets with hand-drawn controls.

Use GNOME HIG as the normative design baseline on GNOME: follow its
navigation, keyboard, adaptive-layout, concise-writing, notification, and
accessibility guidance. Use Adwaita light/dark and high-contrast styling,
system text scaling, standard spacing/style classes, and accessible names.
Do not hard-code colors or rely on color/icon alone for lifecycle state.
Build for the narrowest supported window before optimizing the wide layout;
the device list and event content must remain usable without a permanently
visible sidebar. Keep preferences in the standard application preferences
surface and use standard GNOME keyboard shortcuts/actions.

CMake must discover `gtk4` and `libadwaita-1` with imported pkg-config
targets only inside the GTK app option. Add `src/app/gui` only when this
option is enabled. Keep the existing GoogleTest suite for C++ core/controller
tests; do not introduce a second assertion framework just to test the core.
Before committing to minimum versions, make a package/build spike on the
minimum supported Pop!_OS and GNOME releases and confirm GTK, libadwaita,
GIO notifications, compiler, and CI availability.

## 2. Proposed full product behavior

The following sections describe desired behavior and future scope, not a claim
that every listed screen or control exists in the current application. For
current behavior, use the implementation-status table above and the
implementation-accurate storyboard.

### Proposed main-window structure and user interactions

Use `AdwApplication`/`AdwApplicationWindow`, GTK 4 list models and
`GtkColumnView`, standard application actions, accessible names, and
theme-aware components. At wide widths, show device navigation beside the
event feed. At narrow widths, show one view at a time: the device list is the
navigation root, selecting a device/feed opens its event view, and a standard
Back action returns to the list. Opening event details must not create a
stack of unrelated windows. At wide widths, let users drag the pane divider
to choose how much room the device rows receive, and persist the pane
proportion rather than a fixed pixel position. Open the main window maximized
and use an even split by default. Use a breakpoint/layout component supported
by the declared minimum libadwaita version; do not invent a custom resizing
framework.

Desktop-alert and hidden-window preferences belong in the standard
Preferences surface, not in a permanently visible card embedded in the event
feed. Preferences must clearly describe that hiding the window keeps this
process alive, while quitting stops monitoring. The GTK wireframe's alert
control is illustrative only and must not override this requirement.

![Current wide-window layout of the standalone GTK app](images/dmn-bluetooth-gtk-app.svg)

[The GTK UI storyboard](dmn-bluetooth-gtk-ui-storyboard.md) depicts current
inventory, connection, monitoring, and feed states. Preferences, desktop
alerts, and close/hidden-window options remain unimplemented.

### First run

1. Show a short explanation that the app reads notifications exposed by iOS
   over ANCS and displays only live-session data.
2. Read a fresh `Dmn_BluezClient::snapshot()` on a worker. Present BlueZ-known
   paired and unpaired devices, with addresses available in details. A nearby
   device that is not yet in inventory requires an explicit **Scan for
   devices** action; never start Bluetooth discovery on launch. Let the user
   choose a powered adapter when more than one is available, expose scan
   progress and **Stop scan**. Bound a scan to 30 seconds by default; stop only
   a scan owned by this app when its deadline expires, the user cancels, or an
   error occurs. Use stable adapter/device addresses to resolve saved
   selection; do not key settings by display name or transient object path.
3. Explain that initial pairing/authorization happens through the normal
   BlueZ/system prompt and that iOS may require approval after a new
   notification arrives. Do not request pairing merely to populate the list.
4. Let the user select devices and press **Save selection**. Checkbox edits
   are draft state; connect and monitor-start actions are disabled while the
   draft differs from saved settings. Offer **Discard changes**. Persist stable
   adapter/device identities only. Display name and paired/connected facts
   are live data, not saved selection metadata. Persist the user's explicit
   monitoring intent separately from selection; never persist notification
   text. Multiple devices
   may be saved and monitored independently; qualify simultaneous operation on
   supported BlueZ/phone combinations before claiming it is production-ready.
5. Keep **Paired**, **Connected**, and **ANCS monitoring** as separate states.
   Pairing is performed through the desktop Bluetooth settings. **Connect**
   and **Disconnect** are explicit, accessible per-device actions; a row's
   context menu may provide shortcuts but must not be the only way to reach
   them. If a device is unpaired, explain how to pair it in system settings
   rather than silently pairing it.
6. A BlueZ `Connected=true` value does not mean ANCS is already **Ready**.
   Connecting a device does not start its monitor. Start a
   `Dmn_AncsBluezMonitor` only after the user explicitly activates **Start
   monitoring** for a saved, paired, connected device. Restore that explicit
   intent on later launches, but defer startup until the device is paired and
   BlueZ-confirmed connected. Do not infer intent from connection state or
   monitor other connected devices. The GTK monitor mode requires an existing
   connection and never discovers, pairs, or establishes an initial link.
   Only confirmed ANCS subscriptions mean **Ready**. Associate each event
   with its stable device identity and show only the viewed device's events
   from the process-session feed. No events from before monitor startup are
   available as history. A connected device outside the saved selection is
   never monitored.
7. Observe live connection state while saved devices exist. When a saved
   device disconnects or disappears, stop its monitor but retain its saved
   identity and any explicit monitoring intent so it resumes only after
   reconnection. **Stop monitoring** clears that intent. Disconnecting
   through the app stops the monitor first, then requests BlueZ disconnection;
   stopping a monitor alone must not disconnect the device. An explicit
   **Stop monitoring** action ends the monitor and clears the saved intent
   until the user explicitly starts it again.

### Implemented device pane

Each row shows the display name (address fallback), paired/connected facts,
monitor status when a session exists, a draft-selection checkbox, and
per-device connection and monitor actions. A saved device absent from BlueZ
remains visible as **Unavailable**. The selected-device pane displays live
status and the last monitor error; when **All devices** is enabled for the
event feed, the status-page title changes to **All devices** and device-specific
status is hidden until a particular device is selected again.
`Paired`, `Connected`, and `ServicesResolved` are not treated as ANCS **Ready**.

Current controls include **Refresh devices**, **Scan for devices**, **Stop
scan**, **Save selection**, **Discard changes**, **Selected only**,
per-device **Connect** or **Disconnect**, and **Start monitoring** or **Stop
monitoring**. The selection controls share a row at wide sizes and adapt to
stacked rows at narrow sizes. **Selected only** filters by the current draft;
it does not change or save that selection. Connecting only establishes the
Bluetooth connection and does not start monitoring. Disconnecting a monitored
device stops its ANCS subscriptions first and then requests BlueZ
disconnection. Stopping monitoring alone ends ANCS subscriptions but does not
disconnect or unpair. A saved, connected device remains unmonitored until the
user chooses **Start monitoring**. That choice is remembered across launches;
the app resumes only for devices with this explicit intent, after connection
is confirmed. **Stop monitoring** remains available while an intent is saved,
even when the device is disconnected or no monitor session is active; it
clears the intent. To retry a failed session, clear the intent and choose
**Start monitoring** again.

### Implemented event pane

The `GtkColumnView` shows **Device**, **Received**, **Type/category**, and
**Notification**. Search and device scope (the opened device or all devices),
event kind, and category filters compose without mutating source events.
Search case-folds Unicode text and checks device identity/name, app identifier,
title, subtitle, message, phone-provided date, and error text. Activating a row
opens available ANCS attributes, event kind, device identity, phone-provided
date, and host receipt time; missing values are labeled unavailable. Removed
events show only their current removal metadata, not stale notification
attributes. Hovering a notification value displays its full text wrapped to
the available feed-pane width.

The monitor callback captures host receipt time and a process-wide monotonic
arrival sequence before queuing the event to the GTK context. The bounded
store orders by sequence rather than wall clock or dispatch order, retains at
most 1,000 records across devices, and displays an eviction notice. **Clear
notifications** clears the process-local event store; **Clear filters**
restores the default device scope and filter values. Selecting **All devices**
updates the Bluetooth status-page title and hides device-specific status until
a particular device is selected again. The table distinguishes
empty and no-results states. Search terms and event payloads are never saved.
Notification text remains selectable and copyable from its Copy-only context
menu or Ctrl+C. Routine BlueZ refreshes do not rebuild unchanged event rows.

### Window and process lifecycle

- Minimize/obscure the window: monitoring continues because the app process
  remains alive; optional alerts can be submitted from the event callback.
- Close the window: show a confirmation if monitoring is active. The first
  release default is **Stop monitoring and quit**. An explicit preference
  **Keep monitoring when the window is closed** may instead hide the window
  and keep the process alive; default off. In this mode, hold the
  `GApplication` while a monitor remains active after the last window is
  hidden, and release that hold when monitoring stops. Since there is no
  required tray icon, reopening is done by launching the app again through
  `GApplication`.
- Quit, logout, or process termination: stop monitors, discard the in-memory
  event feed, and leave BlueZ bonds/links intact. No alert is generated for
  events not received while the process is alive.
- Do not describe a hidden GTK window as a daemon. This mode depends on the
  app process staying alive in the user's graphical session.

If setup is already running when the user presses **Stop**, show
**Stop requested — waiting for current setup to finish**. The current
`Dmn_AncsBluezMonitor::stop()` serializes behind synchronous `start()`; it
does not cancel discovery, pairing, or connection. Queue the stop and run it
as soon as start returns. Do not label this UI action **Cancel setup** unless
the Dmn API later implements and tests actual cancellation.

## 3. Optional GIO desktop alerts

Desktop alerts are a distinct, opt-in preference (**Desktop alerts: Off** by
default). The event callback may request an alert while the window is
minimized/hidden; the shell/notification service decides whether, when, and
where it is shown. This does not create a window inside the notification
center and does not keep the monitor alive.

### Default-safe alert policy

- Default summary: **New iPhone notification**; body is empty or generic.
  Do not include device name/address, title, subtitle, message, or other ANCS
  content in the default alert. A separate opt-in **Include device name in
  alerts** may be considered; it must default off. Never add notification
  title/body content without a separately named opt-in that warns the desktop
  server may retain it.
- Aggregate bursts per device rather than generating an unbounded alert per
  event. Suggested behavior: replace one notification ID per device and
  update “N new notifications” without content. Do not assert server support
  or delivery guarantees.
- Include an **Open Dmn Bluetooth** action using a stable
  `GApplication` action. It activates/presents the app; it does not mark an
  ANCS event as read on the phone.
- On **Clear notifications**, withdraw the app's outstanding alert IDs using
  `g_application_withdraw_notification()`. This is best-effort and does not
  clear the desktop notification center or affect ANCS state.
- On explicit stop/quit, also best-effort withdraw this app's outstanding
  generic alert IDs before releasing the application. The desktop may already
  have displayed or retained them; withdrawal is not a guarantee of deletion
  from every notification center.
- Respect the user disabling notifications at the desktop level. Failure to
  send an alert is not a monitor failure and must not interrupt the event
  feed.
- Do not rely on `persistence`, capabilities, or popup appearance as proof
  that the user saw an event. The notification protocol has no general API to
  query notification-center history.

Use `GNotification` through `GApplication` with a stable application ID,
desktop entry, `DBusActivatable=true` where packaging supports it, and
`X-GNOME-UsesNotifications=true` as GNOME metadata where appropriate. These
metadata keys improve GNOME integration but are not a COSMIC service contract.
Test actual supported COSMIC and GNOME releases; `send_notification()` is
best-effort and presentation may be suppressed.

## 4. Shared Dmn integration and concurrency

Create a monitor/session controller that owns `Dmn_AncsBluezMonitor` instances
and a GTK-independent presentation model. Register callbacks before
`start()`. Callback work must be prompt, non-throwing, and must not call
`start()`, `stop()`, or destroy the monitor. Copy event values and enqueue
them to the main context; touch GTK models/widgets only on the GTK main
thread. Send optional notifications from an application-owned notification
dispatcher on the main context.

`start()` and `stop()` are synchronous and bounded; run each on managed worker
execution, report progress to the main context, and serialize lifecycle
operations per monitor. `stop()` waits for an in-progress `start()` and does
not cancel pairing or connection. Hold worker/controller ownership until
callbacks are quiescent, then destroy monitors. Never detach workers whose
callback captures UI state.

The GTK frontend uses
`Dmn_AncsMonitorConfig::require_existing_connection` together with the
GTK-specific `request_remaining_profiles_when_connected` opt-in. It requires
the device to be present, paired, and connected, and never discovers, pairs,
or establishes the initial link. Like the CLI, it asks BlueZ to connect
remaining auto-connectable profiles before starting ANCS. This lets BlueZ
complete GATT-capable bearer/profile setup for a device that reports Connected
without forcing a disconnect/reconnect cycle. The default existing-connection
mode does not issue this additional request; private-bus tests cover both
behaviors and disconnected-device rejection.

The monitor facade represents one device. The GTK frontend creates one facade and worker task per saved, connected
device and associates events with the stable adapter/device identity. It polls
BlueZ while saved devices exist to observe external connection changes. The UI
has no arbitrary active-monitor cap. Before claiming multi-device reliability,
qualify simultaneous monitoring on the supported BlueZ/phone combinations
and add deterministic controller tests for independent start/stop and event
streams. Do not silently start only a subset.

Reuse recovery wording in
[`gtk-bluetooth-ancs-app-spec.md`](gtk-bluetooth-ancs-app-spec.md): missing
ANCS, `Notifying=false`, timeout, user authorization, and iOS-side
forget/re-pair guidance. Never auto-forget, infer a timeout's method, or log
payloads.

## 5. Data, settings, and privacy

Persist selected `(adapter address, device address)` pairs, explicit
monitoring intent, window geometry, pane size, non-content filter
preferences, alert preference, and the explicit hidden-window preference.
Never persist transient BlueZ object paths.
Write GSettings values only after explicit user action or genuine user
preference changes, not while restoring widgets.

ANCS payloads and search text stay in bounded memory for the current process
session. Do not include payloads in logs, crash reports, diagnostics, desktop
alerts, or telemetry. Clear feed and queued payloads on quit. Avoid accidental
copy-to-clipboard, accessibility text outside the app, or broad diagnostic
exports containing message bodies.

## 6. Test-driven implementation plan

Each step is a mergeable vertical slice. Add failing tests first, implement
the smallest behavior that satisfies them, then keep the new and existing
tests green.

Each milestone below names what to implement, what must fail before the
implementation, and how to verify it. Target names are proposed additions to
the existing CMake/CTest structure; preserve repository build conventions.

1. **Toolkit and build gate.**
   - Test first: configure/build with the new option `OFF`; confirm no GTK or
     libadwaita
     package lookup occurs and no GTK target is added. Configure with it `ON`
     while `ENABLE_APP_BLUETOOTH=OFF`; assert the CLI target remains absent
     and the GTK target is present. Configure with it `ON` and GTK missing;
     require a direct actionable diagnostic. On the minimum supported
     Pop!_OS image, verify `pkg-config --modversion gtk4 libadwaita-1`.
   - Implement: option default `OFF`, conditional subdirectory/target, GTK
     application skeleton, stable application ID, desktop entry and icon.
   - Verify: clean configure/build in option-OFF state; enabled build creates
     `dmn-bluetooth-gtk`; desktop-entry validation with
     `desktop-file-validate`; install into a temporary prefix and confirm
     executable, desktop file, icon, and schema paths. Launch twice and assert
     one primary `GApplication` instance/window activation, then quit.
   - Once implemented, the expected focused commands are:
     `cmake -S . -B build-no-gui -DENABLE_APP_BLUETOOTH_GTK=OFF`,
     `cmake -S . -B build-gui -DENABLE_APP_BLUETOOTH_GTK=ON
     -DENABLE_APP_BLUETOOTH=OFF`,
     `cmake --build build-gui --target dmn-bluetooth-gtk`, and
     `ctest --test-dir build-gui -L dmn-gui --output-on-failure`.
2. **Pure presentation model.**
   - Test first: table-driven tests for every `Dmn_AncsEvent::Kind`, absent
     optional fields, AttributesUnavailable, Removed with no attributes,
     category mapping (including unknown raw values), duplicate event
     sequences, device state transitions, 1,000-row retention, and stable
     arrival ordering when the wall clock changes.
   - Implement: `core/event-model`, `event-store`, `filter-state`, value
     types, injected clock and sequence source. Keep this code free of GTK,
     display-server, and D-Bus initialization.
   - Verify: run only the new `dmn-test-bluetooth-gui-model` GTest target
     without a display or BlueZ; run under sanitizers if configured. Assert
     notification payloads are not included in diagnostic formatters.
3. **Monitor controller and fake boundary.**
   - Test first: fake monitor factory verifies register-handler-before-start,
     ordered progress mapping, typed error mapping, no GTK calls on worker,
     stop requested during start queues after start, startup failure cleanup,
     destructor waits for callbacks, and no callback accesses destroyed
     state.
   - Implement: controller and operation executor with injectable monitor
     factory, event sink, and main-context dispatcher. Do not detach a worker.
   - Verify: run `dmn-test-bluetooth-gui-controller` with deterministic
     barriers/timeouts. Add private-bus integration using fake BlueZ/GATT for
     the real monitor facade; no physical device is required.
4. **GTK shell and navigation.**
   - Test first: application activation opens/presents one window; actions
     have stable names; keyboard shortcuts activate the same actions; quit
     requests monitor shutdown first. Resize across the declared adaptive
     breakpoint and assert phone navigation, feed, details, and Back remain
     reachable with no clipped actions or horizontal overflow.
   - Implement: `gtk/` application/window and empty-state placeholders using
     standard GTK widgets. Keep all GTK objects on the main context.
   - Verify: run `dmn-test-bluetooth-gtk-smoke` under a display; assert window
     exists, expected accessible names/roles exist, and repeated activation
     does not create duplicate controllers. Test at both sides of the
     breakpoint. Manual review checks theme behavior and window resizing
     only after automated assertions pass.
5. **Inventory, selection, and preferences.**
   - Test first: paired/unpaired/disconnected/unavailable rows, two adapters
     with matching device names, stable adapter+device key persistence,
     checkbox draft, save, discard, and app restore. Assert startup and
     checkbox toggles cause zero Pair/Connect calls. Verify that status fields
     are refreshed from BlueZ and not persisted as authoritative facts.
   - Implement: device list view, refresh, save/discard, GSettings schema and
     preferences. Persist no BlueZ object path.
   - Verify: model and widget tests against a fake inventory; install schema
     in a temporary schema directory and run
     `glib-compile-schemas --strict`; round-trip non-content settings and
     inspect storage for absence of titles/messages.
   - Current status: inventory, explicit scan, selection, sorting, and
     selection persistence are implemented. Preferences and isolated fake
     inventory/widget tests remain outstanding.
6. **Pairing and connection controls.**
   - Test first: paired/unpaired and connected/disconnected states drive the
     correct explicit action; action requests are serialized off the GTK
     thread, scoped by stable adapter/device identity, and failures preserve
     the observed state. Verify that disconnect does not unpair and that no
     pairing or connection is triggered by inventory, selection, save, or
     refresh.
   - Implement: per-device **Connect** and **Disconnect** using
     `Dmn_BluezClient::connectDevice()` and `disconnectDevice()`. Pair through
     desktop Bluetooth settings; do not invoke `pairDevice()` from this GTK
     flow. A context menu is a shortcut to equivalent visible, keyboard-
     reachable device actions, not their sole entry point.
   - Verify: deterministic fake-BlueZ/private-bus tests cover successful and
     failed requests, idempotent/current-state handling, busy-state ordering,
     and unchanged bond state after disconnect.
   - Current status: visible per-device actions, saved-device gating, and
     stop-before-disconnect are implemented. Unpaired devices are directed to
     system Bluetooth settings; frontend fake-BlueZ/widget tests remain
     outstanding.
7. **Monitor controller, lifecycle, and restart policy.**
   - Test first: a connected-but-unmonitored device is not labeled Ready; a
     monitor starts only after an explicit user action or restoration of that
     previously saved intent; stop
     does not disconnect; switching devices filters a shared session feed;
     monitor events are associated with the correct device. Verify saved
     devices without monitoring intent remain unmonitored at launch and after
     connection; devices with intent resume only after connection; Stop clears
     intent; disconnected devices stop without losing saved intent, unrelated
     devices never start, shutdown waits for all monitor cleanup, and
     notification events are device-scoped.
   - Implement: own one `Dmn_AncsBluezMonitor` per saved device with explicit
     monitoring intent, serialize each instance's transitions on managed
     workers, marshal progress/events onto GTK, poll selected-device link
     state, and retain events only in bounded process memory. Monitor startup
     uses the existing-connection mode and does not pair or establish the
     initial connection.
   - Current status: per-device start/stop, persisted monitoring intent and
     restoration after connection, per-device event routing, and monitor
     cleanup on disconnect/quit are implemented.
     Action-availability and bounded event-store unit tests cover core UI
     state; full fake-controller/widget tests and simultaneous hardware
     qualification remain outstanding.
8. **Event feed and filtering.**
   - Test first: arrival ordering with wall-clock rollback, stable sequence,
     each filter independently and composed, case-folding, missing fields,
     Details content, Removed behavior, no-results, no-events state, clear,
     overflow notice, and all payload retention boundaries.
   - Implemented: bounded process-local event store, stable callback arrival
     sequence, `GListStore`/`GtkFilterListModel`/`GtkSortListModel` and
     `GtkColumnView`, composable device/kind/category/text filters, details,
     clear actions, empty/no-results states, and an eviction notice.
   - Verified: GTK-independent tests cover arrival ordering across wall-clock
     rollback and dispatch reordering, Unicode case folding, composed filters,
     missing attributes, Removed events, clearing, and eviction. Display-backed
     widget and AT-SPI automation remains outstanding.
9. **Optional desktop alert sink.**
   - Test first: injected fake sink proves default Off, no payload in
     summaries/body, per-device aggregate/replacement IDs, Clear/stop
     withdrawal, action activation, send failure isolation, and no alert
     request for state/error events unless explicitly designed.
   - Implement: `GNotification` adapter behind user preference; stable app
     ID/action and desktop metadata. Send on the main context.
   - Verify: private session D-Bus notification fake records exact calls and
     guarantees no body, address, or device name leakage by default. Manual
     opt-in smoke tests on supported COSMIC and GNOME releases verify actual
     shell presentation/disabled policy; popup appearance is not an automated
     delivery guarantee.
10. **Packaging, accessibility, and hardware gate.**
   - Test first: install/uninstall manifest, no stray GTK runtime in default
     install, schema and desktop file resolution, all controls keyboard-
     reachable, dynamic statuses announced, high-contrast labels, and no
     payload in settings/logs/alerts.
   - Verify: run CMake/CTest option OFF and ON; run GUI tests with a virtual
     display in CI, AT-SPI tests in a desktop session, and manual Orca review.
     Opt-in real-iPhone qualification records Pop!_OS, desktop, BlueZ,
     adapter, phone, and iOS versions. Fake-bus tests are not hardware proof.

## 7. Acceptance criteria

- **GTK-AC-01:** Default project build remains free of GTK dependencies;
  opt-in build, package, and app tests pass.
- **GTK-AC-02:** UI reflects typed monitor state and stays responsive through
  slow setup and stop.
- **GTK-AC-03:** A window hidden behind other windows still receives events
  and, if opted in, submits alerts while the process is alive.
- **GTK-AC-04:** Closing/quit behavior matches the selected preference;
  process exit stops monitoring and does not disconnect/unpair.
- **GTK-AC-05:** Alerts are optional, generic by default, withdrawable
  best-effort, and never the authoritative event record.
- **GTK-AC-06:** Unit, fake-bus, private-bus, packaging, accessibility, and
  opt-in hardware gates pass before release claims are made.
- **GTK-AC-07:** The wide and narrow layouts follow GNOME HIG navigation and
  adaptive behavior, and desktop-alert controls are in Preferences rather
  than a persistent event-feed panel.
- **GTK-AC-08:** Bluetooth discovery begins only after **Scan for devices**;
  the user can choose a powered adapter when needed, observe scan status, and
  stop scanning. Scans have a finite deadline and every started, app-owned
  scan is stopped on timeout, cancellation, failure, or shutdown.
- **GTK-AC-09:** Device selection and save/discard behavior use stable
  adapter/device identities and do not silently impose an arbitrary
  concurrency cap. The app never pairs devices. A BlueZ connection is
  requested only by the explicit **Connect** action; inventory, selection,
  saving, and monitor startup must not connect a device. Monitoring requires
  a saved, paired, already-connected device and starts only after the user
  explicitly chooses **Start monitoring**. Qualify concurrent
  multi-device operation before making reliability claims.

## 8. GTK automation notes

Use the existing GoogleTest/CTest stack for `core/` and controller tests.
GTK 4 provides GTK test initialization/helpers (`gtk_test_init()` in GLib
test-based programs and helpers such as
`gtk_test_widget_wait_for_draw()`); widget tests can also use ordinary
assertions against accessible roles/names and application actions. Avoid
mixing two test-runner initializers in one binary: keep GoogleTest binaries
on GoogleTest, and if a GTK helper requires GLib's test runner, use a small
separate GLib test executable.

Run widget tests in CI with a virtual X server such as Xvfb where required;
do not use sleeps as synchronization—wait for the GTK main context/frame
clock or a test signal. UI automation may use AT-SPI actions/queries to
operate controls by accessible name. Keep a small number of toolkit-level
tests; most behavior belongs in display-free core/controller tests. Human
testing is still needed for visual hierarchy, screen-reader quality, and
actual shell notification behavior.

Keep one requirement-to-test mapping in the implementation change (or test
documentation) with a row for every GTK acceptance criterion. Each row names
the test target and whether it is display-free, display-backed, private-bus,
AT-SPI, or manual/hardware qualification. For asynchronous UI behavior, tests
must use barriers, completion signals, or main-context iteration with a
deadline; fixed sleeps are not valid evidence. A failed or unavailable
display/shell test must fail or be explicitly marked skipped in CI, never
silently count as passed.

## 9. Research references

- GTK [`GtkApplication`](https://docs.gtk.org/gtk4/class.Application.html),
  [`GtkApplicationWindow`](https://docs.gtk.org/gtk4/class.ApplicationWindow.html),
  [`GtkColumnView`](https://docs.gtk.org/gtk4/class.ColumnView.html), and
  [list widgets](https://docs.gtk.org/gtk4/section-list-widget.html).
- libadwaita [`AdwApplication`](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/class.Application.html):
  application initialization and GNOME-specific GTK application building
  blocks.
- GTK [test initialization](https://docs.gtk.org/gtk4/func.test_init.html),
  [wait for widget drawing](https://docs.gtk.org/gtk4/func.test_widget_wait_for_draw.html),
  and [accessibility](https://docs.gtk.org/gtk4/section-accessibility.html).
- GLib [unit testing framework](https://docs.gtk.org/glib/testing.html) and
  [GListModel](https://docs.gtk.org/gio/iface.ListModel.html).
- GIO [`GNotification`](https://docs.gtk.org/gio/class.Notification.html),
  [`send_notification`](https://docs.gtk.org/gio/method.Application.send_notification.html),
  and [`withdraw_notification`](https://docs.gtk.org/gio/method.Application.withdraw_notification.html).
- Freedesktop [Desktop Notifications basic design](https://specifications.freedesktop.org/notification/latest/basic-design.html)
  and [D-Bus protocol](https://specifications.freedesktop.org/notification/latest/protocol.html).
- Freedesktop [Desktop Entry Specification](https://specifications.freedesktop.org/desktop-entry/latest/).
- [GNOME Human Interface Guidelines](https://developer.gnome.org/hig/):
  the design baseline for the GTK 4/libadwaita frontend.
- GNOME HIG [sidebars](https://developer.gnome.org/hig/patterns/nav/sidebars.html)
  and [button patterns](https://developer.gnome.org/hig/patterns/controls/buttons.html):
  guidance for the wide layout and action hierarchy.
- GNOME [accessibility guidelines](https://developer.gnome.org/documentation/guidelines/accessibility.html):
  AT-SPI and assistive-technology interaction guidance for final integration
  checks.
- GNOME HIG [overview](https://developer.gnome.org/hig/): the design baseline
  for the GTK 4/libadwaita frontend.
- GNOME HIG [app icons](https://developer.gnome.org/hig/guidelines/app-icons.html):
  use a distinctive, simple 128×128 metaphor and provide a matching symbolic
  icon for small-size contexts.
