# Implementation Plan: Layered ANCS Client for Linux

**Status:** Proposed. This plan implements the contract in
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md). Do not begin
production integration until the real-device feasibility gate passes.

## 1. Recommended minimal solution

Build a small, directly linked C++ application on top of libDmn:

```text
dmn-ancs-monitor (example application)
  -> Dmn_BluezClient (adapter/device inventory and explicit discovery)
  -> optional selected iPhone:
       Dmn_AncsClient (ANCS lifecycle, commands, parser, bounded cache)
       -> Dmn_GattClient (backend-neutral async contract)
       -> Dmn_BluezGattClient (BlueZ system D-Bus backend)
       -> BlueZ -> BLE -> iPhone
```

The application initially displays the current connected-device inventory and
updates it as devices change; an explicit scan control shows nearby
BlueZ-reported devices. The user can select one eligible iPhone to connect and
start the ANCS session; notification updates are emitted to stdout or an
application callback. The app can show connected known devices and
discovered-but-disconnected devices separately. Neither view promises every
nearby radio device. Do not start by building a system daemon, notification
database, libnotify dependency, or IPC protocol. Those features do not reduce
the primary risk: whether the target iPhone exposes usable ANCS to the target
BlueZ environment.

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

### Phase 0 — Physical-device feasibility spike

Before designing a broad public API:

1. Record Linux distribution/kernel, BlueZ version, adapter chipset/firmware,
   iPhone model, and iOS version.
2. Pair the phone using the normal BlueZ/user workflow. For any disposable
   feasibility probe, call BlueZ D-Bus interfaces through libdbus directly;
   do not use a third-party Bluetooth wrapper.
3. Verify the inventory enumerates known devices across adapters. Start
   adapter-scoped discovery explicitly; verify nearby results update live,
   stopping releases this client's session, and scan/list operations do not
   pair or connect devices.
4. Use `bluetoothctl` and a small disposable BlueZ D-Bus probe to establish an
   LE GATT connection, wait for service resolution, locate the ANCS service
   and three characteristics, subscribe to Data Source then Notification
   Source, and request one notification's app identifier/title/message.
   Confirm the operation is using LE; a generic `Connected` state or a
   Bluetooth Classic profile connection is not sufficient evidence.
5. Verify reconnect and whether ANCS stays available after screen lock,
   phone reboot, BlueZ restart, and power cycling. Record authorization prompts
   and any required phone settings. Verify whether existing notifications are
   reported at session start, but do not treat that as a guaranteed snapshot.

**Exit gate:** the target setup demonstrates a repeatable LE GATT connection,
ANCS discovery, authorization, both subscriptions, and at least one
attribute-response round trip, with an understood user pairing/authorization
flow. If this gate fails, stop and investigate OS/device policy instead of
building higher layers on a false assumption. A successful generic Bluetooth
pair/connect does not pass this gate.

### Phase 1 — Freeze protocol model and implement tests first

- Turn the acceptance matrix below into executable Google Test assertions;
  do not count skipped placeholders as coverage.
- Define the BlueZ device inventory row, snapshot/update event contract, and
  stable display-name fallback; add a fake ObjectManager fixture with multiple
  adapters and known, connected, disconnected, and newly discovered devices.
- Freeze the adapter-scoped discovery API, filter/transport contract, shared
  session semantics, and idempotent start/stop lifecycle. Include tests for
  filters, discovery signals, cancellation, and non-interference with other
  clients' discovery sessions.
- Freeze public C++ signatures, ownership, lifecycle, typed errors, event
  semantics, callback execution context, and shutdown behavior from the spec.
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
The fake-system-bus inventory integration tests must also prove enumeration
across adapters, live connection/add/remove updates, explicit discovery
start/stop and filter behavior, and no implicit pairing or connection calls.

### Phase 2 — Generic D-Bus client and BlueZ backend

**Phase 2A — generic D-Bus capability**

- Extend optional `dmn-dbus` with `Dmn_DbusClient` and owned typed D-Bus
  values, implemented directly with libdbus. Centralize the process-wide
  libdbus thread initialization so the existing signal endpoints and generic
  client use the same once-only runtime entry point.
- Preserve existing `Dmn_DbusInput`/`Dmn_DbusOutput` contracts and tests.
  Their signal tuple, `ay` payload, queues, and delivery semantics remain
  independent from the generic method/property/signal API.
- Test method calls/replies, properties, signal subscriptions,
  ObjectManager-shaped values, timeout/error/cancellation, malformed values,
  queue limits, callback context, and shutdown using a private D-Bus daemon
  with a fake service. Do not use the host system bus.

**Phase 2A exit gate:** generic client tests and all existing `dmn-dbus`
regression tests pass; public headers expose no libdbus types; the core
`dmn` target still has no libdbus dependency when `ENABLE_DBUS=OFF`.

**Phase 2B — BlueZ backend**

- Add optional Linux-only Dmn target `dmn-bluez`, built on
  `Dmn_DbusClient` from `dmn-dbus`. `ENABLE_BLUEZ` requires
  `ENABLE_DBUS=ON`; it must not enable that dependency implicitly. BlueZ code
  uses the generic Dmn client for method calls, ObjectManager/property access,
  and signals. Do not add or depend on a third-party Bluetooth/GATT client
  wrapper or GDBus/dbus-glib.
- Do not route BlueZ traffic through, or change the behavior of,
  `Dmn_DBusInput`/`Dmn_DBusOutput`; those are broadcast-signal endpoints, not a
  general BlueZ D-Bus client. Reuse the generic client and the once-only
  private libdbus runtime shared with those endpoints. Do not expose libdbus
  types through public headers.
- Implement ObjectManager enumeration, Adapter/Device lifecycle observation,
  `Device1.Connect`, service-resolution waiting, UUID lookup,
  `GattCharacteristic1.StartNotify`/`StopNotify`, and `WriteValue`.
- Implement `Adapter1.SetDiscoveryFilter`, `StartDiscovery`, and
  `StopDiscovery` with per-client session ownership and live discovery result
  updates. Starting/stopping discovery must not pair/connect/disconnect
  devices, and the API must account for BlueZ's shared scan/filter behavior.
- Expose a separate inventory API that lists every BlueZ-managed `Device1`
  across all adapters, then publishes live changes from object/property
  signals. Applications derive a connected-only view from `Connected` and
  scan results from active discovery. Inventory enumeration itself must be
  read-only and must not initiate discovery, pairing, or connections.
- Ensure signal/property callbacks are installed before enabling notifications.
- Map generic D-Bus transport/method failures and BlueZ remote errors to
  explicit library errors; invalidate characteristic paths on disconnect or
  service change. Follow Dmn's established bounded-wait, signal-dispatch, and
  shutdown patterns.
- Add fake-object integration tests that exercise the actual Dmn BlueZ API and
  generic D-Bus client over a private bus; do not require a physical adapter
  in normal CI and do not use the host system bus in tests.

**Phase 2B exit gate:** the fake BlueZ object tree passes every backend
contract test; standalone public headers compile when the optional target is
enabled. The `dmn-bluez` target depends on `dmn-dbus` and has no
Bluetooth/GATT wrapper dependency; with `ENABLE_DBUS=OFF`, the core target
does not acquire a libdbus dependency.

### Phase 3 — ANCS session and command scheduler

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

### Phase 4 — Example application and compatibility qualification

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

### Phase 1 acceptance matrix

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
| `DmnBluezDeviceListIncludesAllConnectedDevices` | Multiple adapters with connected and disconnected devices | Every connected BlueZ-managed device appears once; disconnected devices are absent |
| `DmnBluezDeviceInventoryIncludesKnownDisconnectedDevices` | Known paired/disconnected and discovered devices | Full inventory includes known objects; connected-only view filters on `Connected` |
| `DmnBluezDeviceListTracksConnectionAndObjectChanges` | Device property and ObjectManager signals | Inventory emits correct add/update/remove changes without stale entries |
| `DmnBluezDeviceListDoesNotChangeDeviceState` | Enumerate and observe inventory | No pairing, connection, disconnection, or discovery operation is initiated |
| `DmnBluezDiscoveryStartsAndStopsOwnedSession` | Start/stop through Adapter1 with active fake BlueZ service | Only this client's scan session is released; no pair/connect/disconnect call occurs |
| `DmnBluezDiscoveryAppliesFilterAndTracksResults` | LE/service filters and live discovery object/property signals | Requested filter is applied and results update; other clients' merged results are not assumed excluded |

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
| Optional Dmn `dmn-bluez` target: `include/dmn-bluez-client.hpp`, `include/dmn-bluez-gatt-client.hpp`; private implementation under `src/` | Linux adapter/device/discovery API and GATT backend built on `Dmn_DbusClient` from `dmn-dbus`; public headers do not expose libdbus types |
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
must not require `ENABLE_ANCS`, so device inventory and discovery can be used
independently. Build the ANCS client when `ENABLE_ANCS` is enabled; build
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
