# Dmn Bluetooth GTK Application and Desktop Alerts Specification

**Status:** Proposed. The GTK frontend and alert integration are not
implemented.

This is the recommended first full GUI for Pop!_OS and other Linux desktops.
It specifies the standalone GTK application, with optional GIO
`GNotification` alerts. The application owns its live event feed; desktop
alerts are a secondary output and not an embedded notification-center view.

## 1. Goals, non-goals, and build boundary

### Goals

- Let a user inspect BlueZ-known devices, explicitly choose one or more
  monitoring targets, save that choice, and start/stop each selected phone.
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
- Pairing or monitoring on launch, automatic repair/reconnect, or automatic
  disconnection/unpairing on stop.
- Continuing monitoring after the process exits, logout, or reboot.
- Sending notification bodies to desktop alerts by default.
- Claiming that ANCS provides complete iPhone notification history.

### Build integration

Add a separate `ENABLE_APP_BLUETOOTH_GTK` option, default `OFF`. When enabled,
it should enable the required ANCS, BlueZ, and D-Bus library layers using the
project's existing app-option dependency pattern, then require GTK 4/gtkmm 4
and GIO development/runtime dependencies. If a required system dependency is
missing, CMake must fail at configure time with the dependency name and
installation guidance. Do not let GTK discovery affect a default/headless
build. Name the executable `dmn-bluetooth-gtk`; do not rename or replace the
CLI target.

### C++ GTK binding

Use GTK 4 with its maintained C++ binding, gtkmm 4 (`gtkmm-4.0` pkg-config
module), as the initial recommendation: the application is C++23 and the
binding provides C++ ownership/types for widgets and GIO. Keep the actual
`core/` layer independent of both gtkmm and GTK C headers. Before committing
to it, make a small package/build spike on the minimum supported Pop!_OS
release and confirm the distribution package, compiler, GTK minor version,
GIO notifications, and CI environment. If gtkmm availability becomes a
release blocker, use GTK's C API behind a narrow adapter rather than mixing C
and C++ widget ownership throughout the app. Do not combine GTK 3 and GTK 4
widgets or add libadwaita as a requirement without a separate design decision.

CMake must `find_package(PkgConfig)` and require the gtkmm-4.0 imported
pkg-config target only inside the app option. Add `src/app/gui` only when this
option is enabled. Keep the existing GoogleTest suite for C++ unit tests; do
not introduce a second assertion/test framework just to test the core.

## 2. Main-window structure and user interactions

Use `GtkApplication`/`GtkApplicationWindow`, GTK 4 list models and
`GtkColumnView`, standard actions, accessible labels, and theme-aware
components. The main layout is a resizable device sidebar and event table.

![Wireframe of the standalone GTK app and optional alert setting](images/dmn-bluetooth-gtk-app.svg)

### First run

1. Show a short explanation that the app reads notifications exposed by iOS
   over ANCS and displays only live-session data.
2. Read a fresh `Dmn_BluezClient::snapshot()` on a worker. Present BlueZ-known
   paired and unpaired devices, with addresses available in details. Use
   stable adapter/device addresses to resolve saved selection; do not key
   settings by display name or transient object path.
3. Explain that initial pairing/authorization happens through the normal
   BlueZ/system prompt and that iOS may require approval after a new
   notification arrives. Do not request pairing merely to populate the list.
4. Let the user select devices and press **Save selection**. Checkbox edits
   are draft state; **Monitor selected** is disabled while the draft differs
   from saved settings. Offer **Discard changes**.
5. Monitoring begins only after explicit **Monitor selected** or per-device
   **Start**.

### Device pane

Each row shows display name (address fallback), paired/connected facts,
monitor state, and a textual last error. A saved device absent from BlueZ
remains visible as **Unavailable**. Distinguish `Paired`, `Connected`, and
`ServicesResolved` from ANCS **Ready**. State transitions are:
`Not monitored → Waiting → Pairing → Connecting → Starting ANCS → Ready`,
with `Failed`, `Unavailable`, and `Stopped` terminal/recovery states.

Controls: **Refresh devices**, **Save selection**, **Discard changes**,
**Monitor selected**, **Stop selected**, **Stop all**, and a per-device
**Retry** when failed. Stop ends ANCS subscriptions but does not disconnect
or unpair. Do not place pairing, disconnect, or unpair in ambiguous one-click
controls.

### Event pane

Default table columns: **Device**, **Received**, **Type/category**, and
**Notification** (app identifier and concise title/message preview).
Activating a row opens details with available ANCS attributes, event kind,
device identity, phone-provided date, and host receipt time. Missing data is
not invented. Removed events contain no stale notification attributes.

Capture host wall-clock receipt time and a monotonic arrival sequence in the
monitor callback before enqueueing to the UI. Sort newest-first by arrival
sequence (not the phone-provided ANCS date or a wall clock that may change);
display the host receipt time to the user. Keep at most 1,000 rows in memory,
evict the oldest on overflow, and announce the eviction nonintrusively. This
cap is independent of the monitor's ANCS pending-request limit.

Filters compose without mutating source events: device, Added/Updated/Removed,
available category, and case-insensitive search over app identifier, title,
subtitle, message, and device name. Provide **Clear filters**, **Clear
notifications**, useful empty/no-results messages, keyboard navigation, and
screen-reader-visible state. Never persist search terms or event payloads.

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

The monitor facade represents one device. Multi-device use means one monitor
per selected device and is a release gate, not an assumed property. Until
qualified, make the supported concurrency limit explicit (prefer one active
phone for initial hardware release rather than silently claiming multi-phone
support).

Reuse recovery wording in
[`gtk-bluetooth-ancs-app-spec.md`](gtk-bluetooth-ancs-app-spec.md): missing
ANCS, `Notifying=false`, timeout, user authorization, and iOS-side
forget/re-pair guidance. Never auto-forget, infer a timeout's method, or log
payloads.

## 5. Data, settings, and privacy

Persist only selected `(adapter address, device address)`, window geometry,
pane size, non-content filter preferences, alert preference, and the explicit
hidden-window preference. Never persist transient BlueZ object paths.
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
   - Test first: configure/build with the new option `OFF`; confirm no gtkmm
     package lookup occurs. Configure with it `ON` and GTK missing; require a
     direct actionable diagnostic. On the minimum supported Pop!_OS image,
     verify `pkg-config --modversion gtkmm-4.0`.
   - Implement: option default `OFF`, conditional subdirectory/target, GTK
     application skeleton, stable application ID, desktop entry and icon.
   - Verify: clean configure/build in option-OFF state; enabled build creates
     `dmn-bluetooth-gtk`; desktop-entry validation with
     `desktop-file-validate`; install into a temporary prefix and confirm
     executable, desktop file, icon, and schema paths. Launch twice and assert
     one primary `GApplication` instance/window activation, then quit.
   - Once implemented, the expected focused commands are:
     `cmake -S . -B build-no-gui -DENABLE_APP_BLUETOOTH_GTK=OFF`,
     `cmake -S . -B build-gui -DENABLE_APP_BLUETOOTH_GTK=ON`,
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
     requests monitor shutdown first.
   - Implement: `gtk/` application/window and empty-state placeholders using
     standard GTK widgets. Keep all GTK objects on the main context.
   - Verify: run `dmn-test-bluetooth-gtk-smoke` under a display; assert window
     exists, expected accessible names/roles exist, and repeated activation
     does not create duplicate controllers. Manual review checks theme
     behavior and window resizing only after automated assertions pass.
5. **Inventory, selection, and preferences.**
   - Test first: paired/unpaired/disconnected/unavailable rows, two adapters
     with matching device names, stable adapter+device key persistence,
     checkbox draft, save, discard, and app restore. Assert startup and
     checkbox toggles cause zero Pair/Connect calls.
   - Implement: device list view, refresh, save/discard, GSettings schema and
     preferences. Persist no BlueZ object path.
   - Verify: model and widget tests against a fake inventory; install schema
     in a temporary schema directory and run
     `glib-compile-schemas --strict`; round-trip non-content settings and
     inspect storage for absence of titles/messages.
6. **Start/stop lifecycle.**
   - Test first: explicit start, each progress state, pairing/connection
     failure, ANCS-not-found, Retry, stop while Ready, stop while start is
     blocked, stop all, close confirmation, hidden-window opt-in, quit,
     callback quiescence, and non-disconnection on stop.
   - Implement: status binding and managed worker execution. Stop while
     startup is running is a queued stop, not cancellation; the UI wording
     must match.
   - Verify: controller fake tests prove operation ordering; widget tests
     assert status strings and sensitivity. A timed fake start must not block
     main-loop heartbeat or window interaction.
7. **Event feed and filtering.**
   - Test first: arrival ordering with wall-clock rollback, stable sequence,
     each filter independently and composed, case-folding, missing fields,
     Details content, Removed behavior, no-results, no-events state, clear,
     overflow notice, and all payload retention boundaries.
   - Implement: `GListModel`/`GListStore`, `GtkFilterListModel`,
     `GtkSortListModel`, `GtkColumnView`, row activation/detail view. Consider
     using gtkmm wrappers for these objects; do not put view positions in the
     domain model as stable event identity.
   - Verify: no-display core tests plus GTK widget tests with synthetic
     events. Under Xvfb, wait for drawing before checking realized/layout
     state; test keyboard row activation and filters. Run AT-SPI checks for
     accessible table/list names and row content.
8. **Optional desktop alert sink.**
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
9. **Packaging, accessibility, and hardware gate.**
   - Test first: install/uninstall manifest, no stray GTK runtime in default
     install, schema and desktop file resolution, all controls keyboard-
     reachable, dynamic statuses announced, high-contrast labels, and no
     payload in settings/logs/alerts.
   - Verify: run CMake/CTest option OFF and ON; run GUI tests with a virtual
     display in CI, AT-SPI tests in a desktop session, and manual Orca review.
     Opt-in real-iPhone qualification records Pop!_OS, desktop, BlueZ,
     adapter, phone, and iOS versions. Fake-bus tests are not hardware proof.

## 7. Acceptance criteria

- Default project build remains free of GTK dependencies; opt-in build,
  package, and app tests pass.
- UI reflects typed monitor state and stays responsive through slow setup and
  stop.
- A window hidden behind other windows still receives events and, if opted
  in, submits alerts while the process is alive.
- Closing/quit behavior matches the selected preference; process exit stops
  monitoring and does not disconnect/unpair.
- Alerts are optional, generic by default, withdrawable best-effort, and
  never the authoritative event record.
- Unit, fake-bus, private-bus, packaging, accessibility, and opt-in hardware
  gates pass before release claims are made.

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

## 9. Research references

- GTK [`GtkApplication`](https://docs.gtk.org/gtk4/class.Application.html),
  [`GtkApplicationWindow`](https://docs.gtk.org/gtk4/class.ApplicationWindow.html),
  [`GtkColumnView`](https://docs.gtk.org/gtk4/class.ColumnView.html), and
  [list widgets](https://docs.gtk.org/gtk4/section-list-widget.html).
- GTK [test initialization](https://docs.gtk.org/gtk4/func.test_init.html),
  [wait for widget drawing](https://docs.gtk.org/gtk4/func.test_widget_wait_for_draw.html),
  and [accessibility](https://docs.gtk.org/gtk4/section-accessibility.html).
- GLib [unit testing framework](https://docs.gtk.org/glib/testing.html) and
  [GListModel](https://docs.gtk.org/gio/iface.ListModel.html).
- gtkmm 4 [documentation](https://gnome.pages.gitlab.gnome.org/gtkmm-documentation/):
  C++ bindings for GTK; verify package/version availability on the target
  distribution.
- GIO [`GNotification`](https://docs.gtk.org/gio/class.Notification.html),
  [`send_notification`](https://docs.gtk.org/gio/method.Application.send_notification.html),
  and [`withdraw_notification`](https://docs.gtk.org/gio/method.Application.withdraw_notification.html).
- Freedesktop [Desktop Notifications basic design](https://specifications.freedesktop.org/notification/latest/basic-design.html)
  and [D-Bus protocol](https://specifications.freedesktop.org/notification/latest/protocol.html).
- Freedesktop [Desktop Entry Specification](https://specifications.freedesktop.org/desktop-entry/latest/).
- GTK [Human Interface Guidelines](https://developer.gnome.org/hig/) as
  cross-desktop interaction guidance, not a GNOME Shell API dependency.
- GNOME [accessibility guidelines](https://developer.gnome.org/documentation/guidelines/accessibility.html):
  AT-SPI and assistive-technology interaction guidance for final integration
  checks.
