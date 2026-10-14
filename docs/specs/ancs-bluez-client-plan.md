# Implementation Plan: Layered ANCS Client for Linux

**Status:** Proposed follow-on plan. The first, hardware-independent
Bluetooth deliverable is the BlueZ inventory/discovery CLI described in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). This plan covers
later GATT/ANCS work and does not make iPhone feasibility a prerequisite for
shipping that first milestone.

## 1. Recommended minimal solution

After the discovery CLI milestone, build a small, directly linked C++
application on top of libDmn:

```text
dmn-ancs-monitor (example application)
  -> Dmn_BluezClient (adapter/device inventory and explicit discovery)
  -> optional selected iPhone:
       Dmn_AncsClient (ANCS lifecycle, commands, parser, bounded cache)
       -> Dmn_GattClient (backend-neutral async contract)
       -> Dmn_BluezGattClient (BlueZ system D-Bus backend)
       -> BlueZ -> BLE -> iPhone
```

The later `dmn-ancs-monitor` may reuse the inventory and discovery API to show
connected devices and nearby BlueZ-reported devices, then let the user select
one eligible iPhone for an ANCS session. The separate first CLI milestone
already provides inventory and explicit scan without any ANCS dependencies.
Neither view promises every nearby radio device. Do not start by building a
system daemon, notification database, libnotify dependency, or IPC protocol.
Those features do not reduce the primary risk: whether the target iPhone
exposes usable ANCS to the target BlueZ environment.

Implement the BlueZ backend as part of the Dmn codebase and call the public
`libdbus` C API directly to use BlueZ's documented system-D-Bus API. Do not
add a third-party Bluetooth/GATT wrapper, D-Bus wrapper such as GDBus, or
another BLE stack. `libdbus` is the direct transport dependency, not a
Bluetooth abstraction. Keep this optional Linux feature separate from the
existing Dmn D-Bus signal transport: `Dmn_DBusInput`/`Dmn_DBusOutput` do not
implement BlueZ method calls, object/property access, or GATT notifications.
The backend-neutral GATT interface is the sole intended extension point for a
future backend; do not add a plugin framework.

## 2. Phases and exit gates

### Phase 0 — BlueZ inventory and `--discovery` CLI (first deliverable)

Implement the separate contract in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). This is the
first independently useful Bluetooth feature and does not require a phone,
GATT, ANCS, or a physical adapter for normal CI.

**Implementation status:** Initial `dmn-bluez` API and
`dmn-bluez-discovery` CLI are available. Private-bus tests and a physical
adapter smoke scan have passed; complete all remaining acceptance gates in the
linked spec before calling Phase 0 fully qualified.

- Implement adapter/device inventory and live changes over the generic
  `Dmn_DbusClient`; enumerate BlueZ `GetManagedObjects` and subscribe to
  ObjectManager, Properties, and BlueZ owner changes without starting a scan.
- Implement explicit one-adapter discovery with the default `auto` transport,
  owned start/stop lifecycle, bounded update staging, and no pairing or
  connection side effects.
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

### Phase 1 — GATT/ANCS real-device feasibility spike (later gate)

Perform this feasibility work only after the inventory/discovery CLI works:

1. Record Linux distribution/kernel, BlueZ version, adapter chipset/firmware,
   iPhone model, and iOS version.
2. Use the normal BlueZ/user pairing flow and direct BlueZ D-Bus calls through
   libDmn to establish an LE GATT connection and check for ANCS.
3. Verify service resolution, characteristic availability, authorization,
   subscriptions, one attribute-response round trip, and behavior across
   disconnect and BlueZ restart.

**Exit gate:** the target setup demonstrates repeatable usable LE GATT access
to ANCS, including at least one attribute-response round trip and an understood
authorization flow. A generic `Connected` state or successful scan does not
pass this gate. If it fails, the inventory/discovery CLI remains a valid
standalone deliverable; stop only GATT/ANCS work and investigate platform
constraints.

### Phase 2 — Freeze GATT/ANCS protocol model and implement tests first

- Turn the ANCS acceptance matrix below into executable Google Test
  assertions; do not count skipped placeholders as coverage.
- Freeze public GATT/ANCS signatures, ownership, lifecycle, typed errors,
  callback execution context, and shutdown behavior from the spec.
- Add protocol-neutral ANCS value types and a parser that consumes byte spans.
- Implement Notification Source parsing and Data Source response reassembly.
- Define limits for attribute values, individual fragments, total response,
  pending request count, active notification records, and timeouts.
- Define typed state/error/event values and callback ordering/context.
- Add a fake `Dmn_GattClient` for deterministic unit tests.

**Exit gate:** executable hardware-independent tests pass, including malformed
input, arbitrary fragmentation boundaries, UID removal races, request
serialization, and bounded-resource tests. This gate establishes protocol
logic correctness only; it does not establish iPhone/BlueZ compatibility.

### Phase 3 — Generic D-Bus readiness and BlueZ GATT backend

**Phase 3A — generic D-Bus capability**

The initial `Dmn_DbusClient`/`Dmn_DbusValue` implementation and private-bus
test fixture are present in `dmn-dbus`; this is initial implementation, not
complete acceptance. Close only the generic-client gaps required by the
BlueZ/GATT operations, using the readiness gaps and acceptance gates in
[`dbus-client-spec.md`](dbus-client-spec.md). The initial discovery CLI is
specified separately and does not wait for GATT or ANCS readiness.

- Preserve existing `Dmn_DbusInput`/`Dmn_DbusOutput` contracts and tests.
  Their signal tuple, `ay` payload, queues, and delivery semantics remain
  independent from the generic method/property/signal API.
- Test method calls/replies, properties, signal subscriptions,
  ObjectManager-shaped values, timeout/error/cancellation, malformed values,
  queue limits, callback context, and shutdown using a private D-Bus daemon
  with a fake service. Do not use the host system bus.

**Phase 3A exit gate:** the generic-client acceptance cases needed by GATT and
all existing `dmn-dbus` regression tests pass; public headers expose no
libdbus types; core `dmn` still has no libdbus dependency when
`ENABLE_DBUS=OFF`.

**Phase 3B — Extend `dmn-bluez` with GATT**

- Extend the `dmn-bluez` target delivered by the discovery milestone; do not
  duplicate the inventory, ObjectManager, discovery-session, or CLI
  implementation.
- Keep all BlueZ calls on `Dmn_DbusClient`; do not route them through or change
  `Dmn_DbusInput`/`Dmn_DbusOutput`, and do not expose libdbus types publicly.
- Add explicit user-selected-device connection, service-resolution waiting,
  GATT UUID lookup, characteristic `StartNotify`/`StopNotify`, and
  `WriteValue`. Connection must never be an implicit consequence of
  inventory or scan.
- Ensure signal/property callbacks are installed before enabling notifications.
- Map generic D-Bus transport/method failures and BlueZ remote errors to
  explicit library errors; invalidate characteristic paths on disconnect or
  service change. Follow Dmn's established bounded-wait, signal-dispatch, and
  shutdown patterns.
- Add fake-object integration tests that exercise the actual Dmn BlueZ API and
  generic D-Bus client over a private bus; do not require a physical adapter
  in normal CI and do not use the host system bus in tests.

**Phase 3B exit gate:** fake BlueZ GATT objects pass backend contract tests;
the existing discovery API/CLI tests still pass; standalone public headers
compile with the optional targets enabled. `dmn-bluez` depends on `dmn-dbus`
and no third-party Bluetooth/GATT wrapper.

### Phase 4 — ANCS session and command scheduler

- Implement explicit connect/discover/subscribe/ready/reconnect/stop states.
- Subscribe Data Source first and Notification Source second.
- Parse Added/Modified/Removed events; retrieve attributes for added/modified
  only.
- Serialize Get Notification Attributes/Get App Attributes requests with a
  finite FIFO and timeout. Do not send actions in V1.
- Reassemble Data Source payloads, populate optional normalized fields, remove
  stale responses, and emit state/error/notification callbacks on the
  documented libDmn execution context.
- Keep all cache state in memory and avoid content logging by default.

**Exit gate:** fake backend end-to-end tests prove event handling, requesting,
reassembly, cache updates/removals, disconnect races, timeout recovery, and
bounded backpressure.

### Phase 5 — ANCS example application and compatibility qualification

- Add `dmn-ancs-monitor` as an opt-in Linux example. It displays currently
  connected BlueZ-managed devices and provides an explicit discovery control
  for nearby results, tracking adapter/device changes. The user may provide
  `--device <address>` (or select a listed device) to connect and start ANCS
  for exactly one iPhone; scanning/listing does not automatically pair or
  connect devices.
- Require explicit user selection; report pairing, missing service,
authorization, connection, and unsupported-characteristic states distinctly.
- Add real-device tests for each claimed iPhone/iOS/BlueZ/adapter combination
and document the tested matrix.
- Keep stdout content display opt-in and never print notification bodies in
diagnostic logs by default.

**Exit gate:** the example runs through connect, notification add/update/
remove, and reconnect on the supported matrix without unbounded resource
growth or leaked subscriptions.

### Later GATT/ANCS acceptance matrix

Use these names as stable case names in one or more test executables. Each
case must assert observable results through the public protocol/session
surface; avoid tests that merely exercise private helpers.

| Test name | Purpose | Hard gate |
|---|---|---|
| `DmnAncsParserRejectsMalformedNotificationSource` | Reject payloads not exactly eight bytes and reserved event IDs | No crash or unbounded allocation; malformed event has defined typed outcome |
| `DmnAncsParserAcceptsKnownNotificationSourceValues` | Validate event ID, flags, category, count, and little-endian UID | Exact decoded fields, including unknown category/flag preservation |
| `DmnAncsParserReassemblesFragmentedDataSourceResponse` | Split every response header/value field across fragments | Correct ordered values; completion only after requested tuples are complete |
| `DmnAncsParserDropsStaleRemovedUid` | Deliver a response after the UID was removed | No stale notification is recreated or emitted as active |
| `DmnAncsRequestSchedulerSerializesAttributeRequests` | Queue multiple attribute requests | At most one active response-bearing command; queued work progresses FIFO |
| `DmnAncsRequestSchedulerTimesOutStaleRequest` | Omit Data Source completion | Active request expires and the next valid request progresses |
| `DmnBluezGattClientMapsBluezErrorsToTypedFailures` | Inject service, authorization, property, and write failures | Typed error and recoverable/fatal classification; no raw D-Bus exception escapes |
| `DmnAncsSessionReconnectsAfterDisconnect` | Disconnect during notification and request handling | Old paths/UIDs/requests invalidated; rediscovery does not reuse stale state |
| `DmnAncsClientHandlesUnknownAttributeId` | Include an unknown bounded attribute tuple | Unknown tuple is safely skipped; known requested tuples remain correct |
Add focused cases for absent ANCS characteristics, empty requested values,
unexpected tuple order/UID, malformed UTF-8 policy, hard resource limits,
Control Point ATT errors, stop during connect, and callback ordering. These
cases are part of the acceptance gate even when grouped into the named tests
above.

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
| `dmn-ancs` target: `include/dmn-gatt-client.hpp`, `include/dmn-ancs-client.hpp`, `src/dmn-ancs-client.cpp` | Backend-neutral GATT contract plus ANCS states, notification values, request scheduling, parser, and callback API; no BlueZ or D-Bus dependency |
| Optional Dmn `dmn-bluez` target: `include/dmn-bluez-client.hpp`, `include/dmn-bluez-gatt-client.hpp`; private implementation under `src/` | Linux adapter/device/discovery API first; later extended with GATT on `Dmn_DbusClient` from `dmn-dbus`; public headers do not expose libdbus types |
| `dmn-bluez-discovery` executable | First user-facing milestone: read-only inventory and explicit live `--discovery` scan, no GATT/ANCS dependency |
| `test/dmn-test-dbus-client.cpp` | Private-bus tests for generic D-Bus method, property, signal, value-codec, timeout, and lifecycle behavior |
| `test/dmn-test-ancs-parser.cpp` | Pure parser/reassembly/request-scheduler tests |
| `test/dmn-test-bluez-gatt-client.cpp` | Private-bus fake-BlueZ integration tests |
| `test/dmn-test-bluez-device-list.cpp` | Private-bus Linux integration tests for known/connected-device enumeration, discovery, and live updates |
| `examples/dmn-ancs-monitor.cpp` | Minimal selected-device app using the public API |

Build the protocol library behind `ENABLE_ANCS` (off by default); it has no
Bluetooth or D-Bus dependency and can be tested on any supported core platform.
`ENABLE_DBUS` (off by default) builds `dmn-dbus`, containing both the existing
byte-signal API and the new generic D-Bus client/value capability, and owns the
optional `dbus-1` dependency. Build the Dmn-owned adapter/device/GATT client
behind `ENABLE_BLUEZ` (off by default, Linux-only); it requires
`ENABLE_DBUS=ON` and builds `dmn-bluez` on top of `dmn-dbus`. `ENABLE_BLUEZ`
must not require `ENABLE_ANCS`, so `dmn-bluez-discovery` and device inventory
can be built and used independently. `dmn-bluez-discovery` is built whenever
`ENABLE_BLUEZ` is enabled. Build the ANCS client when `ENABLE_ANCS` is enabled; build
`dmn-ancs-monitor` only when both `ENABLE_ANCS` and `ENABLE_BLUEZ` are enabled.
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
5. Hardware tests are opt-in and manual/qualification tests; never make CI
   depend on a paired iPhone or Bluetooth adapter. Include a Linux smoke test
   confirming the example lists actual connected devices, starts/stops a
   user-requested scan, tracks discovered-device changes, and tracks a
   connect/disconnect.
6. Add tests at each phase gate before wiring the next layer.

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
