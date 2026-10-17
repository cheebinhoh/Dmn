# Implementation Plan: Layered ANCS Client for Linux

**Status:** Phase 0 inventory/discovery and Phase 1A pairing plus Phase 1B
backend-neutral/BlueZ GATT implementation and private-bus gates are complete.
Phase 1C real-device qualification remains open, and no ANCS/iPhone
compatibility is claimed. The detailed ANCS library/monitor contract is now
specified, but its implementation has not started. The next implementation
work is the spec-frozen ANCS protocol/session and `dmn-bluetooth monitor`
command defined in
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
dmn-bluetooth (primary CLI)
  -> Dmn_BluezClient (adapter/device inventory and explicit discovery)
  -> optional selected iPhone:
       Dmn_AncsClient (ANCS lifecycle, commands, parser, bounded cache)
       -> Dmn_GattClient (backend-neutral bounded-operation contract)
       -> Dmn_BluezGattClient (BlueZ system D-Bus backend)
       -> BlueZ -> BLE -> iPhone

future GTK application
  -> the same Dmn_BluezClient / Dmn_GattClient / Dmn_AncsClient APIs
```

`dmn-bluetooth` must reuse the existing `dmn-bluez` library/client surface and
preserve `dmn-bluez-discovery` for compatibility. The explicit
`dmn-bluetooth monitor DEVICE` command may discover the target, pair it if
needed using the system default agent, connect it if needed, and start ANCS;
it never pairs or connects unrelated scan results. The GTK frontend must call
the same typed APIs directly;
it must not spawn the CLI, parse its CSV, or depend on CLI output as an
internal protocol. Neither frontend promises every nearby radio device. Do not
start by building a system daemon, notification database, libnotify
dependency, or IPC protocol.

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
- Private-bus tests cover selected-device object filtering, uppercase
  canonical UUIDs, service-resolution timeout/readiness, initial notification
  delivery, explicit StopNotify, StopNotify retry after remote failure,
  disconnect/object-removal/BlueZ-owner invalidation, authorization and write
  permission errors, acknowledged write bytes/options, and rejection of
  foreign or non-notifiable characteristics.
- The focused GATT private-bus suite, BlueZ inventory suite, D-Bus suite,
  parser-contract executable, CLI checks, standalone public-header compilation,
  and formatting/whitespace checks pass. The parser-contract executable still
  contains skipped ANCS placeholders; those are not ANCS protocol coverage.
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
flow, on a recorded target configuration. The last recorded read-only check of
the “iPhone Old” target found it paired and connected with
`ServicesResolved=true`, but its BlueZ object tree exposed no remote
`GattService1` or `GattCharacteristic1` objects at that time. This does not
establish a usable LE GATT session; no pairing, connection, or other device
state was changed during that check. Until a real iPhone/iOS target passes the
GATT and ANCS gates, Phase 1 remains hardware-unqualified; protocol work may
continue against deterministic fake backends, but do not claim iPhone
compatibility. If the target setup fails, keep the inventory/pair/connect CLI
useful and investigate the platform limitation before ANCS production
integration.

### Phase 2 — Implement the spec-frozen ANCS parser and tests

- Turn the ANCS acceptance matrix below into executable Google Test
  assertions; do not count skipped placeholders as coverage.
- Treat the new GATT API, ownership, typed errors, operation deadlines, and
  callback context as the frozen backend contract; close its Phase 1B gates
  before building the ANCS session on it.
- Implement the exact ANCS public values, errors, limits, and `Dmn_AncsClient`
  contract frozen in `ancs-bluez-client-spec.md`; do not add BlueZ types or
  connection/pairing behavior to this library.
- Add protocol-neutral ANCS value types and a parser that consumes byte spans.
- Implement Notification Source parsing and Data Source response reassembly.
- Enforce the specified attribute, fragment, response, queue, record, and
  timeout limits; overflow is a typed failure, never silent data loss.
- Add a fake `Dmn_GattClient` for deterministic unit tests.

**Exit gate:** executable hardware-independent tests pass, including malformed
input, arbitrary fragmentation boundaries, UID removal races, request
serialization, and bounded-resource tests. This gate establishes protocol
logic correctness only; it does not establish iPhone/BlueZ compatibility.

### Phase 3 — ANCS session and command scheduler

- Implement the specified start/discover/subscribe/ready/stop/failure states;
  reconnect is intentionally out of scope for this increment.
- Subscribe Data Source first and Notification Source second.
- Parse Added/Modified/Removed events; retrieve attributes for added/modified
  only.
- Serialize Get Notification Attributes requests for app ID, title, subtitle,
  message, and date with the specified finite FIFO, byte maxima, and deadline.
  Get App Attributes, display-name lookup, and action commands are deferred.
- Reassemble Data Source payloads, populate optional normalized fields, remove
  stale responses, and emit state/error/notification callbacks on the
  documented libDmn execution context.
- Keep all cache state in memory; never log notification content in the
  protocol library.

**Exit gate:** fake backend end-to-end tests prove event handling, requesting,
reassembly, cache updates/removals, disconnect races, fatal response-timeout
handling, cleanup, and bounded backpressure.

### Phase 4 — Primary CLI and compatibility qualification

- Add `dmn-bluetooth monitor DEVICE` with exact matching, bounded target
  discovery, optional adapter selection, explicit Pair/Connect for that
  target, ANCS startup, live stdout notification records, and signal cleanup
  exactly as specified. A monitor invocation is explicit consent to Pair,
  Connect, and display available notification content. Keep
  `dmn-bluez-discovery` unchanged and available for compatibility.
- Use the exact documented exit statuses, stdout/stderr split, terminal-safe
  escaping, finite timeout defaults, no-reconnect policy, and no-disconnect/
  unpair-on-exit behavior.
- Build `dmn-ancs` behind `ENABLE_ANCS`; it remains independent of BlueZ and
  D-Bus. Build `dmn-bluetooth` only when both `ENABLE_ANCS` and `ENABLE_BLUEZ`
  are enabled. Keep the existing discovery CLI available with `ENABLE_BLUEZ`
  alone.
- Add private-bus CLI tests for device resolution, discovery, Pair/Connect
  targeting, startup failures, signal shutdown, output escaping/privacy, and
  compatibility. Add separate hardware-independent fake-GATT session tests.
- Add real-device tests for each claimed iPhone/iOS/BlueZ/adapter combination
  and document the tested matrix.

**Exit gate:** all parser, fake-GATT, private-bus CLI, existing discovery CLI,
build-matrix, and standalone-header tests pass. A separate hardware gate
records an authorized ANCS attribute round trip on each claimed supported
matrix entry. No reconnect claim is made in this first monitor increment.

### Future phase — GTK frontend

- Build the GUI directly on the same public BlueZ/GATT/ANCS APIs. Do not invoke
  `dmn-bluetooth` as a subprocess or parse CSV.
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
| `DmnAncsClientHandlesRequestScopedAncsError` | Control Point reports an ANCS command error | Only that UID is marked unavailable; queued work for other UIDs progresses |
| `DmnAncsClientEnforcesQueueAndCacheLimits` | Exceed finite event/request/record capacity | Typed resource failure; no silent loss or unbounded growth |
| `DmnAncsClientSerializesCallbacksAndStopsCleanly` | Race callback delivery with stop | Serialized callbacks cease after stop; both tokens are cleaned up |
| `DmnBluetoothMonitorSelectsExactDeviceName` | Exercise exact and ambiguous target selectors | No fuzzy/first-match selection; candidates are reported |
| `DmnBluetoothMonitorPairsAndConnectsOnlySelectedDevice` | Pair/connect target with synthetic unrelated devices | Only selected device is changed; state is refreshed and verified |
| `DmnBluetoothMonitorStopsOnSignalAndCleansUp` | Send SIGINT during active monitor | Both subscriptions stop; status 0 and pairing/link remain |
| `DmnBluetoothMonitorDoesNotLogContentToStderr` | Deliver fixture notification content | Fields appear only on stdout; stderr contains no content |
| `DmnBluetoothMonitorBoundsOutputQueue` | Slow/full output consumer | Explicit failure and cleanup; no silent event loss |
| `DmnBluetoothMonitorRejectsInvalidTimeouts` | Supply invalid timeout values | Exit status 2 before any D-Bus access |
| `DmnBluetoothMonitorEscapesDiagnostics` | Inject control characters in adapter/device names and remote errors | Terminal-safe stderr with no notification contents |
| `DmnBluetoothMonitorPreservesDiscoveryCliCompatibility` | Run old CLI tests with monitor target enabled | Existing `dmn-bluez-discovery` behavior is unchanged |

Also cover absent ANCS characteristics, empty requested values, unexpected
tuple order/UID, malformed UTF-8, Control Point errors, stop during startup,
and callback ordering. These cases are part of the phase gate even when
grouped under the named tests.

### Future phase — Desktop and service integration

Only after Phase 4:

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
| Backend-neutral GATT contract: `include/dmn-gatt-client.hpp` | Implemented in Phase 1B; currently packaged with the optional `dmn-bluez` target. Before building ANCS with BlueZ disabled, expose this contract through a platform-neutral target/header installation path |
| Future `dmn-ancs` target: `include/dmn-ancs-client.hpp`, `src/dmn-ancs-client.cpp` | ANCS states, notification values, request scheduling, parser, and callback API; depend on the backend-neutral GATT contract without linking BlueZ or D-Bus |
| Implemented optional Dmn `dmn-bluez` target: `include/dmn-bluez-client.hpp`, `include/dmn-bluez-gatt-client.hpp`, `include/dmn-gatt-client.hpp`; private implementation under `src/` | Linux adapter/device/discovery and remote-GATT APIs on `Dmn_DbusClient` from `dmn-dbus`; public headers do not expose libdbus types |
| `dmn-bluez-discovery` executable | First user-facing milestone: read-only inventory and explicit live `--discovery` scan, no GATT/ANCS dependency |
| `test/dmn-test-dbus-client.cpp` | Private-bus tests for generic D-Bus method, property, signal, value-codec, timeout, and lifecycle behavior |
| `test/dmn-test-ancs-parser.cpp` | Pure parser/reassembly/request-scheduler tests |
| `test/dmn-test-bluez-gatt-client.cpp` | Private-bus fake-BlueZ integration tests |
| `test/dmn-test-bluez-device-list.cpp` | Private-bus Linux integration tests for known/connected-device enumeration, discovery, and live updates |
| `src/dmn-bluetooth.cpp` | Primary `dmn-bluetooth monitor DEVICE` application adapter |
| `test/dmn-test-ancs-session.cpp` | Fake-GATT ANCS lifecycle, scheduler, callback, and cleanup tests |
| `test/dmn-test-bluetooth-monitor.cpp` | Private-bus monitor CLI acceptance tests |

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
