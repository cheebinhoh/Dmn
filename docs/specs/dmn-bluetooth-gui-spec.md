# Dmn Bluetooth GUI Design and Specification Index

**Status:** Proposed; no graphical frontend or user-session monitor service is
implemented.

This document is the entry point for evaluating graphical frontends for Dmn's
existing BlueZ/ANCS monitor. It replaces the former GTK-only proposal:
the product is a Bluetooth notification GUI, while GTK, GNOME Shell, and
COSMIC are distinct presentation/integration choices.

## Recommendation

Build the first complete user-facing experience as a standalone GTK 4
application. Keep the event feed and device lifecycle in that application,
use the existing C++ Dmn monitor, and optionally send privacy-safe desktop
alerts through GIO `GNotification` when the user explicitly enables them.
The window may be hidden while the application remains running; alerts require
the process and monitor session to remain alive. By default closing/exiting
stops monitoring; an explicit **Keep monitoring when the window is closed**
preference may hide the window and keep that app process alive.

Do not start with a GNOME Shell extension or a COSMIC applet: they provide
small shell surfaces, not a portable home for a detailed event table. They
may later be thin, desktop-specific launch/status controls. A separate
background D-Bus service is **optional**, not a prerequisite for the standalone
GTK application or a launcher-only shell extension/applet. Add it only if
monitoring must outlive the UI process or multiple independent frontends need
to share the monitor and event state.

The freedesktop notification protocol lets an app submit passive alerts; it
does not provide an API to read/subscribe to the desktop's notification
history or embed an application feed into the notification center.

## Shared product boundary

- Reuse `Dmn_BluezClient`, `Dmn_AncsBluezMonitor`, and owned
  `Dmn_AncsEvent` values. Do not shell out to `dmn-bluetooth-cli` or duplicate
  ANCS, GATT, pairing, or BlueZ logic in a frontend.
- Treat ANCS as a live event source, not a complete or durable iPhone
  notification archive. The app's own bounded, in-memory event list is the
  source of truth for the current session.
- Do not persist, log, transmit, or expose message content through alerts by
  default. Saved device identities and ordinary UI preferences are separate
  from notification content.
- Keep pairing, connection, monitor start/stop, alert delivery, and app
  shutdown explicit and observable. A BlueZ `Connected` property is not ANCS
  readiness.
- Treat multiple simultaneous `Dmn_AncsBluezMonitor` instances as unqualified
  until concurrency, cleanup, and hardware behavior have passed the release
  gates in the frontend specs.
- Resolve saved targets by stable adapter and device Bluetooth addresses;
  never persist or rely on transient BlueZ object paths or ambiguous display
  names when an address is available.
- GUI code must keep bounded blocking monitor startup/stop work off the UI
  thread. Event/progress callbacks must be copied and marshalled safely to
  the UI context. `stop()` waits for a synchronous `start()`; it does not
  cancel discovery, pairing, or connection in progress.

## Architecture options and decision

| Option | Role | Decision |
| --- | --- | --- |
| Standalone GTK 4 app with optional GIO desktop alerts | Full device manager and live event reader; can send passive alerts while still running with its window hidden | **Recommended first implementation**; see [GTK application spec](dmn-bluetooth-gtk-app-spec.md). |
| GNOME Shell extension | GNOME-only panel menu/quick actions that may activate the app or, later, call a service | Optional companion only; it cannot run on COSMIC and is not a plug-in for the GNOME notification center. See [GNOME extension spec](dmn-bluetooth-gnome-extension-spec.md). |
| Native COSMIC app and COSMIC applet | Rust/`libcosmic` native window and optional panel surface | Alternative if COSMIC integration is the priority; requires a maintained C++ boundary. See [COSMIC spec](dmn-bluetooth-cosmic-spec.md). |
| User-session D-Bus monitor service | Owns monitor lifetime/state for multiple frontends or after a window/process exits | Optional independent topic, not a default dependency; see [service spec](dmn-bluetooth-session-service-spec.md). |

## Feasibility for test-driven, spec-driven implementation

All four designs are feasible to implement incrementally with agentic AI if
each task is bounded by a numbered acceptance criterion, a fakeable boundary,
and an executable verification command. They are not equally automatable:

| Surface | Feasibility | Reliably automatable | Must remain a separate qualification |
| --- | --- | --- | --- |
| GTK application | **High** | Core/model and controller behavior with GoogleTest; GTK widget/action checks with a display; package/build-gate checks; AT-SPI queries for accessible controls | Visual quality, screen-reader usability, target desktop integration, and iPhone/BlueZ behavior |
| GNOME Shell extension | **Medium** | Pure GJS state/client modules, launcher behavior, and fake-service protocol tests | Shell popup lifecycle and API compatibility require an actual declared GNOME Shell session; no version-independent extension UI test contract is assumed |
| COSMIC app | **Medium-high** | Rust model/reducer and boundary contract tests; headless widget interaction only when the pinned `libcosmic` version works with the chosen simulator | Target COSMIC session, package/launcher behavior, and any FFI/D-Bus integration not exercised by fakes |
| COSMIC applet | **Medium** | Popup state mapping and service-client tests | Panel orientation, sizing, popup lifecycle, and shell integration on each supported COSMIC release |
| Optional D-Bus service | **High** | Contract/introspection tests, pure state tests, and private-session-bus tests with delayed/failing fake monitors | Same-user threat assumptions, service packaging/session lifecycle, and real BlueZ/ANCS integration |

These ratings describe testability, not implementation cost or product
priority. GTK is the recommended first frontend because the main workflow can
be tested in the existing C++/GoogleTest/CTest environment, with a small
display-backed layer for GTK-specific behavior. The existing backend private
bus tests can exercise the real monitor adapter without a physical phone.
GNOME and COSMIC remain viable but add independently versioned runtimes and
shell-specific release gates.

### Gaps to close before implementation

1. Declare and test the minimum supported GTK/libadwaita, GNOME Shell/GJS, and
   COSMIC/libcosmic versions. Check package availability and CI image support
   before choosing a release matrix; upstream web documentation is not a
   compatibility guarantee for a distribution release.
2. Keep the proposed GTK build option separate from the existing
   `ENABLE_APP_BLUETOOTH`, which currently builds `dmn-bluetooth-cli`.
   `ENABLE_APP_BLUETOOTH_GTK` is a new, proposed opt-in target and must not
   silently rename, replace, or build the CLI. Its dependency behavior is
   specified in the GTK application spec.
3. For each milestone, link requirement IDs to focused tests and distinguish
   automated evidence from human/hardware qualification. A test name or
   screenshot alone is not evidence that the required behavior is covered.
4. Keep platform-specific build and test dependencies optional. CI must
   include the default/headless configuration and each enabled frontend
   configuration; an unavailable GNOME/COSMIC shell runner must be reported
   as an unrun integration gate, not a passing test.
5. Keep payload-bearing event access in the full application. Shell surfaces
   need only payload-free state/count signals and must not subscribe to or
   request event contents.

### Layout feasibility and standard compliance

The proposed layouts are implementable with the selected toolkit, but the
wireframes are interaction sketches, not evidence of platform compliance:

| Layout | Assessment | Required design adjustment |
| --- | --- | --- |
| GTK/libadwaita main window | Feasible as a wide split view with a phone list and event feed | At narrow widths, collapse to a single navigable view; keep alert controls in Preferences rather than a persistent feed card. Use GTK/libadwaita theme styling, standard controls, and GNOME HIG keyboard/accessibility/adaptive rules. |
| GNOME Shell extension | Feasible only as a compact Shell menu, not a full application panel | Use standard Shell menu items and actions; the extension remains launcher-only unless the optional service is present. Test on declared Shell releases. |
| COSMIC application | Feasible as a native `libcosmic` list/detail application | Use COSMIC theme/spacing/widget conventions and an explicit narrow-window layout; validate keyboard and accessibility behavior against the pinned `libcosmic` revision. |
| COSMIC applet | Feasible as a small, optional status/action popup | Keep it payload-free and compact; do not attempt to squeeze the full device/event workflow into the panel. |

No spec can certify an app as GTK/GNOME/COSMIC-compliant before it is
implemented and checked on declared platform versions. Each frontend's
acceptance criteria therefore require a platform-specific design review,
keyboard/accessibility checks, and shell/package verification; COSMIC
accessibility is an explicit risk because upstream `libcosmic` currently
describes its `a11y` feature as experimental.

The detailed specs below define test seams and milestone checks. Before a
coding milestone, the implementer must record the selected platform/version,
requirement IDs under test, target commands, and any unavailable environment
as explicit assumptions. If a test seam cannot be implemented without
changing a public Dmn API, stop at the adapter boundary and propose that API
change separately instead of faking proof in the UI test.

## UI scratch artifacts

These are interaction/layout wireframes, not polished visual designs or
pixel-perfect toolkit mockups:

- [GTK wide-window layout sketch](images/dmn-bluetooth-gtk-app.svg)
- [GNOME Shell extension menu](images/dmn-bluetooth-gnome-extension.svg)
- [COSMIC application and applet](images/dmn-bluetooth-cosmic.svg)
- [Optional background service ownership diagram](images/dmn-bluetooth-session-service.svg)

## Shared implementation order

1. Preserve and test the existing BlueZ/ANCS API contracts before adding UI
   code; add no new GTK/COSMIC/GJS dependency to default builds.
2. Add an app-private, frontend-neutral presentation model and controller for
   owned/copyable values, bounded rows, host receipt time and a monotonic
   sequence, per-device lifecycle, and payload-free diagnostics. Do not place
   GTK/GObject types in this layer.
3. Build the UI in vertical slices: pure model tests first, fake backend next,
   then widgets and desktop integration. Make every step independently
   verifiable before moving on. Keep physical iPhone tests opt-in.
4. Keep toolkit dependencies and platform packaging isolated. Default Dmn
   CMake builds must not discover GTK, GJS, Rust, or COSMIC. Test both the
   option-OFF path and each explicitly enabled frontend.

## Recommended source and test layout

Use **`src/app/gui/gtk` for the GTK-specific application**, not `src/app/gtk+`
and not one `src/app/gui` directory containing every desktop toolkit. Keep
the existing CLI and other executable entry points in `src/app/`. Separate
the frontends that use different languages, toolchains, and packaging:

```text
src/
  app/
    dmn-bluetooth-cli.cpp              # existing terminal app; unchanged
    gui/
      CMakeLists.txt                   # only entered for GTK app option
      core/                            # private C++ UI-independent logic
        event-model.hpp/.cpp
        event-store.hpp/.cpp
        filter-state.hpp/.cpp
        monitor-controller.hpp/.cpp
        settings-model.hpp
      gtk/                             # GTK/libadwaita C API views and adapters
        main.cpp
        bluetooth-gtk-application.hpp/.cpp
        main-window.hpp/.cpp
        device-list-view.hpp/.cpp
        notification-view.hpp/.cpp
        notification-detail-view.hpp/.cpp
        gnotification-sink.hpp/.cpp
        settings-store.hpp/.cpp
  service/
    bluetooth/                         # only if optional D-Bus service is built
      main.cpp
      service-object.hpp/.cpp
      monitor-registry.hpp/.cpp
      dbus-contract.xml
apps/
  cosmic/                              # independent Cargo package/workspace
  cosmic-applet/                       # separate package; depends on service
desktop/
  gnome-extension/                     # GJS extension and metadata/package
data/
  applications/                        # desktop entry and icons
  gsettings/                            # GTK app schema, if selected
test/
  gui/
    core/                               # pure GTest unit tests; no display
    gtk/                                # GTK model/widget and app tests
  service/                              # private-session-bus tests
```

The actual filenames may be grouped differently; preserve the boundaries:

- `core/` is C++ and GTK-free. It can link to Dmn's public monitor/event
  types, but should accept injected monitor factories, clocks, executors, and
  alert sinks so model/controller tests do not require a display, Bluetooth,
  or the desktop notification daemon.
- `gtk/` is the only C++ directory that includes GTK/libadwaita APIs. It translates
  core state into widgets and translates user actions into controller
  requests. GTK models/widgets are created and mutated only on the GTK main
  context.
- `desktop/gnome-extension/` is separate because GNOME Shell loads GJS, not
  the C++ app target. It should not be a subdirectory compiled by CMake.
- Each COSMIC Cargo package is independent of CMake and has an explicit
  C++-FFI or D-Bus boundary. Do not let `src/app/gui/core` become a Rust/C++
  bridge accidentally.
- A service is a separate process/target, not a toolkit layer. Its code may
  reuse a Dmn monitor-controller library, but it owns monitor lifetime only
  when the service option is enabled.
- Keep tests adjacent by layer, not by screen. Prefer `test/gui/core`,
  `test/gui/gtk`, `test/service`, and a separate extension/Cargo test suite.
  If maintaining the current flat `test/` convention is easier, use
  unambiguous test executable names and keep these same dependency boundaries.

This is intentionally a **small layered design**, not a generic cross-toolkit
GUI framework. For the recommended GTK implementation, begin with only
`core/` plus `gtk/`; add the GNOME extension, COSMIC packages, or service
directories only when implementing those separately gated options.

## What can be automated

- **Pure unit/model tests:** Fully automated, fast, no UI display. Test
  selection draft/save/discard, state transitions, event normalization,
  ordering, filtering, bounded retention, error mapping, and privacy.
- **Controller/backend tests:** Fully automated with injected fakes and the
  repository's private D-Bus/BlueZ fixtures. Verify calls made/not made,
  progress, errors, thread handoff, operation serialization, callback
  quiescence, and stop/shutdown behavior.
- **Toolkit widget tests:** Automate deterministic actions and observable
  widget state. GTK 4 provides `gtk_test_init()` and GTK test helpers on top
  of GLib's `g_test` framework; keep this separate from this repository's
  GoogleTest binary initialization. Existing GTest-based widget tests can
  initialize GTK and use GTK's drawing/main-context helpers. Run under a
  virtual display when a display connection is required. These tests do not
  prove usability or accessibility.
- **Accessibility tree/integration tests:** Use AT-SPI-aware automation
  (for example, Orca plus an accessibility inspector or dogtail/pyatspi in a
  GNOME test session) to find controls by accessible role/name, activate
  buttons, and assert labels/status. Keep this optional-tooling job separate
  from fast unit tests.
- **Desktop/shell integration:** Run isolated GNOME Shell or COSMIC sessions
  in a VM/test machine and verify installation, activation, popup/widget
  behavior, and teardown. GJS pure-module tests and service mocks can run
  automatically outside Shell; GNOME Shell widget APIs require a Shell
  session, and there is no version-independent extension UI automation
  contract. COSMIC state/reducer tests run as Rust tests; headless widget
  simulation depends on the exact iced/libcosmic version. Shell-specific UI
  automation is version-sensitive; do not make it the sole correctness suite.
- **Physical hardware:** Human-assisted, opt-in qualification is appropriate
  for iPhone pairing prompts, ANCS availability, permissions, and radio
  behavior. Record versions and outcomes; do not use physical hardware as a
  substitute for automated fake-backend tests.

Automate all behavior that has a stable state transition or accessible
observable. Reserve human review for visual polish, interaction quality, and
physical device behavior. Never write tests that need a person to click
through routine repeated setup if the same action can be injected into the
model/controller or driven through the accessibility tree.

## Agentic implementation handoff

Each detailed frontend spec is intended to be usable as a sequence of bounded
agent/human work items, not as a one-shot instruction to generate the whole
application. For each numbered stage:

1. Give the implementer only the stage plus the relevant shared contract and
   file boundary.
2. Require a failing automated test for the acceptance behavior before
   implementation.
3. Require the smallest change that passes that test, with compilation and
   the existing relevant tests still passing.
4. Require a concise evidence report listing files, commands, test results,
   and unverified desktop/hardware assumptions.
5. Do not begin the next stage until the current stage's acceptance checks
   pass. Do not report a screenshot/manual smoke test as a substitute for
   model, controller, or integration tests.

The implementation plans in the frontend specs include a test and an
independent verification action for every milestone. They are detailed enough
to guide such staged implementation; platform version and package availability
must still be confirmed at the start of each platform-specific project.

### Reusable work-item template

Use this for each agentic implementation request:

```text
Implement only milestone <N: title> from <frontend spec>.
Read the shared contracts in dmn-bluetooth-gui-spec.md and the relevant
backend/API spec. Do not implement later milestones.

Allowed areas: <specific directories/files>.
Requirement IDs: <specific AC IDs from the frontend spec>.
Behavior: implement only those acceptance criteria.
Test first: add or run the focused failing test(s) for <assertions>.
Implementation: make the smallest change that passes those tests.
Verification: run <targeted build>, <focused test command>, and any
step-specific package/UI check from the spec.

Do not add unrelated refactors, widen the default build dependencies, invent
UI/API state, or claim desktop/hardware compatibility that was not tested.
Report changed files, commands/results, and unresolved assumptions.
```

The human or coordinating agent should review the work-item diff and test
evidence before issuing the next milestone. For final toolkit-specific
behavior, combine automated evidence with a short human acceptance checklist;
do not use manual clicking in place of tests for deterministic state logic.
Acceptance IDs use the prefix in each spec (`GTK-AC`, `ANCS-GUI-AC`,
`GNOME-AC`, `COSMIC-AC`, or `SERVICE-AC`). Every ID must map to at least one
automated test or be explicitly marked as a human/hardware qualification; do
not silently drop criteria while splitting work into agent-sized tasks.

## Related specifications and evidence

- [`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md): reusable
  backend-neutral ANCS API.
- [`gtk-bluetooth-ancs-app-spec.md`](gtk-bluetooth-ancs-app-spec.md):
  observed iPhone/BlueZ failure conditions and user-facing recovery language.
- [`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md): BlueZ
  inventory and discovery behavior.
- [Freedesktop Desktop Notifications](https://specifications.freedesktop.org/notification/latest/):
  sender-to-desktop alert protocol; not a notification-history query API.
- [GIO `GNotification`](https://docs.gtk.org/gio/class.Notification.html):
  desktop notification API, including lifecycle and application-identity
  requirements.
- [GNOME Shell extensions overview](https://wiki.gnome.org/Projects/GnomeShell/Extensions):
  GNOME-specific JavaScript/GJS and CSS shell customization.
- [COSMIC application template](https://github.com/pop-os/cosmic-app-template),
  [COSMIC applet template](https://github.com/pop-os/cosmic-applet-template),
  and [libcosmic book](https://pop-os.github.io/libcosmic-book/): Rust-based
  application/applet toolchain and design framework.
- [`iced_test` simulator documentation](https://docs.rs/iced_test/latest/iced_test/):
  example of headless widget interaction for iced; use only if compatible with
  the pinned libcosmic build, not as an assumed COSMIC test guarantee.
- [GNOME accessibility guidelines](https://developer.gnome.org/documentation/guidelines/accessibility.html):
  AT-SPI context for automated accessible-control checks.

Research references and implementation assumptions were reviewed on
2026-10-18. Desktop-specific APIs and packaging change over time; each
frontend specification requires validating the actual supported desktop
release before implementation.
