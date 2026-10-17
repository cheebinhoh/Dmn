# Implementation Plan: Layered ANCS Client for Linux

**Status:** Phase 0 inventory/discovery and Phase 1A pairing plus Phase 1B
backend-neutral/BlueZ GATT implementation and private-bus gates are complete.
Phase 1C is implemented through reusable `Dmn_AncsBluezMonitor`; the CLI now
handles arguments, terminal presentation, signals, and output backpressure
while the library owns the selected-device workflow. The monitor separately asks
BlueZ to connect remaining auto-connectable profiles when the selected device
is connected but services are unresolved. A private-bus end-to-end regression
test covers the new path. The first production retry received
`org.bluez.Error.Failed: Operation already in progress`; the monitor now
treats only explicit already-connected/in-progress responses (including this
specific BlueZ message) as an active request and continues waiting for
service resolution. The updated production retry entered ANCS startup but
still timed out after 60 seconds. Read-only inspection confirms
`Connected=true`, `ServicesResolved=false`, and only a partial GATT object
tree. A user-approved disconnect/reconnect then allowed
`ServicesResolved=true`, but the production CLI exited because the resolved
GATT tree had no ANCS service. Read-only inspection found Generic Attribute
and two vendor-specific services, with no ANCS UUID. No ANCS subscription or
attribute exchange occurred. The next hardware step requires investigation of
why this iPhone/BlueZ connection does not publish ANCS; do not infer
authorization or compatibility from the link or resolved services. The phone
is bonded, so no pairing prompt is expected. A second target, “iPhone Genghis”
(iPhone 15 Plus, iOS 26.7.1), was paired by the user and left disconnected
without forgetting it. The production monitor reconnected it, resolved GATT,
found ANCS and its required characteristics, subscribed, and reached its
ready state. The first 100-second run ended without a notification event, so
an actual attribute round trip still needs to be observed. In a subsequent
240-second run, the user triggered a notification while the CLI was ready,
but no Notification Source event or Data Source attribute response appeared.
BlueZ still reported the phone connected with services resolved after the CLI
timed out. A fresh 120-second production-monitor run again reached ready but
ended without an event. In a later 300-second run, the user confirmed
triggering several SMS notifications that appeared on the iPhone screen while
the monitor was ready; output still contained only the ready message, and
BlueZ remained connected with services resolved. Because SMS is a valid iOS
notification type, the test type alone did not explain the missing ANCS
event. Those earlier Genghis attempts preceded the iOS permission grant. On
the successful physical test, the iPhone prompted the user to allow Pop!_OS
to receive notifications for Dmn running on another OS. No event arrived
before approval; after approval, the live CLI printed multiple notification
events and their available attributes. This confirms end-to-end ANCS delivery
on Genghis once notification access is authorized. A later run failed explicitly
when the ANCS GATT event queue exceeded its 256-event limit. This was fixed by
separating acknowledged Control Point writes from GATT event intake and giving
each worker its own condition variable; the normal monitor command now passed
focused unit/private-bus tests and a physical Genghis run. The library has no
lifetime notification-count cap: it defaults to a rolling 255 pending
attribute-request backlog that discards the oldest pending item with a
nonfatal resource-limit event, while setting
`Dmn_AncsConfig::maximum_pending_notifications` to zero (CLI `--unlimited`)
removes that pending-request cap. GATT event intake has no count-based cap.
The reusable library and CLI also provide an opt-in startup cutoff
(`notification_date_cutoff` / `--ignore-existing`) based on ANCS's
Date attribute; BlueZ itself has no notification timestamp. Missing or
unrecognized dates are suppressed in this mode, and accurate filtering
requires aligned host/iPhone clocks and timezones.
For a controlled test, a newly
delivered notification from any app
whose iOS notifications are allowed in Notification Center is valid; SMS/
iMessage is convenient but not required. Ensure Focus or Scheduled Summary
is not delaying delivery. The protocol parser, fake-GATT ANCS session client,
and initial `dmn-bluetooth monitor` implementation are in place.
Parser and session unit tests plus CLI help/argument tests pass. Private-bus CLI
tests now exercise explicit adapter/name selection, ambiguous-name rejection,
target-only Pair/Connect ordering, connection failure reporting, and rejection
of a connected device with no ANCS service. A fake-BlueZ end-to-end test now
establishes both ANCS subscriptions, writes an attribute request, streams the
response to CLI output, and verifies SIGINT cleanup. Output privacy/stderr
separation, notification escaping, and broken-stdout cleanup are now covered
by dedicated private-bus subprocess tests. SIGINT during blocked startup
cleanup and output-queue saturation are covered as well. An opt-in physical
device CTest exercises ANCS startup and shutdown through the production CLI;
it is disabled by default. Recent monitor attempts found “iPhone Old” paired
and connected but service resolution did not complete within 60 seconds; a
user-approved disconnect/reconnect then resolved the GATT tree but exposed no
ANCS service, so no subscription or attribute exchange occurred. The current
implementation and remaining gates
are tracked against
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md). The first,
hardware-independent Bluetooth deliverable is the BlueZ
inventory/discovery CLI described in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). This plan covers
later GATT/ANCS work and does not make iPhone feasibility a prerequisite for
shipping that first milestone.

## 1. Recommended minimal solution

The command-line application is the primary V1 interface. Keep it a thin
adapter over reusable Dmn APIs, not the owner of Bluetooth or ANCS behavior.
After the discovery milestone, build the selected-device workflow as:

```text
dmn-bluetooth CLI ─┐
                   ├─> Dmn_AncsBluezMonitor
future GTK GUI ────┘       ├─> Dmn_BluezClient
                           ├─> Dmn_AncsClient
                           └─> Dmn_BluezGattClient
                                  └─> Dmn_GattClient contract
                                         └─> BlueZ -> BLE -> iPhone
```

`Dmn_AncsBluezMonitor` owns the current selected-device orchestration and event
delivery boundary; the CLI and future GTK frontend are adapters for argument
parsing, presentation, and UI-thread marshalling only. The monitor reuses the
existing `dmn-bluez` and backend-neutral ANCS surfaces and never pairs or
connects unrelated scan results. Its optional progress callback reports setup
phases on the thread calling synchronous `start()`; `stop()` waits for startup
and does not cancel discovery, pairing, or connection. GTK must invoke startup
outside the UI thread and marshal progress and ANCS callbacks onto the GTK
context. It must not spawn the CLI, parse terminal output, or depend on CLI
output as an internal protocol. Neither frontend promises every nearby radio
device. Do not start by building a system daemon, notification database,
libnotify dependency, or IPC protocol.

Implement the BlueZ backend as part of the Dmn codebase and call the public
`libdbus` C API directly to use BlueZ's documented system-D-Bus API. Do not
add a third-party Bluetooth/GATT wrapper, D-Bus wrapper such as GDBus, or
another BLE stack. `libdbus` is the direct transport dependency, not a
Bluetooth abstraction. Keep this optional Linux feature separate from the
existing Dmn D-Bus signal transport: `Dmn_DBusInput`/`Dmn_DBusOutput` do not
implement BlueZ method calls, object/property access, or GATT notifications.
The backend-neutral GATT interface is the sole intended extension point for a
future backend; do not add a plugin framework. Keep UI frameworks out of
library targets. Blocking BlueZ operations must run away from a GTK main
thread; callbacks use documented serialized execution contexts, and a GTK
adapter must marshal UI updates onto its main context.

## 2. Phases and exit gates

### Phase 0 — BlueZ inventory and `--discovery` CLI (first deliverable)

Implement the separate contract in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). This is the
first independently useful Bluetooth feature and does not require a phone,
GATT, ANCS, or a physical adapter for normal CI.

**Implementation status:** The `dmn-bluez` inventory/discovery API and
`dmn-bluez-discovery` CLI are implemented and Phase 0 qualified. The private
bus tests, standalone-header check, build matrix, and physical adapter smoke
record are documented in the linked specification.

- Implement adapter/device inventory and live changes over the generic
  `Dmn_DbusClient`; enumerate BlueZ `GetManagedObjects` and subscribe to
  ObjectManager, Properties, and BlueZ owner changes without starting a scan.
- Implement explicit one-adapter discovery with the default `auto` transport,
  owned start/stop lifecycle, bounded update staging, and no pairing or
  connection side effects during inventory or scanning.
- Add `dmn-bluez-discovery` with read-only snapshot mode and continuous
  `--discovery` mode. Select the sole powered adapter by default; require an
  explicit adapter selector if multiple powered adapters exist.
- Add executable private-bus tests for the actual public BlueZ API, CLI
  argument/output/lifecycle behavior, and no implicit Bluetooth operations.
  Retain the hardware smoke test as an opt-in qualification step.

**Exit gate:** follow all API, CLI, private-bus, build-matrix, and smoke-test
criteria in the linked discovery specification. This milestone makes no GATT,
ANCS, or complete-radio-visibility claim and is not blocked by iPhone
feasibility.

### Phase 1 — Explicit pairing and BlueZ GATT capability

This phase builds the minimum explicit pairing and remote-GATT surface needed
to test the iPhone hypothesis. Keep each gate independently testable; do not
start ANCS session/cache behavior before the BlueZ GATT contract passes its
private-bus tests.

**Phase 1A — Pairing API and CLI (implemented and private-bus validated)**

- Provide an explicit `Dmn_BluezClient::pairDevice` operation and
  `--pair DEVICE` CLI action. Resolve selectors against current inventory and
  current object paths; an already-paired target is an idempotent success.
- Call `Device1.Pair` only for that explicit request. Rely on BlueZ's default
  agent; do not register an application agent, set `Trusted`, or pair during
  enumeration, scanning, or connection.
- Confirm `Paired=true` before reporting a successful paired action. Surface
  BlueZ authentication cancellation/rejection and missing-agent failures as
  typed remote errors. Use a finite 120-second CLI action deadline.
- Keep `dmn-bluez-discovery` compatible. The eventual `dmn-bluetooth` command
  can unify inventory, discovery, pair/connect, GATT inspection, and ANCS
  without changing the library API.

**Phase 1B — Remote GATT backend (hardware-independent gate passed)**

- The generic D-Bus method/property/signal client, typed object values,
  callback worker, finite calls, and private-bus tests are already present.
  Preserve `Dmn_DBusInput`/`Dmn_DBusOutput` behavior and keep public headers
  free of libdbus types.
- `Dmn_GattClient` and `Dmn_BluezGattClient` now provide bounded synchronous
  service-resolution, discovery, notification setup/cleanup, and acknowledged
  writes. Notification events are asynchronous on the D-Bus callback worker;
  GTK and other UI callers must invoke blocking methods off their UI thread
  and marshal events onto their UI context.
- Before Step 1A, expose the backend-neutral GATT declarations through a
  small `dmn-gatt` interface target (or equivalent exported header-only
  target). `dmn-ancs` depends on that contract, not on `dmn-bluez`; the BlueZ
  implementation continues to depend on D-Bus.
- Private-bus tests cover selected-device object filtering, uppercase
  canonical UUIDs, service-resolution timeout/readiness, initial notification
  delivery, explicit StopNotify, StopNotify retry after remote failure,
  disconnect/object-removal/BlueZ-owner invalidation, authorization and write
  permission errors, acknowledged write bytes/options, and rejection of
  foreign or non-notifiable characteristics.
- The focused GATT private-bus suite, BlueZ inventory suite, D-Bus suite,
  CLI checks, standalone public-header compilation, and formatting/whitespace
  checks pass. The ANCS parser and fake-GATT session tests now cover source
  decoding, bounded Data Source reassembly, subscription ordering, request
  serialization, default pending-request backlog policy, callback execution context,
  startup rollback, invalidation, cleanup retry, write-error failure, and
  response timeout. The monitor CLI's private-bus tests cover selected-device
  Pair/Connect targeting, ambiguous selection, missing-ANCS failure,
  subscription, attribute exchange/output, and SIGINT cleanup. Output
  privacy/stderr separation, notification escaping, and broken-stdout cleanup
  are covered by separate subprocess cases, as is SIGINT during a blocked
  GATT-subscription startup step and output-queue saturation under a
  backpressured stdout pipe.
- No physical-phone operation is part of private-bus validation. The real
  device feasibility probe below remains a separate gate.

**Phase 1C — Real-device feasibility probe**

- Probe only after the fake GATT contract passes. Keep pairing, connection,
  service resolution, ANCS discovery, notification authorization, and one
  attribute-response exchange as separately recorded gates.
- Record Linux distribution/kernel, BlueZ version, adapter chipset/firmware,
  iPhone model, and iOS version. Do not record phone identifiers or
  notification contents in repository artifacts.
- Use explicit `--pair` when pairing is needed, with the system's default
  BlueZ agent available for the user interaction. Then explicitly connect,
  await service resolution, locate the ANCS service/characteristics by UUID,
  and attempt authorized subscriptions and one attribute-response exchange.
- Record each observed stage separately: Pair result, Connected,
  ServicesResolved, ANCS tree/flags, StartNotify authorization, Control Point
  write result, Data Source response, and what invalidation/disconnect does.
  The first monitor exits on disconnect and makes no reconnect claim. Do not
  infer one stage from another or claim compatibility after only
  `Connected=true`.
- Do not enable payload logging by default; collect only the minimum data
  required to establish protocol behavior, and redact notification content
  from any diagnostic record.

**Exit gates:** The hardware-independent gate requires Phase 1A/1B private-bus
tests, the existing BlueZ regression suite, and standalone headers to pass.
The real-device gate requires repeatable usable LE GATT access to ANCS,
including one attribute-response round trip and an understood authorization
flow, on a recorded target configuration. The latest read-only BlueZ inventory
found “iPhone Old” paired and connected but with `ServicesResolved=false`.
Production CLI attempts with 15-second and 60-second service-resolution
deadlines failed before GATT readiness. The latest attempt invoked the
separate `connectAvailableProfiles` request; BlueZ returned “Operation
already in progress,” after which the monitor continued waiting for service
resolution. Following a user-approved disconnect/reconnect, the production
CLI observed resolved services but found no ANCS service UUID in the GATT
tree. There were no ANCS subscriptions or attribute exchanges. These
observations do not establish usable ANCS access. Investigate why this
iPhone/BlueZ connection does not publish the service, and only request further
phone-side action when its purpose is clear. Until a real iPhone/iOS target
passes the GATT and ANCS gates, Phase 1 remains hardware-unqualified; protocol
work may continue against deterministic fake backends, but do not claim iPhone
compatibility.

### Step 1 — CLI: pair/connect, monitor ANCS, and print notifications

This is the first user-facing product step. The command pairs and connects
only the explicitly selected iPhone when needed, establishes an authorized
ANCS session, continuously receives Notification Source events, requests and
reassembles notification attributes, and prints available notification data
to the terminal until Ctrl+C/SIGTERM or a typed fatal error. Pairing/connection
and notification display are explicit because this workflow changes Bluetooth
state and exposes private notification content. It never claims success merely
because the device is paired, connected, or has resolved services.

Implement this step through three internal gates; they are implementation
order, not separate user-facing products:

#### Step 1A — Protocol parser and request primitives

**Implementation status:** Implemented and covered by
`dmn-test-ancs-parser`; malformed lengths, field decoding, fragmented response
reassembly, tuple validation, size limits, and invalid UTF-8 have executable
assertions. Keep scheduler-specific cases below as future coverage rather than
treating parser tests as proof of session behavior.

- Turn the ANCS acceptance matrix into executable Google Test assertions; do
  not count skipped placeholders as coverage.
- Treat the existing `Dmn_GattClient` ownership, typed errors, operation
  deadlines, and callback context as the frozen transport contract.
- Implement the exact ANCS values, errors, limits, and `Dmn_AncsClient`
  contract in `ancs-bluez-client-spec.md`. Add protocol-neutral owned values
  and byte-span parsers to `dmn-ancs`; do not add BlueZ types or pairing,
  connection, or CLI policy to this library.
- Parse Notification Source and reassemble Data Source responses across
  arbitrary fragment boundaries. Enforce per-value, attribute, response, and
  timeout limits; keep event intake dynamically sized, and apply the
  configurable pending-request backlog policy without a lifetime count cap.
  Optionally suppress notification details whose ANCS Date predates session
  startup. A frontend can supply its own earlier cutoff so connection setup
  time does not shift the intended application-start boundary.
- Add a fake `Dmn_GattClient` for deterministic tests of malformed input,
  unknown fields, stale/removed UIDs, request ordering, and bounded resources.

**Gate:** protocol-only tests pass without BlueZ, D-Bus, or physical hardware.

#### Step 1B — Reusable ANCS session library

**Implementation status:** `Dmn_AncsClient` is implemented over the borrowed
backend-neutral GATT interface. Its fake-GATT test covers startup subscription
order, configuration/handler validation, missing service, removal, serialized
requests, fatal Control Point write errors, and response timeout. Burst tests
verify the default rolling 255-request backlog policy and processing 600
notifications with the library backlog cap disabled. GATT event intake and
active notification records have no count-based cap. Callback thread affinity
and no delivery after a completed stop are covered; concurrent callback/stop
races remain open. Startup rollback, GATT invalidation cleanup, and retry
after subscription-cleanup failure have focused tests.
`dmn-ancs` links `dmn` for `Dmn_Proc` plus `dmn-gatt`; it does not link BlueZ
or D-Bus. The library does not use `Dmn_Runtime_Manager`.

- Implement the specified start/discover/subscribe/ready/stop/failure states;
  reconnect is out of scope.
- Subscribe Data Source first and Notification Source second; process Added,
  Modified, and Removed events; request only the specified notification
  attributes and serialize response-bearing Control Point requests.
- Reassemble responses, discard stale UIDs, emit typed state/error/
  notification callbacks on the documented Dmn execution context, and keep
  bounded session state in memory only. The library never logs content.
- Prove startup order, request serialization, removal races, response-timeout
  failure, invalidation, cleanup, and backpressure using fake GATT.

**Gate:** fake-backend end-to-end tests pass, including cleanup and fatal
response-timeout behavior.

#### Step 1C — `dmn-bluetooth monitor` application

**Implementation status:** The reusable `Dmn_AncsBluezMonitor` library
composes BlueZ, GATT, and ANCS; `dmn-bluetooth` is now a terminal adapter that
uses the facade and `Dmn_Runtime_Manager`/`Dmn_Pipe` for signal/job and output
handling. The ANCS worker remains independent of that runtime. The private-bus
suite now exercises both CLI behavior and direct facade startup/event/cleanup.
Output privacy/stderr separation, notification escaping, and broken-stdout
cleanup remain frontend-specific CLI tests.

- Compose `Dmn_BluezClient`, `Dmn_BluezGattClient`, and `Dmn_AncsClient` in
  the reusable facade. Resolve a device only as an exact, unique BlueZ
  display-name match at runtime; do not embed or persist its address or object
  path. Provide an explicit adapter selector when adapter choice is ambiguous.
- The monitor command is explicit consent to discover the selected target if
  needed, Pair if not already paired, Connect if not already connected, and
  display available notification content. If already connected but services
  are unresolved, call `connectAvailableProfiles` once and then continue
  waiting for service resolution, including when BlueZ reports an existing
  connection attempt. Operate on no unrelated device; refresh identity/state
  after Pair and verify each stage before proceeding.
- Continuously print Added/Modified notification records with available
  requested attributes and print Removed events without attributes. Use the
  exact stdout/stderr split, terminal-safe escaping, finite timeout defaults,
  exit statuses, signal cleanup, and content-privacy rules in the client spec.
  Leave the phone paired/connected on exit and do not reconnect automatically.
- Keep `dmn-bluez-discovery` unchanged. Build `dmn-ancs` behind `ENABLE_ANCS`
  without BlueZ/D-Bus linkage; build `dmn-ancs-bluez-monitor` and
  `dmn-bluetooth` only when both `ENABLE_ANCS` and `ENABLE_BLUEZ` are enabled.
- Test exact/ambiguous selectors, unrelated-device noninterference, discovery
  and Pair/Connect ordering, authorization/startup failures, ongoing events,
  output/privacy/escaping, SIGINT/SIGTERM cleanup, queue saturation, and
  compatibility with the existing discovery CLI.

**Verified gates:** parser, fake-GATT, focused private-bus CLI, existing
discovery CLI, standalone-header, and formatting checks pass. The opt-in
production-CLI hardware test is not run by default and requires a working
iPhone ANCS GATT session. These software checks do not prove real-iPhone
compatibility. The physical-device gate remains blocked while BlueZ reports
`ServicesResolved=false`.

#### Step 1D — “iPhone Old” hardware qualification

- Before changing device state, do a read-only preflight: record the deployed
  Linux/kernel, BlueZ, adapter, iPhone model, and iOS version, plus the
  current BlueZ row and `ServicesResolved` state. Do not Pair, Connect,
  Disconnect, set `Trusted`, or start discovery during this preflight.
- If the target is not currently known, obtain user approval before discovery.
  If GATT objects remain absent, stop and investigate bearer/BlueZ/iOS state;
  do not repeatedly connect or infer ANCS availability.
- Only after software gates pass and with explicit user approval, run the
  opt-in `dmn-ancs-iphone-hardware` CTest or monitor against the selected
  target. Record Pair, Connected,
  ServicesResolved, ANCS tree/flags, each subscription authorization, and one
  controlled notification attribute round trip as separate observed results.
  Do not record phone addresses or notification contents.
- Use a newly delivered notification from any app whose iOS notifications are
  enabled and allowed to appear in Notification Center. A Messages SMS or
  iMessage is a convenient test but is not an ANCS requirement. Check that
  Focus or Scheduled Summary is not delaying it; do not include its contents
  in test notes.

The hardware gate passes only when the selected iPhone exposes the current
ANCS service/characteristics, both subscriptions are authorized, and a Control
Point request produces a fully reassembled Data Source response. Apple marks
Notification Source mandatory but Control Point and Data Source optional; this
notification-details client requires all three. BlueZ `Device1.Connect`
requests generic auto-connectable profiles and does not promise an LE bearer.
`ServicesResolved=true` is not proof of ANCS. Pairing, link connection,
service resolution, characteristic visibility, subscription authorization,
and attribute response are separate results. If “iPhone Old” cannot pass the
gate, report it as unqualified rather than hiding the limitation with retries,
fallback profiles, or success-shaped output.

The physical CLI smoke test is registered only when
`ENABLE_ANCS_HARDWARE_TESTS=ON`, which itself requires `ENABLE_ANCS=ON`,
`ENABLE_BLUEZ=ON`, and `ENABLE_DBUS=ON`. It runs the production
`dmn-bluetooth` binary against the exact `DMN_ANCS_HARDWARE_DEVICE` selector
(default `iPhone Old`), waits for ANCS readiness, and sends SIGINT to verify
orderly shutdown. It is not part of Docker/default test runs. Enable it with:

```bash
cmake -B build -DENABLE_DBUS=ON -DENABLE_BLUEZ=ON -DENABLE_ANCS=ON \
  -DENABLE_ANCS_HARDWARE_TESTS=ON \
  -DDMN_ANCS_HARDWARE_DEVICE="iPhone Old"
cmake --build build --target dmn-bluetooth dmn-test-bluez-device-list
ctest --test-dir build -R dmn-ancs-iphone-hardware --output-on-failure
```

### Step 2 — Future GTK+ frontend

- Build the GUI directly on the same public BlueZ/GATT/ANCS APIs. Do not invoke
  `dmn-bluetooth` as a subprocess or parse CSV.
- Provide an explicit device-selection/connection/session view that displays
  stage state and normalized ANCS notification events. Pairing/connection and
  notification-content display remain explicit user actions; GTK must not
  auto-connect or silently expose notification content on launch.
- Marshal worker callbacks to the GTK main context before touching widgets.
  Run synchronous system-D-Bus calls on a worker so the UI remains responsive
  while pairing prompts, service resolution, or timeouts are in progress.
- Keep notification display opt-in and ephemeral by default. Do not add a
  persistent notification database or log message bodies unless a separate
  user-visible privacy requirement is approved.

### Later GATT/ANCS acceptance matrix

Use these names as stable case names in one or more test executables. Each
case must assert observable results through the public protocol/session
surface; avoid tests that merely exercise private helpers.

| Test name | Purpose | Hard gate |
|---|---|---|
| `DmnAncsParserRejectsMalformedNotificationSource` | Reject payloads not exactly eight bytes and reserved event IDs | Wrong length is fatal typed error; reserved IDs are nonfatal unsupported input |
| `DmnAncsParserAcceptsKnownNotificationSourceValues` | Validate event ID, flags, category, count, and little-endian UID | Exact decoded fields, including unknown category/flag preservation |
| `DmnAncsParserReassemblesFragmentedDataSourceResponse` | Split every response header/value field across fragments | Correct ordered values; completion only after requested tuples are complete |
| `DmnAncsParserDropsStaleRemovedUid` | Deliver a response after the UID was removed | No stale notification is recreated or emitted as active |
| `DmnAncsRequestSchedulerSerializesAttributeRequests` | Queue multiple attribute requests | At most one active response-bearing command; queued work progresses FIFO |
| `DmnAncsRequestSchedulerFailsOnStaleRequestTimeout` | Data Source response misses its deadline | Typed fatal timeout; do not write another request because a late response cannot be correlated safely |
| `DmnBluezGattClientMapsBluezErrorsToTypedFailures` | Inject service, authorization, property, and write failures | Typed error and recoverable/fatal classification; no raw D-Bus exception escapes |
| `DmnAncsSessionFailsAndCleansUpAfterInvalidation` | Disconnect during notification and request handling | Old paths/UIDs/requests invalidated; subscriptions cleaned up; no reconnect attempted |
| `DmnAncsClientHandlesUnknownAttributeId` | Include an unknown bounded attribute tuple | Unknown tuple is safely skipped; known requested tuples remain correct |
| `DmnAncsClientSubscribesDataSourceBeforeNotificationSource` | Validate session startup order and immediate initial event | Both subscriptions succeed in order; Ready is dispatched before queued notification events |
| `DmnAncsClientFailsOnAttributeResponseTimeout` | Data Source response misses its deadline | `AttributesUnavailable` and fatal `Timeout` are delivered; no later Control Point request is sent |
| `DmnAncsClientFailsSessionOnControlPointWriteError` | BlueZ/GATT write fails without a verified ANCS-specific classification | Fatal typed error; stop the session and do not advance the request queue |
| `DmnAncsClientDefaultBacklogKeepsNewestNotificationsAndReportsDiscard` | Exceed the default pending-request backlog | Oldest pending request is discarded, newer requests are retained, and a nonfatal resource-limit event is emitted |
| `DmnAncsClientProcessesMoreThanFiveHundredQueuedNotifications` | Process a 600-notification burst with unlimited mode | Client remains ready and writes requests for all notifications |
| `DmnAncsClientCanSuppressNotificationsPredatingSessionStart` | Enable the startup date cutoff | Old and unparseable dates are suppressed; a newer notification is delivered |
| `DmnAncsClientSerializesCallbacksAndStopsCleanly` | Race callback delivery with stop | Serialized callbacks cease after stop; both tokens are cleaned up |
| `DmnBluetoothMonitorSelectsExactDeviceName` | Exercise exact and ambiguous target selectors | No fuzzy/first-match selection; candidates are reported |
| `DmnBluetoothMonitorPairsAndConnectsOnlySelectedDevice` | Pair/connect target with synthetic unrelated devices | Only selected device is changed; state is refreshed and verified |
| `DmnBluetoothMonitorStopsOnSignalAndCleansUp` | Send SIGINT during active monitor | Both subscriptions stop; status 0 and pairing/link remain |
| `DmnBluetoothMonitorDoesNotLogContentToStderr` | Deliver fixture notification content | Fields appear only on stdout; stderr contains no content |
| `DmnBluetoothMonitorCli.BoundsOutputQueueWhenStdoutIsBackpressured` | Fill stdout and deliver sustained notification events | Explicit 256-event failure and subscription cleanup; no silent success |
| `DmnBluetoothMonitorRejectsInvalidTimeouts` | Supply invalid timeout values | Exit status 2 before any D-Bus access |
| `DmnBluetoothMonitorEscapesDiagnostics` | Inject control characters in adapter/device names and remote errors | Terminal-safe stderr with no notification contents |
| `DmnBluetoothMonitorPreservesDiscoveryCliCompatibility` | Run old CLI tests with monitor target enabled | Existing `dmn-bluez-discovery` behavior is unchanged |

Also cover absent ANCS characteristics, empty requested values, unexpected
tuple order/UID, malformed UTF-8, Control Point errors, stop during startup,
and callback ordering. These cases are part of the phase gate even when
grouped under the named tests.

### Later extensions — Desktop notification and service integration

Only after the CLI and its reusable library pass their software gates; GTK+
itself is the separate Step 2 above:

- Add an optional notification sink (for example libnotify) outside the ANCS
  protocol library.
- Decide whether the process should become a user service; prefer a user
  systemd unit over a privileged system daemon unless a concrete system-wide
  requirement demands otherwise.
- If multiple applications need the same Bluetooth session, design a
  separate local IPC protocol and service ownership model. Do not let multiple
  clients independently race to own the same ANCS connection.
- Consider action commands only with explicit user-consent/security behavior
  and dedicated tests.

## 3. Proposed target and source layout

Names are proposals to keep implementation discussion concrete:

| Path / target | Contents |
|---|---|
| `dmn-dbus` target: `include/dmn-dbus-client.hpp`, `include/dmn-dbus-value.hpp`, private client/runtime implementation under `src/` | Generic owned D-Bus values, method/reply, property and signal client API, and the shared once-only libdbus runtime; existing signal endpoints retain their current behavior |
| Backend-neutral GATT contract: `include/dmn-gatt-client.hpp` | Implemented in Phase 1B; expose it through a small platform-neutral `dmn-gatt` interface target so ANCS can be built without BlueZ or D-Bus |
| Implemented `dmn-ancs` target: `include/dmn-ancs-client.hpp`, `src/dmn-ancs-client.cpp` | ANCS states, notification values, request scheduling, parser, and callback API; links `dmn` for `Dmn_Proc` plus backend-neutral `dmn-gatt`, never BlueZ or D-Bus |
| Implemented optional Dmn `dmn-bluez` target: `include/dmn-bluez-client.hpp`, `include/dmn-bluez-gatt-client.hpp`, `include/dmn-gatt-client.hpp`; private implementation under `src/` | Linux adapter/device/discovery and remote-GATT APIs on `Dmn_DbusClient` from `dmn-dbus`; public headers do not expose libdbus types |
| `dmn-bluez-discovery` executable | First user-facing milestone: read-only inventory and explicit live `--discovery` scan, no GATT/ANCS dependency |
| `test/dmn-test-dbus-client.cpp` | Private-bus tests for generic D-Bus method, property, signal, value-codec, timeout, and lifecycle behavior |
| `test/dmn-test-ancs-parser.cpp` | Pure parser/reassembly tests |
| `test/dmn-test-ancs-client.cpp` | Fake-GATT ANCS lifecycle, request/queue limits, callback context, cleanup, and timeout tests |
| `test/dmn-test-bluez-gatt-client.cpp` | Private-bus fake-BlueZ integration tests |
| `test/dmn-test-bluez-device-list.cpp` | Private-bus Linux integration tests for known/connected-device enumeration, discovery, and live updates |
| `src/dmn-bluetooth.cpp` | Implemented `dmn-bluetooth monitor DEVICE` application adapter |
| `test/CMakeLists.txt` | CLI help/argument tests and filtered `dmn-bluetooth-monitor-private-bus` CTest backed by the existing fake-BlueZ fixture |

Build the protocol library behind `ENABLE_ANCS` (off by default); it has no
Bluetooth or D-Bus dependency and can be tested on any supported core platform.
`ENABLE_DBUS` (off by default) builds `dmn-dbus`, containing both the existing
byte-signal API and the new generic D-Bus client/value capability, and owns the
optional `dbus-1` dependency. Build the Dmn-owned adapter/device/GATT client
behind `ENABLE_BLUEZ` (off by default, Linux-only); it requires
`ENABLE_DBUS=ON` and builds `dmn-bluez` on top of `dmn-dbus`. `ENABLE_BLUEZ`
must not require `ENABLE_ANCS`, so `dmn-bluez-discovery` and device inventory
can be built and used independently. `dmn-bluez-discovery` is built whenever `ENABLE_BLUEZ` is enabled. Build the
ANCS client when `ENABLE_ANCS` is enabled; build `dmn-bluetooth` only when both
`ENABLE_ANCS` and `ENABLE_BLUEZ` are enabled.
Do not make BlueZ, libdbus, libnotify, or a real adapter a dependency of core
`dmn` when these optional features are disabled.

## 4. Test strategy

1. Parser unit tests use byte fixtures derived from the Apple ANCS
   specification; no copied notification content or third-party app data.
2. Scheduler tests use a fake backend that controls notification timing,
   fragments, write completions, disconnects, and timeouts.
3. Linux device inventory/discovery integration tests use a private D-Bus
   daemon with fake ObjectManager, Adapter1, and Device1 objects across
   multiple adapters. Assert complete known-device enumeration, connected
   view filtering, discovery-result updates, name fallbacks, explicit
   filter/start/stop behavior, release of only this client's discovery
   session, and no implicit Pair/Connect/Disconnect during enumeration or
   scanning.
4. BlueZ GATT backend tests use a private D-Bus daemon with fake Adapter1, Device1,
   GattService1, and GattCharacteristic1 objects, exercising the public
   `dmn-bluez` API over Dmn's generic direct-libdbus client. Test service
   absence and authorization failures explicitly; tests must not connect to
   the host system bus.
5. Hardware tests are opt-in qualification tests; never make CI depend on a
   paired iPhone or Bluetooth adapter. Include a Linux smoke test confirming
   actual adapter/device selection, pairing/connection as needed, ANCS
   characteristic authorization, notification delivery, and orderly monitor
   shutdown. Do not disconnect/unpair the target on monitor exit.
6. Add tests at each phase gate before wiring the next layer.

### Required optional-feature build matrix

| Configuration | Required result |
|---|---|
| Default options (`ENABLE_ANCS=OFF`, `ENABLE_BLUEZ=OFF`) | Core library/tests build unchanged; no Bluetooth/ANCS dependencies or monitor executable |
| `ENABLE_ANCS=ON`, `ENABLE_BLUEZ=OFF` | Build `dmn-ancs`, parser/session tests, and public headers without D-Bus or BlueZ |
| `ENABLE_BLUEZ=ON`, `ENABLE_DBUS=ON`, `ENABLE_ANCS=OFF` | Build existing `dmn-bluez` and `dmn-bluez-discovery`; no ANCS dependency |
| `ENABLE_ANCS=ON`, `ENABLE_BLUEZ=ON`, `ENABLE_DBUS=ON` on Linux | Build `dmn-ancs`, `dmn-bluez`, `dmn-bluetooth`, all hardware-independent ANCS tests, and private-bus monitor tests |
| Above plus `ENABLE_ANCS_HARDWARE_TESTS=ON` | Additionally register the physical-device production-CLI test with the `hardware` label; never enabled by default |

No matrix entry may require an adapter or paired phone. The ANCS-only
configuration must remain portable to supported non-Linux builds; the
BlueZ/monitor configurations are Linux-only.

## 5. Research-driven decisions

- **Dmn-owned generic D-Bus capability:** BlueZ's documented D-Bus API exposes
  adapter/device discovery and connection, remote GATT objects,
  subscriptions, writes, and value-change signals. `dmn-dbus` provides the
  generic typed method/property/signal client directly over libdbus; the
  `dmn-bluez` layer uses that capability without a third-party Bluetooth or
  D-Bus wrapper. Keep this API distinct from the byte-signal endpoints and
  preserve the backend-neutral GATT contract.
- **One active attribute request:** ANCS response notifications do not carry a
  request correlation ID. Serializing the request queue is the simplest safe
  policy.
- **No daemon in V1:** first validate real-device behavior and app ergonomics
  in one process. Add a shared service only if multiple local clients need to
  share one BLE connection.
- **No universal compatibility promise:** Apple's archived ANCS material says
  the service is not guaranteed to always be present and does not publish a
  current general iOS compatibility matrix. Support is established only for
  tested configurations.

## 6. References

See the [ANCS and BlueZ client specification](ancs-bluez-client-spec.md) for
primary Apple and BlueZ sources and the constraints derived from them.
