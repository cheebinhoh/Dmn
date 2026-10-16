# Generic D-Bus Client Capability and BlueZ Extension

**Status:** The generic D-Bus client implementation and its focused
private-bus API tests are implemented. Broader robustness and BlueZ
qualification gates remain; see Section 6. The existing `dmn-dbus`
byte-signal endpoints remain implemented. The initial BlueZ inventory/
discovery API and CLI and the backend-neutral/BlueZ GATT API are implemented.
The ANCS protocol client and monitor CLI are not implemented.

The current client slice provides typed scalar/container values, bounded
method calls with timeout/cancellation, properties, constrained signal
subscriptions, callback-worker delivery, status snapshots, and a shared
libdbus thread runtime. It is not yet considered BlueZ-integration-ready:
see the remaining implementation and acceptance gates in Section 6.

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

For method calls, properties, and decoded signals, the typed-value path is:

```text
BlueZ API                    Generic D-Bus API                Transport boundary
Dmn_BluezClient  <-------->  Dmn_DbusClient  <----------->    private codec
service-specific meaning     Dmn_DbusValue arguments/replies  libdbus messages
                                                                |
                                                                v
                                                          D-Bus / BlueZ
```

`Dmn_DbusValue` is the owned data model carried across the generic client API;
it is not a transport or a BlueZ model. The private codec in
`src/dmn-dbus-client.cpp` is responsible for translating between those values
and libdbus message bodies. `Dmn_DbusClient` handles bus operations and
delivers owned typed replies/signals. `Dmn_BluezClient` interprets those
generic values as BlueZ interfaces, properties, and events and exposes
Bluetooth-specific types to its callers. This separation also lets other
service clients use `Dmn_DbusClient` and `Dmn_DbusValue` without depending on
BlueZ.

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

### D-Bus header relationships

The public header dependencies are:

```text
dmn-dbus-client.hpp ───────> dmn-dbus-value.hpp
dmn-dbus-io.hpp ───────────> dmn-dbus-config.hpp
                         └─> dmn-io.hpp
dmn-dmesgnet-dbus.hpp ─────> dmn-dbus-config.hpp
                         └─> dmn-dmesg.hpp

dmn-dbus-config.hpp          (standalone configuration/status types)
dmn-dbus-value.hpp           (standalone value model)
dmn-dbus-runtime.hpp         (internal initialization declaration)
```

`dmn-dbus-runtime.hpp` is not a dependency of the public client or endpoint
headers. At implementation level, `src/dmn-dbus-client.cpp` and
`src/dmn-dbus-io.cpp` include it and call `dmn::detail::initializeDbusRuntime()`
before using libdbus. The function is implemented in
`src/dmn-dbus-runtime.cpp` and initializes libdbus threading once per process.
The `Dmn_DMesgDbus` implementation composes `Dmn_DbusInput`,
`Dmn_DbusOutput`, and `Dmn_DMesgNet`; its public header depends on the
configuration and DMesg APIs but does not expose the endpoint implementation
headers.

## 2. Code-derived starting point and reuse contract

The current implementation is deliberately narrower:

| Existing code                                               | Verified behavior                                                                                                                                   | Reuse or boundary                                                                                                                                                                                                                            |
| ----------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Dmn_DbusInput` / `Dmn_DbusOutput` in `src/dmn-dbus-io.cpp` | Private bus connection; exact configured signal match; `ay` payload; bounded queues; worker dispatch/send; status, errors, and idempotent shutdown. | Keep public behavior and payload/routing contract unchanged. Reuse common runtime ownership where suitable, not the endpoint API for BlueZ.                                                                                                  |
| `Dmn_DbusConfig`                                            | Signal tuple and byte-queue limits; empty address selects the session bus.                                                                          | Keep it signal-specific. Add distinct client configuration; never reinterpret an empty signal address as the BlueZ system bus.                                                                                                               |
| Private `initializeDbus()` in the endpoint translation unit | Previously used one translation-unit-local `std::call_once`.                                                                                        | The once-only initialization now lives in `src/dmn-dbus-runtime.cpp` and is used by both existing endpoints and the generic client. Keep this as the only libdbus thread initializer.                                                        |
| Private RAII wrappers and `connectBus()`                    | Owns libdbus connection/message/error lifetimes; endpoint default is session bus, explicit address is used without fallback.                        | Reuse or consolidate private ownership/error helpers where doing so does not change endpoint behavior. The new client must support explicit session/system selection and explicit custom addresses; BlueZ must select system bus explicitly. |
| `Dmn_Proc` workers and endpoint shutdown                    | Cooperative stop, finite dispatch waits, join before filter removal/connection close.                                                               | Reuse the lifecycle pattern, not necessarily the same worker or dispatch implementation. No cancellation-based shutdown; no close while a worker may use the connection.                                                                     |
| `dmn-test-dbus-support.hpp` and private-bus tests           | Test-owned private D-Bus daemon and endpoint tests without requiring the host bus.                                                                  | Extend for fake D-Bus services/objects. All new client and BlueZ tests use a private test bus.                                                                                                                                               |

These are reuse candidates, not permission to couple method/property traffic
to signal endpoint semantics. Existing signal tests are regression gates for
any shared-runtime extraction. In particular, do not change the session-bus
default, match rules, byte signatures, queue/drop policy, delivery meaning,
status accounting, or shutdown behavior of the existing signal API as part of
adding BlueZ.

## 3. Generic `dmn-dbus` client contract

### 3.1 Public surface and data model

The initial generic Dmn-owned client API is provided by the public headers
`dmn-dbus-client.hpp` and `dmn-dbus-value.hpp`. Its current public contract
includes:

- A client connection configuration with an explicit bus kind (`Session`,
  `System`, or caller-supplied `Address`), finite timeout/resource limits,
  and no implicit fallback. All three choices create a client-owned private
  connection: `Session` connects privately to the user's session bus,
  `System` connects privately to the machine's system bus, and `Address`
  opens a private connection to the exact supplied bus address. The address
  may identify an isolated/private test daemon or another explicitly
  addressed bus. “Private” describes ownership of the connection, not a
  fourth bus kind; the ordinary system and session buses are bus daemons
  shared by many clients. The BlueZ API fixes production use to `System`;
  test address injection is explicit and cannot silently select a production
  fallback.
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
- Bounded asynchronous calls so the connection dispatch worker is never
  blocked waiting for a reply. A synchronous convenience operation may wait
  only on a non-dispatch caller thread and must have a finite timeout.
- Generic `org.freedesktop.DBus.Properties.Get`/`Set` operations implemented
  through method calls and typed variants; property changes arrive through
  signal subscriptions.
- Constrained signal subscriptions by sender/interface/member and optional
  path/path namespace, with owned decoded arguments and a cancellation token.
  Subscription callbacks do not receive borrowed `DBusMessage` or iterator
  pointers.
- A callback-queue drain barrier that waits for signal callbacks already
  queued before the barrier to finish. It cannot be called from the callback
  worker itself.
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

### D-Bus message types and method request/reply flow

In D-Bus, a **message** is one protocol unit sent over a connection. The wire
protocol defines four message types:

| D-Bus message type | Purpose                                                           | Typical direction                            |
| ------------------ | ----------------------------------------------------------------- | -------------------------------------------- |
| Method call        | Request an operation or information from a service.               | Caller to service                            |
| Method return      | Carry a successful result for a method call.                      | Service to caller                            |
| Error              | Report that a method call failed.                                 | Service to caller                            |
| Signal             | Announce an event to matching listeners; no method reply follows. | Sender through the bus to matching listeners |

A method call identifies its destination bus name, object path, interface,
member (method name), and typed arguments. For example, a caller can invoke
`Get` on `org.freedesktop.DBus.Properties` for a particular object and
property. The bus daemon routes the call to the process that owns the
destination name. That service handles the request and sends either a method
return or an error; the daemon routes that response to the original caller.
The request has a message serial, and the response carries that serial as its
reply serial. This lets the caller-side connection associate it with the right
outstanding call. Thus the request and result are two distinct D-Bus messages
and two directions of traffic, but are one logical request/reply operation
for the caller.

```text
Client process             Bus daemon                Service process
     |                           |                          |
     |-- MethodCall ------------>|                          |
     |                           |-- routed call ---------->| executes method
     |                           |<-- MethodReturn/Error ---|
     |<-- routed result ---------|                          |
```

Signals have a different interaction pattern: a sender emits one signal, and
the daemon routes it to connections whose installed match rules accept it.
There is no per-listener method return or acknowledgement. D-Bus properties
are not a fifth message type: `Properties.Get` and `Properties.Set` are method
calls with a method return or error, while `PropertiesChanged` is a signal.

`Dmn_DbusClient` uses one bidirectional connection to the selected bus daemon;
it does not open one channel for sending and another for listening. An API
caller submits a call through libdbus's thread-safe connection API. The client
registers it as a pending call, then its I/O/dispatch worker reads and
dispatches incoming traffic. When the matching method return or error arrives,
libdbus completes that pending call and Dmn's completion handler decodes the
reply into owned `Dmn_DbusValue` arguments (or records the typed error) and
fulfills the call's result future. This worker waits through the libdbus
read/dispatch API; it is not a busy-spin and does not require application code
to issue a separate receive call.

With `callAsync()`, the caller receives a `Dmn_DbusCall` handle and can
continue other work, then wait for or retrieve its result. With `call()`, the
calling thread waits on that same asynchronous operation; the dispatch worker
continues processing the reply. A reply does not have to wake the original
thread directly: it completes the future, which releases whichever caller is
waiting on it. Calls have finite deadlines; a remote D-Bus error, timeout,
disconnect, cancellation, or shutdown produces an error result, not a
successful reply.

The client tests exercise this round trip against an isolated test bus. In
`test/dmn-test-dbus-client.cpp`, `PrivateBus` starts a real test-owned bus
daemon and `FakeDbusService` connects to it and owns the well-known name
`org.dmn.TestService`. For the `Echo` case, the client calls
`/org/dmn/Test`, interface `org.dmn.Test`, member `Echo`, with typed values.
The fake service receives the call, builds a method-return message for that
request, copies the arguments into it, and sends it through the bus. The client
decodes the returned values, and the test checks their values and signatures.
The service's `Fail` method similarly demonstrates an error reply, while its
`Delay` method exercises a response that arrives after a deliberate delay. The
test service is fake; the bus daemon and the routed request/reply exchange are
real.

#### Initial API and limits

The first implementation uses the public headers `dmn-dbus-value.hpp` and
`dmn-dbus-client.hpp`. `Dmn_DbusValue` is an owned recursive value; callers
construct container values with an explicit element/key signature so empty
arrays and dictionaries retain their D-Bus type. Dictionary entries preserve
their key and value types. Struct fields preserve order. A variant contains
exactly one owned value and contributes the contained value's signature inside
the `v` wrapper.

The initial defaults are deliberately below libdbus's protocol-wide maxima:

| Resource                                                 | Default limit                                     | Enforcement                                                                                                                                |
| -------------------------------------------------------- | ------------------------------------------------: | ------------------------------------------------------------------------------------------------------------------------------------------ |
| Method timeout                                           | 5 seconds                                         | Per call; positive finite millisecond duration                                                                                             |
| In-flight method calls                                   | 128                                               | Admission fails with a typed `ResourceLimit` result                                                                                        |
| Additional call/subscription cap (`max_queued_commands`) | 256                                               | Caps active subscriptions and participates in the effective call cap; there is no separate local command FIFO                              |
| Signal callback queue                                    | 256 callback deliveries / 16 MiB serialized bytes | A signal matching multiple subscriptions is queued/charged once per callback; a signal that would exceed either cap is dropped and counted |
| Serialized method-call/reply/signal message              | 8 MiB                                             | Checked during encode/decode; includes D-Bus framing, not only the argument body                                                           |
| Recursive container depth                                | 32                                                | Decode/encode rejects deeper trees                                                                                                         |
| Values/entries per message                               | 65,536                                            | Decode/encode rejects the first excess item                                                                                                |

Limits are finite and immutable for the initial API. The maximum per-call
timeout is also bounded by the platform-supported libdbus integer timeout.
An empty method argument list is valid. D-Bus message signatures remain subject
to the protocol's 255-byte signature maximum.

The initial result type distinguishes local invalid input, not connected,
transport failure/disconnect, timeout, cancellation, shutdown, resource-limit,
remote named error, and decode/encode failure. Remote error name and text are
retained separately from the local category. The synchronous `call()` helper
is a wrapper over `callAsync()` and waits only on its calling thread; it is
documented as unsuitable on the client's signal-callback thread.
All D-Bus identifiers and addresses must reject embedded NUL bytes before
validation or passing them to libdbus, so C-string truncation cannot silently
change the requested name, path, member, or connection address.

Signal subscription matching supports an optional well-known or unique sender,
interface, member, exact object path, or path namespace. At least interface
and member are required. The daemon receives a constrained match rule; the
client repeats the match locally. Subscription tokens are move-only and
cancellation is idempotent. Callback arguments are owned values and callbacks
run serially on one client-owned callback worker, never on the libdbus
dispatching thread. User callbacks must not destroy the client from that
callback worker; shutdown from that context would self-join.

### 3.2 Dispatch, callbacks, and lifetime

- Each client owns one private connection and one D-Bus I/O/dispatch worker.
  API caller threads submit method requests through libdbus's thread-safe
  connection API; the dispatch worker runs the read/dispatch loop and all
  libdbus message filters.
- Method requests use `dbus_connection_send_with_reply()` and complete through
  libdbus pending calls. Submission does not wait for the reply; the dispatch
  worker processes replies and invokes pending-call completion handlers. Do
  not use `send_with_reply_and_block()` on the dispatch worker.
- The D-Bus dispatch callback performs bounded signature/type/size checks and
  copies data into owned values. It never invokes application callbacks or
  waits on a DMesg/BlueZ/application queue.
- A caller needing a snapshot/event ordering boundary drains the callback
  worker after the corresponding D-Bus reply; signals queued before that reply
  are processed before the caller closes its staging window.
- Signal callbacks run on a documented Dmn-owned callback context, separate
  from the libdbus I/O/dispatch worker. Callback ordering is defined per
  connection/subscription. Callbacks must not be required to run on the
  process-wide `Dmn_Runtime` unless an explicit executor contract is added.
- Pending calls, signal subscriptions, callback deliveries, strings, arrays,
  and nested values have finite limits. Saturation, timeout, cancellation,
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
- There is no separate local command FIFO: a call admitted by the in-flight
  limit is submitted to libdbus immediately. Cancellation cancels the pending
  call when possible; if the request has already reached the service,
  cancellation does not undo remote work. Cancellation racing a reply still
  produces exactly one result. Timeout and cancellation discard later replies
  and immediately remove the call from the client's pending registry,
  releasing its pending-call reference rather than retaining canceled/timed
  out calls until shutdown.
- Shutdown completes pending calls with a shutdown result, discards
  queued callbacks while recording their count/bytes, and stops all workers
  before closing the connection. Subscription tokens may outlive the client;
  destroying one after shutdown is harmless because its cancellation action
  no longer has a live subscription to remove.

Tests must exercise each configured threshold at and over the boundary rather
than merely proving that values are nonzero.

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
- Explicit user-selected device pair/connect/disconnect behavior. Inventory
  and scanning never pair or connect.
- GATT service/characteristic discovery, readiness tracking, acknowledged
  writes, notifications, and characteristic path invalidation on
  disconnect/service change. The current GATT API does not expose a generic
  characteristic-read operation; ANCS attribute responses arrive through its
  Data Source notification.
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

#### Current implementation review

`dbus_threads_init_default()` is shared through
`src/dmn-dbus-runtime.cpp`; both legacy byte endpoints and
`Dmn_DbusClient` call that entry point. Existing endpoint semantics remain on
their prior private connections and workers. The generic client is implemented
in `src/dmn-dbus-client.cpp`, with owned values in `dmn-dbus-value.*`; it uses
pending calls dispatched by one client-owned I/O thread and a separate serial
signal-callback worker.

The first client test commit was deliberately red because the public client
header and implementation did not exist. The private fake-service suite now
covers primitive and byte round trips, remote errors, finite timeout,
properties, callback thread separation and cancellation, exact-path signal
scoping, ObjectManager-shaped decode and encode, in-flight saturation, call
cancellation/late replies, shutdown and bus disconnect with calls in flight,
concurrent construction with both legacy endpoints, visible signal queue
overflow, invalid input/type mismatch, depth and encoded-size rejection, and
explicit-address failure. The suite now also exercises the public handle
classes' move/cancel/destruction rules, method/property error paths,
`subscribeWithPath`, path-namespace boundaries, overlapping-subscription
ordering, callback-drain rejection from its own worker, shutdown admission,
both bus selectors against private daemons, and exact/over resource
boundaries. Legacy D-Bus endpoint/facade and DMesg conflict/lifetime
regressions remain required gates.

Every public operation in `dmn-dbus-client.hpp` now has executable success
and/or failure-path coverage against a standard Linux `dbus-daemon` started by
the test fixture. Tests use a test-owned private bus address and fake D-Bus
service; the system-bus selection case sets libdbus's documented
`DBUS_SYSTEM_BUS_ADDRESS` override to that same private daemon. Tests do not
connect to the host session/system bus, replace the Dmn client with a mock, or
use a custom Dmn-specific bus. This validates the actual Dmn client, codec,
libdbus integration, and bus protocol without relying on host bus policy or
services.

Remaining readiness work is narrower than basic API usage coverage:
malformed-but-well-formed wire replies should be explicitly injected for
property/reply-shape decoder failures; add exhaustive stress/race repetition
for disconnect versus completion/cancellation; and retain the normal endpoint
and broader sanitizer/build gates. The client intentionally treats bus
disconnect as terminal rather than reconnecting. `GTEST_SKIP()` ANCS/BlueZ
scaffolds are not behavioral coverage. The initial BlueZ inventory/
device-action and GATT APIs are implemented, with private-bus coverage for
their current behavior. Broader BlueZ lifecycle acceptance cases and
real-device GATT/ANCS qualification remain separate open gates; private-bus
success does not establish iPhone compatibility.

### Step 1 — Extract shared libdbus runtime (implemented)

The process-wide initializer is implemented as one compiled private runtime
entry point. Legacy signal endpoints retain their existing private connections
and workers. Existing D-Bus endpoint/facade tests pass after the extraction.
Concurrent construction across endpoint types remains a test gate.

### Step 2 — Generic client and D-Bus value codec (initial slice implemented)

The current slice implements generic method calls/replies, recursive owned
values, properties, signal subscriptions, ObjectManager-shaped values,
timeout/cancellation/error mapping, configured resource limits, status, and
shutdown. It is exercised against an owned private bus and fake service; no
BlueZ dependency has been introduced.

Do not mark this step complete until the remaining Section 6 implementation
gates and the Section 7 acceptance tests pass. The public client API usage
matrix is covered by private-bus tests; broader stress, malformed-reply, and
production BlueZ qualification gates remain distinct requirements.

### Step 3 — BlueZ inventory/discovery API and CLI (initial implementation)

The optional `dmn-bluez` target and `dmn-bluez-discovery` CLI provide the first
typed adapter/device snapshot, updates, and explicit scan. Private-bus tests
cover known disconnected devices, property changes, discovery filter and
owned start/stop, and the absence of forbidden Pair/Connect/Disconnect/
RemoveDevice/SetPowered operations during inventory and scanning. Explicit
pairing is a separate action and uses the BlueZ default agent. A
physical-adapter CLI scan has also been run. Continue with the full
multi-adapter, removal, restart, race, failure, and CLI acceptance cases in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md); do not block
this first milestone on GATT or ANCS.

### Step 4 — BlueZ GATT and ANCS protocol

The backend-neutral `Dmn_GattClient` and BlueZ GATT lifecycle/operations are
implemented and covered by private-bus fake-object tests for methods,
properties, characteristic notifications, BlueZ error mapping, service
resolution, and invalidation. Remaining work is the ANCS parser, fragment
reassembly, serialized requests, timeout, and session state against a fake
GATT interface without a D-Bus dependency.

### Step 5 — Example and real-device gate

Add the selected-device monitor only after fake-bus behavior is executable.
Qualify actual LE GATT, ANCS service/characteristics, authorization,
subscriptions, and attribute response on the target iPhone/BlueZ/adapter
matrix. A generic Connected property, successful method reply, or passing
fake-bus tests is not evidence that iPhone ANCS is available.

## 7. Acceptance tests

### 7.1 Generic client API coverage

The unit-test executables are `dmn-test-dbus-client` and
`dmn-test-dbus-client-session`. The main fixture starts the standard Linux
`dbus-daemon` in an isolated private session configuration and registers a
fake service on that bus. The isolated session-selector test starts its own
private daemon before the libdbus session address is first resolved. No test
uses the host session or system bus. The system selector is also exercised
safely by pointing `DBUS_SYSTEM_BUS_ADDRESS` at the fixture-owned daemon.
This is the real D-Bus protocol/daemon and real libdbus transport, not a
custom Dmn bus and not a mocked `Dmn_DbusClient`.

Each public class/API surface is covered as follows:

| Public API                                                                                                                                | Executable coverage                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `Dmn_DbusSubscription` construction, move construction/assignment, `cancel()`, destruction, idempotence, cancellation callback exceptions | `DbusClientHandleTest.SubscriptionMoveAssignmentAndCancellationAreIdempotent`; `DbusClientHandleTest.SubscriptionMoveConstructorTransfersCancellation`; `DbusClientHandleTest.DestroyingActiveSubscriptionCancelsExactlyOnce`; `DbusClientHandleTest.HandleCancellationSuppressesCallbackExceptions`; client-level token behavior in `DbusClientTest.DeliversOwnedSignalsOffDispatchThreadAndCancels`.                                                                                                                    |
| `Dmn_DbusCall` construction, `waitFor()`, `get()`, move construction/assignment, `cancel()`, handle destruction semantics                 | `DbusClientHandleTest.CallMoveWaitGetAndCancelOwnOneResult`; `DbusClientHandleTest.CallMoveAssignmentTransfersResultAndCancellation`; `DbusClientHandleTest.DestroyingCallHandleDoesNotCancelOperation`; `DbusClientHandleTest.HandleCancellationSuppressesCallbackExceptions`; client cancellation/late-reply, timeout, shutdown, and asynchronous completion tests.                                                                                                                                                     |
| `Dmn_DbusClient` construction and bus configuration                                                                                       | `DbusClientSessionTest.DefaultSessionSelectionUsesPrivateDaemon` runs in an isolated test process for default session selection; `DbusClientSessionTest.ExplicitAddressSelectsPrivateBusDaemon` connects to an explicitly addressed isolated bus; `DbusClientInitializationTest.SystemBusSelectionUsesPrivateDaemonOverride`; `DbusClientTest.RejectsInvalidConnectionConfigurationWithoutFallback`; `DbusClientTest.RejectsEveryZeroOrOutOfRangeResourceLimit`; concurrent runtime initialization with legacy endpoints. |
| `call()` / `callAsync()`                                                                                                                  | Primitive/byte values, nested array/dictionary/struct/variant calls, remote error, timeout, bad identifiers, exact/over message-byte bounds on arguments and value/depth bounds on arguments and replies, in-flight and queued-command/subscription saturation, cancellation, late reply, disconnect, shutdown, and repeat-shutdown/admission cases.                                                                                                                                                                      |
| `getProperty()` / `setProperty()`                                                                                                         | Typed get/set, remote unknown-property and wrong-value-type errors, and malformed property reply shapes in `DbusClientTest.GetsAndSetsTypedPropertiesThroughPropertiesInterface`.                                                                                                                                                                                                                                                                                                                                         |
| `subscribe()` / `subscribeWithPath()`                                                                                                     | Callback execution context and ownership, exact path and path namespace filtering, retained path, invalid matches/callbacks, token cancellation, and overlap ordering.                                                                                                                                                                                                                                                                                                                                                    |
| `drainSignalCallbacks()`                                                                                                                  | Drains already-queued work; rejects invocation from the callback worker; reports shutdown.                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `dispatchThreadId()` / `status()`                                                                                                         | Stable worker identity, connection/pending/shutdown status, visible message/byte overflow and shutdown discards, and bus-disconnect state.                                                                                                                                                                                                                                                                                                                                                                                |
| `Dmn_DbusClientLimits`                                                                                                                    | Invalid zero/out-of-range configuration; exact and over value/message/signal-byte thresholds; pending/command/subscription and signal-queue caps; recursive depth and oversized reply tests.                                                                                                                                                                                                                                                                                                                              |

Additional codec/wire cases include scalar and byte-array signatures,
ObjectManager-shaped nested dictionary/variant values in both directions,
remote error-name/message preservation, and owned values delivered to callbacks.

### 7.2 Remaining broader integration gates

The public API behavior matrix above is covered. Before declaring the generic
client fully acceptance-complete, inject malformed generic method replies
beyond the property operation cases, exercise disconnect/completion/
cancellation races repeatedly under stress, and run the broader endpoint and
sanitizer gates. Do not interpret these remaining robustness gates as missing
API entry-point coverage.

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
