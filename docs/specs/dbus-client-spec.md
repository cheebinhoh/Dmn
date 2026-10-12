# Generic D-Bus Client Capability and BlueZ Extension

**Status:** Proposed, implementation-ready architecture. The existing
`dmn-dbus` byte-signal endpoints are implemented. The generic method/property/
signal client and the BlueZ API described here are not yet implemented.

## 1. Decision

Extend the existing optional `dmn-dbus` capability with a reusable,
Dmn-owned **generic D-Bus client API** implemented directly on libdbus. Build
the Linux BlueZ client as a separate typed layer on top of that capability.
This reuses Dmn's current libdbus integration without turning its
byte-oriented endpoints into a BlueZ API.

```text
Linux application
  -> Dmn_BluezClient / Dmn_BluezGattClient          [optional dmn-bluez]
       -> Dmn_DbusClient / Dmn_DbusValue            [generic dmn-dbus API]
            -> private Dmn libdbus runtime
                 -> public libdbus C API
                      -> system D-Bus -> BlueZ org.bluez

Existing Dmn_DMesgDbus users
  -> Dmn_DbusInput / Dmn_DbusOutput                 [unchanged byte-signal API]
       -> same private Dmn libdbus runtime
            -> public libdbus C API
```

The `dmn-dbus` library owns the generic client and the existing signal
transport. The BlueZ library owns Bluetooth concepts and depends on
`dmn-dbus`; it does not use `Dmn_DbusInput`/`Dmn_DbusOutput`, construct raw
libdbus messages, or depend on GDBus/dbus-glib, `gattlib`, `tinyb`, or another
third-party Bluetooth/D-Bus wrapper. Dmn still uses libdbus directly: the
generic Dmn client is the direct libdbus boundary, and BlueZ builds on the
typed Dmn API above it.

The generic client is reusable by applications integrating with other D-Bus
services, but it is not a bus daemon, D-Bus server/object exporter, network
RPC tunnel, or untyped exposure of libdbus handles. Its value/message API is
typed and owned. BlueZ calls are ordinary D-Bus operations through this API;
BlueZ object paths, UUIDs, policies, and state machines remain in the BlueZ
layer.

## 2. Code-derived starting point and reuse contract

The current implementation is deliberately narrower:

| Existing code | Verified behavior | Reuse or boundary |
|---|---|---|
| `Dmn_DbusInput` / `Dmn_DbusOutput` in `src/dmn-dbus-io.cpp` | Private bus connection; exact configured signal match; `ay` payload; bounded queues; worker dispatch/send; status, errors, and idempotent shutdown. | Keep public behavior and payload/routing contract unchanged. Reuse common runtime ownership where suitable, not the endpoint API for BlueZ. |
| `Dmn_DbusConfig` | Signal tuple and byte-queue limits; empty address selects the session bus. | Keep it signal-specific. Add distinct client configuration; never reinterpret an empty signal address as the BlueZ system bus. |
| Private `initializeDbus()` in the endpoint translation unit | One `std::call_once` for its translation unit and `dbus_threads_init_default()`. | Move the once-only process initialization behind one compiled private D-Bus runtime entry point used by both existing endpoints and the new client. Do not create independent once-flags in multiple libraries. |
| Private RAII wrappers and `connectBus()` | Owns libdbus connection/message/error lifetimes; endpoint default is session bus, explicit address is used without fallback. | Reuse or consolidate private ownership/error helpers where doing so does not change endpoint behavior. The new client must support explicit session/system selection and explicit custom addresses; BlueZ must select system bus explicitly. |
| `Dmn_Proc` workers and endpoint shutdown | Cooperative stop, finite dispatch waits, join before filter removal/connection close. | Reuse the lifecycle pattern, not necessarily the same worker or dispatch implementation. No cancellation-based shutdown; no close while a worker may use the connection. |
| `dmn-test-dbus-support.hpp` and private-bus tests | Test-owned private D-Bus daemon and endpoint tests without requiring the host bus. | Extend for fake D-Bus services/objects. All new client and BlueZ tests use a private test bus. |

These are reuse candidates, not permission to couple method/property traffic
to signal endpoint semantics. Existing signal tests are regression gates for
any shared-runtime extraction. In particular, do not change the session-bus
default, match rules, byte signatures, queue/drop policy, delivery meaning,
status accounting, or shutdown behavior of the existing signal API as part of
adding BlueZ.

## 3. Generic `dmn-dbus` client contract

### 3.1 Public surface and data model

Add a generic Dmn-owned client API, with proposed public headers
`dmn-dbus-client.hpp` and `dmn-dbus-value.hpp`. Exact names may be finalized
before implementation, but the public contract must include:

- A client connection configuration with an explicit bus kind (`Session`,
  `System`, or caller-supplied address), finite timeout/resource limits, and
  no implicit fallback. The generic library may use either standard bus or an
  intentionally supplied address. The BlueZ API fixes production use to
  `System`; test address injection is explicit and cannot silently select a
  production fallback.
- Owned, type-preserving D-Bus values for all types needed to represent
  BlueZ's public API, including booleans, signed/unsigned integer widths,
  doubles, strings, object paths, signatures, byte arrays, arrays, dictionaries,
  structs, and variants. Preserve enough type/signature information to
  validate replies and distinguish an object path from a string. Recursive
  containers must be bounded by documented depth, element, and encoded-size
  limits.
- A method-call operation accepting a destination, object path, interface,
  member, and typed arguments; it returns a typed reply or a Dmn error that
  preserves a remote D-Bus error name/message separately from local transport,
  timeout, cancellation, and decode failures.
- Bounded asynchronous calls so the one connection dispatch owner is never
  blocked waiting for its own reply. A synchronous convenience operation may
  wait only on a non-dispatch caller thread and must have a finite timeout.
- Generic `org.freedesktop.DBus.Properties.Get`/`Set` operations implemented
  through method calls and typed variants; property changes arrive through
  signal subscriptions.
- Constrained signal subscriptions by sender/interface/member and optional
  path/path namespace, with owned decoded arguments and a cancellation token.
  Subscription callbacks do not receive borrowed `DBusMessage` or iterator
  pointers.
- Support for the standard `org.freedesktop.DBus.ObjectManager` calls and
  signals through the generic typed value API. The generic layer transports
  and validates D-Bus values; BlueZ interprets its object/interface/property
  model.
- A thread-safe status/error snapshot sufficient to observe connection state,
  pending work, queue pressure, and terminal disconnect/shutdown.

The public API does not expose `DBusConnection *`, `DBusMessage *`,
`DBusMessageIter`, raw libdbus ownership, or arbitrary callbacks into borrowed
libdbus memory. It does not export services or object methods. A client method
call may target any bus service the process is permitted to access, but the
generic API never bypasses bus policy or claims application-level
authorization.

### 3.2 Dispatch, callbacks, and lifetime

- Each client owns one private connection and one documented D-Bus I/O/
  dispatch owner. All connection operations that require dispatch participate
  in that ownership model.
- Method requests are submitted to the dispatch owner and completed through
  libdbus pending calls or an equivalent event-driven mechanism. Do not use
  `send_with_reply_and_block()` on the dispatch worker.
- The D-Bus dispatch callback performs bounded signature/type/size checks and
  copies data into owned values. It never invokes application callbacks or
  waits on a DMesg/BlueZ/application queue.
- Signal callbacks run on a documented Dmn-owned callback context, separate
  from the libdbus I/O/dispatch owner. Callback ordering is defined per
  connection/subscription. Callbacks must not be required to run on the
  process-wide `Dmn_Runtime` unless an explicit executor contract is added.
- Pending calls, signal subscriptions, callback queues, strings, arrays, and
  nested values have finite limits. Saturation, timeout, cancellation,
  malformed data, and disconnect are observable outcomes, never silent
  success.
- Every call completes at most once. A late reply after timeout/cancellation
  is discarded. Shutdown rejects new work, completes pending calls with a
  shutdown outcome, cancels subscriptions, stops and joins dispatch/callback
  workers, then closes/unrefs the connection.
- Shutdown is idempotent. No worker is cancelled with `pthread_cancel`; no
  connection/filter/callback state is destroyed before all workers using it
  have joined. Concurrent destruction with active public method calls is
  unsupported unless the eventual public contract explicitly adds shared
  lifetime ownership.

The implementation plan must select concrete default limits and finite call
timeouts before code is written. Tests must exercise those thresholds at and
over the boundary rather than merely proving that values are nonzero.

### 3.3 BlueZ system-bus policy

`Dmn_BluezClient` always connects to the **system bus** in production and
addresses the documented `org.bluez` service. It does not use
`Dmn_DbusConfig`'s session-bus default, try the session bus after a system-bus
failure, or silently switch to a custom address. Fake-service tests inject a
private bus through a test-only seam.

Dmn does not request unrestricted system-bus policy. Deployment documentation
must identify the narrow send/receive policy and user permissions required
for the BlueZ operations actually used. Authentication by the local bus does
not imply permission to pair/connect, or trust in any remote Bluetooth peer.
Policy denial and missing BlueZ service are explicit typed errors.

## 4. BlueZ layer built on the generic capability

The optional Linux-only `dmn-bluez` target depends on `dmn-dbus`. BlueZ code
uses `Dmn_DbusClient`, `Dmn_DbusValue`, typed errors, and subscriptions; it
does not directly include libdbus headers or own a second connection runtime.
Its public C++ API is domain-specific and does not let applications issue
arbitrary BlueZ calls.

BlueZ owns:

- Adapter and known-device enumeration using ObjectManager snapshots.
- Adapter/device properties and `InterfacesAdded`, `InterfacesRemoved`,
  `PropertiesChanged`, and BlueZ owner-change handling.
- Explicit adapter-scoped discovery, filter application, and release of only
  the session requested by this client.
- Explicit user-selected device connect/disconnect behavior. Inventory and
  scanning never pair or connect.
- GATT service/characteristic discovery, readiness tracking, reads, writes,
  notifications, and characteristic path invalidation on disconnect/service
  change.
- Mapping of BlueZ remote error names and generic D-Bus errors into typed
  BlueZ outcomes.

ANCS remains a protocol-neutral layer above the backend-neutral GATT
interface. It has no BlueZ/libdbus dependency. The monitor/example is an
application layer and must display currently connected BlueZ-known devices
separately from nearby discovery results.

## 5. Build and installation contract

- `ENABLE_DBUS` remains off by default. When on, it builds/installs
  `dmn-dbus`, containing the existing signal endpoints plus the generic
  client/value capability. It discovers and links `dbus-1`; core `dmn`
  remains independent of libdbus. Update the option description to identify
  both generic D-Bus client and byte-signal support.
- `ENABLE_BLUEZ` is off by default, Linux-only, and requires
  `ENABLE_DBUS=ON`. Configuration with BlueZ enabled and D-Bus disabled fails
  with a clear instruction; it must not silently enable unrelated features.
- When `ENABLE_BLUEZ=ON`, build/install `dmn-bluez` and its public headers.
  `dmn-bluez` links to `dmn-dbus`, which owns libdbus runtime initialization
  and connection mechanics. BlueZ must not discover or link a second D-Bus
  wrapper or private duplicate initialization library.
- `ENABLE_ANCS` remains optional and does not require BlueZ for parser,
  scheduler, and protocol-only tests. The example monitor requires both
  `ENABLE_ANCS` and `ENABLE_BLUEZ`.
- With `ENABLE_DBUS=OFF`, core `dmn` has no libdbus dependency and the
  generic D-Bus/BlueZ headers are not installed. With `ENABLE_DBUS=ON` and
  `ENABLE_BLUEZ=OFF`, generic D-Bus client and existing signal transport
  build without BlueZ. With both enabled, the typed BlueZ library builds on
  the same Dmn client capability.
- Public headers never include `<dbus/dbus.h>`. Build-interface definitions
  and include directories must follow the repository's existing optional
  target conventions.

## 6. Incremental implementation and gates

### Step 0 — Preserve baseline

Record/build the current `ENABLE_DBUS=ON` signal endpoint and facade test
results. Keep all existing public endpoint tests passing before and after each
shared-runtime change. Existing ANCS/BlueZ `GTEST_SKIP()` placeholders do not
count as behavioral coverage.

### Step 1 — Extract shared libdbus runtime

Move process-wide `dbus_threads_init_default()` behind a single compiled
private runtime entry point used by both the existing endpoints and the
generic client. Consolidate only ownership/error/connection helpers that can
preserve current endpoint semantics. Keep signal endpoints on their existing
private connections and workers unless a measured, tested design proves a
shared connection is behaviorally identical. Add tests proving initialization
is safe across concurrent construction of old and new client types, and retain
all existing signal queue, routing, status, and shutdown tests.

### Step 2 — Generic client and D-Bus value codec

Implement generic method calls/replies, type-safe marshalling/unmarshalling,
properties, signal subscriptions, ObjectManager value support, timeout/error
mapping, limits, status, and shutdown. Use a private bus with a fake D-Bus
service. Test exact signatures and values, malformed signatures/types,
remote errors, timeout, cancellation/late reply, disconnect, queue
saturation, callback context, and concurrent shutdown. No BlueZ dependency is
introduced in this step.

### Step 3 — BlueZ inventory and discovery

Implement the typed adapter/device API on `dmn-dbus`. Against a private bus
with fake ObjectManager/Adapter1/Device1 objects, test multiple adapters,
known connected and disconnected devices, live changes/removal, display-name
fallback, explicit scan filters/start/stop, and proof that listing/scanning
does not pair or connect devices.

### Step 4 — BlueZ GATT and ANCS protocol

Implement BlueZ GATT lifecycle and operations on the generic client; test
methods, properties, characteristic notifications, BlueZ error mapping,
service resolution and invalidation using fake objects. Implement ANCS parser,
fragment reassembly, serialized requests, timeout and session state against a
fake GATT interface without D-Bus dependency.

### Step 5 — Example and real-device gate

Add the selected-device monitor only after fake-bus behavior is executable.
Qualify actual LE GATT, ANCS service/characteristics, authorization,
subscriptions, and attribute response on the target iPhone/BlueZ/adapter
matrix. A generic Connected property, successful method reply, or passing
fake-bus tests is not evidence that iPhone ANCS is available.

## 7. Acceptance tests

Generic D-Bus client coverage must include:

- Concurrent safe client/endpoint construction and exactly-once libdbus
  thread initialization.
- Session, system, and explicit-address connection selection; explicit
  connection failure never falls back.
- Round-trip scalar, byte-array, nested array/dictionary/struct/variant values
  with exact signatures, including ObjectManager-shaped values.
- Method replies, remote named errors, timeout, cancellation, late reply, bus
  disconnect, and bounded pending-call behavior.
- Property get/set and signal add/change/remove flows; exact match scoping and
  subscription cancellation.
- Malformed and oversized values, queue saturation, deterministic callback
  context/order, and shutdown while calls/signals are in flight.
- No user callback runs on the libdbus dispatch owner; no callback receives
  borrowed message storage; every operation has one observable completion.

BlueZ coverage must exercise the actual `dmn-bluez` API over the generic Dmn
client against a private bus with fake `org.bluez` objects. It must not test
only private parsers or call the host system bus. ANCS protocol tests remain
independent of both BlueZ and D-Bus.

## 8. Relationship to existing D-Bus and BlueZ specifications

This document specifies the new generic client capability and its BlueZ
extension boundary. The byte-signal transport's existing wire and lifecycle
contract remains in [`dmesg-dbus-spec.md`](dmesg-dbus-spec.md). The
BlueZ/ANCS functional contract and device-facing behavior remain in
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md), with the
implementation gates in [`ancs-bluez-client-plan.md`](ancs-bluez-client-plan.md).
These documents are complementary: the generic client supplies D-Bus
mechanics, the BlueZ layer supplies Bluetooth semantics, and the signal
endpoints continue to supply byte-oriented DMesg transport.

## 9. References

- BlueZ [Adapter API](https://bluez.readthedocs.io/en/latest/adapter-api/),
  [Device API](https://bluez.readthedocs.io/en/latest/device-api/), and
  [GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/).
- D-Bus [message-bus specification](https://dbus.freedesktop.org/doc/dbus-specification.html#message-bus)
  and [libdbus connection API](https://dbus.freedesktop.org/doc/api/html/group__DBusConnection.html).
- Repository baseline: [`dmesg-dbus-spec.md`](dmesg-dbus-spec.md) and
  [`dmn-proc-dbus-worker-integration-spec.md`](dmn-proc-dbus-worker-integration-spec.md).
