<!-- Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved. -->

# Dmn Bluetooth Native COSMIC Application and Applet Specification

**Status:** The native `libcosmic` application (`dmn-bluetooth-cosmic`) is
specified in [section 1A](#1a-implemented-native-application-architecture) and
implemented under `src/app/gui/cosmic/`; hardware and target-session
qualification remain. The COSMIC panel applet and the user-session D-Bus
service remain proposals. Neither is required for the standalone GTK app.

This document covers two related but separately packaged COSMIC integrations:
a native `libcosmic` application for the full workflow and an optional COSMIC
panel applet for concise status/quick access.

![Illustrative wide COSMIC app layout and optional panel applet](images/dmn-bluetooth-cosmic.svg)

## 1. Platform and architecture decision

COSMIC applications and applets use the Rust-based `libcosmic` toolkit and
COSMIC templates. This gives a COSMIC-coherent appearance and a supported
COSMIC applet mechanism, but the existing ANCS/BlueZ monitor is C++.

Two boundaries were evaluated:

1. **Rust-to-C++ FFI (selected for the native application):** a narrow,
   exception-safe C ABI around the existing C++ monitor and GUI core. Every
   value that crosses the ABI is an owned, NUL-terminated UTF-8 JSON string; no
   STL type, exception, callback, or borrowed pointer crosses it.
2. **User-session D-Bus service (deferred):** the preferred boundary only if
   several frontends (for example app plus applet) must share one live monitor
   or monitoring must outlive a window. See
   [`dmn-bluetooth-session-service-spec.md`](dmn-bluetooth-session-service-spec.md).

Do not duplicate BlueZ/GATT/ANCS in Rust and do not let an applet independently
connect to BlueZ. A panel applet alone is not suitable for a detailed event
archive; the applet needs shared state through the service.

## 1A. Implemented native application architecture

### Build direction: Cargo drives CMake

Cargo is the only entry point for building the application. CMake never
invokes Cargo or Rust.

```text
cargo build / cargo test
  +- crates/dmn-bluetooth-sys/dmn-build.rs
       +- cmake -S <repo> -B <OUT_DIR>/cmake -DENABLE_APP_BLUETOOTH_COSMIC=ON
            +- builds target dmn-bluetooth-cabi (libdmn-bluetooth-cabi.so)
       +- emits cargo:rustc-link-* so Rust links the C library
```

- `ENABLE_APP_BLUETOOTH_COSMIC` (default `OFF`) is the only gate for the C++
  side. It enables the ANCS/BlueZ/D-Bus layers, requires GLib (used by GUI
  core), and defines `dmn-bluetooth-cabi`. With the option off the target does
  not exist, and default, headless, GTK, and CLI builds never need Rust or
  `libcosmic`. A developer may also configure CMake with the option directly to
  build and test the C library without Rust.
- `dmn-build.rs` passes the option, a build type matching the Cargo profile,
  `DMN_BUILD_TESTS=OFF`, and `ENABLE_DMN_DMESGNET=OFF`, and configures into
  Cargo's `OUT_DIR`, so the source tree stays clean. It fails with an actionable
  message if `cmake` is missing and re-runs when C++ sources change.
- The library is shared so its transitive static C++ dependencies (ANCS,
  BlueZ, D-Bus, GUI core) need no hand-maintained link list in Rust. The
  executable's RPATH points at the library's build directory; an installed
  package places the library beside the binary.

### Reuse boundary

| Layer | Source | Reuse |
| --- | --- | --- |
| BlueZ inventory/connect/discovery | `Dmn_BluezClient` | Linked unchanged |
| ANCS monitor lifecycle | `Dmn_AncsBluezMonitor` | Linked unchanged |
| Selection, draft/save, sorting, action availability | `dmn-bluetooth-device-core` | Linked unchanged (GLib-free) |
| Retry/backoff and monitor reconciliation | `Dmn_DeviceMonitorController` | Linked unchanged |
| Engine (orchestration, no GTK/GLib) | `src/app/engine/` | Shared by the CLI, GTK and COSMIC apps |
| JSON protocol | `include/app/engine/` | New |
| Presentation and persistence | `src/app/gui/cosmic/` | New (Rust) |

The GTK app and `dmn-bluetooth-cli` also run on this engine. The engine lives
outside `src/app/gui` because it is not GUI-specific; `dmn-bluetooth-gui-core`
holds only GLib-based GUI helpers (alert policy, event store).

### C ABI (`include/app/gui/cabi/dmn-bluetooth-cabi.h`)

```c
dmn_bt_engine *dmn_bt_engine_create(void);       /* real BlueZ/ANCS backend */
dmn_bt_engine *dmn_bt_engine_create_demo(void);  /* synthetic, no hardware */
int   dmn_bt_engine_send(dmn_bt_engine *, const char *command_json);
char *dmn_bt_engine_next_message(dmn_bt_engine *, int timeout_ms);
void  dmn_bt_string_free(char *);
void  dmn_bt_engine_destroy(dmn_bt_engine *);
const char *dmn_bt_version(void);
```

- The ABI is pull-based. The frontend blocks on `next_message` in its own
  thread, so no C++ thread ever calls into Rust and "no callback after drop"
  holds by construction.
- Functions catch every C++ exception. `send` returns 0 when the command was
  accepted and non-zero for malformed input or a closed engine.
  `next_message` returns `NULL` on timeout or after the engine closed and the
  queue drained. Returned strings are caller-owned and freed only with
  `dmn_bt_string_free`.
- `destroy` stops monitors and joins all worker threads before returning;
  callers must not race it with other calls on the same handle.
- The demo backend provides deterministic synthetic phones and notifications so
  the UI, Docker, and CI run without Bluetooth hardware. It is a documented
  product mode (`dmn-bluetooth-cosmic --demo`), not a test-only symbol.

### Protocol v1 (UTF-8 JSON, one object per message)

Frontend to engine (`type`): `hello`, `configure` (restore saved selection,
monitoring intents, sort), `refresh`, `scan` (`enabled`, optional `adapter`),
`select` (draft), `save_selection`, `discard_selection`, `sort`, `connect`,
`disconnect`, `monitor` (`enabled`), `monitor_direct` (`device`, optional
`adapter`; CLI one-device monitor that discovers, pairs and connects), `stop_direct`,
`shutdown`. Device commands carry stable
`adapter` and `device` Bluetooth addresses, never object paths.

Engine to frontend (`type`): `ready` (protocol and engine version), `state`
(full snapshot: adapters, scanning, busy, status text, unsaved flag, and per
device identity/name/status text/paired/connected/present/saved/selected,
monitor requested/state/detail, and action availability computed by the shared
GUI core), `event` (sequence, receive time, device identity, kind, optional
state/notification/error), `persist` (saved selection and monitoring intents to
store), `direct_monitor` (`state`: progress, ready, failed, stopped; failure
adds `code`, `detail`, `remote_name`, `cancelled`), `error` (message), `bye`.
`event` messages carry a `terminal` flag and notifications an `event` name.

Unknown fields and unknown `type`/`kind` values must be ignored by both sides
so v1 can grow compatibly. A protocol major-version mismatch is reported in
`ready` and refused by the frontend.

### Engine behavior

The engine mirrors the GTK semantics in the
[GTK specification](dmn-bluetooth-gtk-app-spec.md): launch and selection never
pair or connect; monitoring is per saved, paired, connected device; intent is
persisted until the user stops monitoring or explicitly disconnects; failed
starts and unexpected ANCS failures retry with jittered exponential backoff
through `Dmn_DeviceMonitorController`; events are ordered by an engine-assigned
arrival sequence. Bounded blocking work (BlueZ operations, monitor start/stop)
runs on worker threads and never on the thread that reads commands. The
engine never writes to disk or logs notification content; the frontend owns
persistence and stores only stable identities and UI preferences.

### Rust workspace (`src/app/gui/cosmic/`)

```text
Cargo.toml                         # workspace, pinned libcosmic revision
crates/dmn-bluetooth-sys/          # dmn-build.rs (drives CMake), FFI, safe Engine
crates/dmn-cosmic-model/           # pure protocol types, reducer, feed, filters
app/                               # binary dmn-bluetooth-cosmic (libcosmic UI)
data/                              # desktop entry, AppStream metadata, icon
```

The model crate has no UI or FFI dependency, so `cargo test -p
dmn-cosmic-model` runs without a shell or C++ build. The sys crate's tests
drive the real C library through the demo backend.

### Docker and CI

The repository `Dockerfile` installs a current Rust toolchain and the
`libcosmic` system packages and runs `cargo test --workspace` (which builds the
C library through `dmn-build.rs`) during the image build. The container entry point
runs the same Cargo tests after `ctest`. The default CMake configuration in the
image is unchanged and does not enable `ENABLE_APP_BLUETOOTH_COSMIC`.

### Acceptance additions

- **COSMIC-IMPL-01:** A default CMake configure defines no COSMIC target;
  `ENABLE_APP_BLUETOOTH_COSMIC=ON` defines only `dmn-bluetooth-cabi` and its
  tests.
- **COSMIC-IMPL-02:** `cargo build` from a clean checkout builds the C library
  without a manual CMake step.
- **COSMIC-IMPL-03:** C ABI tests cover create/destroy, malformed commands,
  timeout, and the full demo workflow including shutdown with a live monitor.
- **COSMIC-IMPL-04:** The Docker image builds and `cargo test --workspace`
  passes without Bluetooth hardware.
- **COSMIC-IMPL-05:** The app starts in `--demo` mode on a Wayland COSMIC
  session; real iPhone/BlueZ behavior is a separate manual hardware gate.

Out of scope for this implementation: the applet, hidden-window monitoring, and
the session D-Bus service. Generic desktop alerts are implemented (see below).

### Toolkit and package layout

Use the official `cosmic-app-template` and `cosmic-applet-template` as
starting points, pin the Rust toolchain and `libcosmic` revision, and keep
application and applet artifacts independently installable. `libcosmic` is
Rust-based and built on iced; do not assume upstream iced examples or
`iced_test` are binary/source-compatible with the exact `libcosmic` revision.
The official template's `just build`, `just run`, and `just check` are useful
build/lint entry points; add deterministic `cargo test` suites for logic.

The implemented layout is the Rust workspace described above. If an applet is
added later, give it its own package under the same workspace.

Do not add empty packages before they are needed. The pure model and service
client should be Rust libraries with injected clocks/transports, allowing
`cargo test` without a running COSMIC shell or Dmn C++ runtime. Keep COSMIC
widgets in the app package and an applet's shell integration in its own package.

## 2. Native COSMIC application

### Goals and build/package boundary

The app provides the same domain workflow as the GTK specification while
using COSMIC widgets, themes, launcher metadata, and keyboard conventions.
Name it `dmn-bluetooth-cosmic`; do not replace `dmn-bluetooth-cli` or claim
GTK support. Keep it in a separate Rust/Cargo package and build gate so normal
Dmn CMake builds do not require Rust or `libcosmic`. Define supported COSMIC
versions and package using the actual COSMIC app template guidance. The
`libcosmic` project documents `com.system76.CosmicApplication` in metainfo for
COSMIC app identification; applications may be submitted to Flathub.
COSMIC-specific applets use `com.system76.CosmicApplet` and the COSMIC Flatpak
repository rather than treating app and applet as one package. Confirm current
store submission requirements at implementation time.

### Implemented layout

**Toolbar** (always visible): a device search box, the **Selected only**
checkbox, then **Save** and **Discard**. A header-bar menu holds the **Desktop
alerts** checkbox and **About Dmn Bluetooth**, which opens libcosmic's About
panel in the context drawer. The search box is half as wide as the
device panel (a quarter of the window when the panes are side by side) and
filters both device groups case-insensitively by name, address, adapter,
status, or monitor state. Save and Discard are enabled only while there are
unsaved selection changes.

**Panes.** At wide widths the window shows the device list and the
notification feed side by side; at narrow widths they stack vertically. The
header toggle hides the device panel (not persisted). Each pane scrolls
independently with its own scroll bar. Device cards and notification rows are
the same square cards and share one vertical gap (half of the standard
`space_xs` step).

**Device list.** Two groups: **Previous selected devices** (top, with the
Name/Status sort buttons) holds devices whose selection has been saved, and
**Nearby devices** (bottom) holds nearby or unselected devices. Grouping follows
the saved state: ticking a device in the bottom group moves it up only after
Save, and unticking a top device moves it down only after the unselection is
saved. A group heading sits 56% of `space_xs` above its list, and the two
groups are 130% of `space_xs` apart.

**Scanning.** The app scans for its whole lifetime, so there are no Scan or
Refresh controls. It sends `scan` with `enabled: true` whenever the engine
reports an adapter, no scan running and no operation in flight (retried at most
every 10 seconds), and the engine refreshes inventory as results arrive.

**Notification feed.** Newest first, with kind, device and text filters and a
search box. Each row is a card showing the title, the notification subtitle and
message, and the device name. Hovering a row shows the whole message in a
wrapped tooltip. The title and message can be highlighted with the mouse and
copied with the right-click Copy / Select All menu, as in the GTK app. There is
no separate detail view and rows are not clickable.

Feature behavior, privacy policy, the 1,000-row cap, filters, ANCS errors, and
test gates match the shared [GTK app spec](dmn-bluetooth-gtk-app-spec.md)
unless this document says otherwise. The interaction requirements below that mention a first-run
privacy explanation or an applet remain targets, not current behavior.

### Interaction requirements

- First run explains live-session ANCS behavior and privacy; fetch inventory
  and show paired, connected, and disconnected BlueZ-known devices.
- Checkbox changes are draft state. **Save selection** persists stable
  `(adapter address, device address)` identities; **Monitor selected** is
  disabled until the draft is saved. Launch and selection edits never pair
  or connect.
- **Start** reports inventory, optional discovery, pairing, connection, ANCS
  subscription, and Ready as distinct states. A per-device failure must not
  mislabel another device or implicitly stop it.
- Event list has **Device**, **Received**, **Type/category**, and
  **Notification**; filters cover device, event kind/category, and search.
  The COSMIC app has no detail surface; rows show the text and a hover
  tooltip shows it in full. Empty,
  unavailable, no-results, overflow, and permission-needed states are
  explicit.
- **Stop** ends the ANCS session only. Quit stops monitor and clears
  in-memory content; bonds/links remain. Generic desktop alerts (on by default,
  toggled in the header menu) are sent over `org.freedesktop.Notifications`
  with the same text and per-device replacement ids as the GTK app; clicking an
  alert does not raise the window. Hidden-window monitoring follows the GTK
  spec only if the selected platform integration is actually available.
- Persist UI preferences and selected stable identities only. Never persist,
  log, or export ANCS notification text by default.

Use COSMIC standard theme tokens and controls, scalable layouts, keyboard
navigation, screen-reader semantics, visible focus, and textual status in
addition to icons. Do not assume a GNOME panel, shell extension, system tray,
or notification API is present.

### Applet interaction requirements

The applet is a separately installable COSMIC component. Its compact icon
opens a popup with:

- **Dmn Bluetooth** and current service connection state;
- selected phone rows with concise textual states (Ready, Starting, Failed,
  Unavailable);
- an optional generic **N new events** count (user-controlled and off by
  default);
- **Open application**, **Start selected**, **Stop all**, and **Preferences**.

The applet does not display notification titles/bodies, full history, pairing
dialogs, or address-level settings. **Open application** activates
`dmn-bluetooth-cosmic`. Without a service, the applet must either be a
launcher-only control or explicitly show unavailable state; it must not
invent live status. If service connection is lost, disable mutations and
preserve **Open application**. The applet consumes only payload-free service
status and aggregate-count signals; it never subscribes to event payloads or
requests event history.

The wireframe shows both the full window and compact applet popup.

## 3. D-Bus service and alert relationship

COSMIC's documented app/applet toolkit is not a guarantee of GIO
`GNotification` integration or notification-center API parity. If desktop
alerts are required, isolate them behind an adapter, test the notification
service on the target COSMIC release, and keep alert delivery optional and
generic. No frontend may read notification-center history.

If a COSMIC app and applet need to share monitor state, prefer one service
owner rather than multiple independent C++ monitor instances. Define the
service API, version negotiation, caller permissions, event retention,
restart/shutdown, and privacy before applet implementation. The native app
may also use tested C++ FFI instead of a service when it is the sole frontend.

## 4. Test-driven implementation plan

### Native application

1. **Toolchain/template.**
   - Test: verify chosen compiler, template revision, minimum COSMIC release,
     packaging format, and a clean default Dmn CMake build without Cargo.
   - Implement: generate the official application template and pin the
     Rust/libcosmic versions.
   - Verify: template builds, launches, packages, installs, and removes on a
     clean supported Pop!_OS/COSMIC machine; run template `just check`.
2. **Boundary proof.**
   - Test: construct/destroy the C ABI wrapper or fake D-Bus client; validate
     owned event copies, typed errors, callback quiescence, finite operations,
     and (for D-Bus) version negotiation, owner loss, and malformed responses.
   - Implement: only the selected boundary. C ABI functions must catch all
     C++ exceptions and return owned values; no STL object or borrowed string
     may cross into Rust.
   - Verify: Rust contract tests plus C++ boundary tests; ASan/TSan or other
     configured sanitizers where available. Prove no callback after drop.
3. **Pure Rust presentation model.**
   - Test: event/status conversion, optional attributes, Removed events,
     arrival-sequence order across wall-clock changes, filters, 1,000-row
     eviction, and payload-free diagnostics.
   - Implement: `dmn-cosmic-model` as a display-free crate with injected clock
     and synthetic event fixtures.
   - Verify: `cargo test -p dmn-cosmic-model`; run `cargo clippy`/template
     `just check` and inspect test coverage for every state/enum.
4. **Inventory and selection.**
   - Test: paired/connected/unavailable rows, duplicate names across
     adapters, stable identities, draft/save/discard, settings round-trip,
     and no Pair/Connect call on launch or checkbox change.
   - Implement: inventory-facing model, saved preference values, then the
     COSMIC list/actions.
   - Verify: model tests run without shell; view tests with an injected fake
     inventory assert text and control enabled-state. Confirm settings
     contain no ANCS payload.
5. **Monitor lifecycle.**
   - Test: progress ordering, start failure, Retry, stop during start,
     shutdown, per-device error isolation, and cleanup. Document that the
     existing synchronous Dmn stop waits for start and is not cancellation.
   - Implement: controller/client calls without blocking the UI executor;
     make stop-during-start show “stop requested” until actual completion.
   - Verify: deterministic fake boundary test with delayed responses; assert
     app state loop continues and no callback uses a dropped view/controller.
6. **Event feed and accessible view.**
   - Test: filters, sort/arrival sequence, search, detail values, empty and
     no-results states, keyboard navigation, no payload persistence, and
     narrow/wide layout transitions without clipped actions.
   - Implement: responsive list/table and detail surface using `libcosmic`
     widgets/theme and accessible labels.
   - Verify: unit tests first; use `iced_test::Simulator` only if the pinned
     `libcosmic` version supports it without substituting a different widget
     tree. Otherwise use tested reducer/view-model actions plus shell-session
     smoke tests. Verify all controls with keyboard and screen reader on the
     target COSMIC release.
7. **Desktop packaging and alerts.**
   - Test: launcher activation, optional alert adapter disabled/off by
     default, generic alert text only, notification-service absence, and app
     shutdown.
   - Implement: only integration supported by the selected target COSMIC
     release. Keep passive alert submission behind an adapter and never make
     the notification center the event store.
   - Verify: `just check`, `cargo test --workspace`, and package install/
     removal. Test notification behavior on actual COSMIC; other desktops are
     not inferred to behave the same.
8. **Hardware gate.**
   - Test: retain fake backend coverage for known ANCS failure and recovery
     states.
   - Implement: no protocol work in the GUI; fix only demonstrated adapter/
     presentation bugs.
   - Verify: opt-in real iPhone tests with recorded COSMIC, BlueZ, adapter,
     phone, and iOS versions. Fake-bus success does not prove hardware support.

### COSMIC applet

1. **Applet template.** Build/install official template. Verify enable,
   disable, and remove without affecting the native app or shell.
2. **Popup model.** Test all state mappings, unknown future enum, service
   missing, count-hidden behavior, and no `GetRecentEvents` call with a fake
   service client. Verify the applet requests only payload-free status/count
   data and disconnected state is unavailable, never stale Ready.
3. **Popup UI.** Test keyboard, screen reader, popup open/close, panel
   orientation/size, theme, and zero payload text. Verify the pinned
   `libcosmic` accessibility feature is enabled and its exposed names/states
   work with the target COSMIC accessibility stack. Verify interactively on
   the target shell and automate model/control actions where `libcosmic`
   widget simulation supports the pinned version.
4. **Service calls.** Test authorization/error/timeout and ensure applet
   shutdown does not stop service-owned monitoring. Verify with a private
   session-bus test service and fake responses.
5. **Package lifecycle.** Test update/removal and declared COSMIC release
   compatibility using official template `just check`, `cargo test`, and
   package install checks.

## 5. Acceptance criteria

- **COSMIC-AC-01:** The native application remains useful without an applet,
  shell-specific component, or notification server.
- **COSMIC-AC-02:** The selected FFI or D-Bus boundary has automated ownership,
  shutdown, and error tests; no direct ANCS logic is duplicated in Rust.
- **COSMIC-AC-03:** The applet is optional, concise, payload-free, and reports
  unavailable rather than stale status.
- **COSMIC-AC-04:** UI and packaging pass on declared COSMIC versions; default
  Dmn CMake builds do not acquire COSMIC dependencies.
- **COSMIC-AC-05:** Both app and applet use `libcosmic` widgets/theme/spacing,
  support keyboard operation and responsive layouts, and expose accessible
  names/states through the verified accessibility feature on each supported
  release.

## 6. Research references

- System76 [COSMIC apps](https://system76.com/cosmic/apps) and
  [COSMIC UX/applets](https://system76.com/cosmic/ux).
- [`cosmic-app-template`](https://github.com/pop-os/cosmic-app-template) and
  [`cosmic-applet-template`](https://github.com/pop-os/cosmic-applet-template):
  templates recommend `cargo generate` and provide `just` build/check
  workflows.
- [libcosmic book](https://pop-os.github.io/libcosmic-book/): describes
  `libcosmic` as a Rust toolkit based on iced with COSMIC widgets, theming,
  and platform integration; Rust experience is expected.
- [Freedesktop Desktop Notifications](https://specifications.freedesktop.org/notification/latest/):
  alert sender protocol, not an event-history API.
- Related: [GUI overview](dmn-bluetooth-gui-spec.md), [GTK app](dmn-bluetooth-gtk-app-spec.md),
  and [optional session service](dmn-bluetooth-session-service-spec.md).
