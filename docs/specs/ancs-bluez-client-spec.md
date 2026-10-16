# iPhone Notifications on Linux with ANCS and BlueZ

**Status:** Proposed follow-on to the separately specified first milestone:
the BlueZ inventory and `--discovery` CLI in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). The optional
`dmn-bluez` inventory/discovery API and CLI, including explicit selected-device
connect/disconnect, are implemented with extended lifecycle acceptance work
still outstanding. A GATT client/backend and ANCS protocol client are not
implemented. GATT and ANCS are not prerequisites for shipping the current CLI.
The eventual ANCS delivery is a directly linked Linux application, not a
system daemon. Hardware compatibility with the target iPhone and Linux/BlueZ
combination is a release gate for GATT/ANCS, not for inventory/discovery or
basic device connection control.

## 1. Goal and scope

The longer-term goal is a Dmn-owned C++ API that lets a Linux application
discover BlueZ-known Bluetooth devices, select and connect to a device, use
remote GATT, and optionally run an ANCS session with one user-selected iPhone
over Bluetooth Low Energy. The first independently useful increment is BlueZ
inventory, explicit discovery, selected-device connection control, and the
`dmn-bluez-discovery` CLI; its contract and build/test gates are specified in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). Later
increments add GATT and ANCS. The implementation calls BlueZ's documented
system-D-Bus API through the generic Dmn client and does not depend on a
third-party Bluetooth/GATT wrapper. No custom iPhone app is part of this
design; visibility of any particular notification remains subject to iOS and
the notification provider's behavior.

Version 1 includes a live view of BlueZ-known devices and explicit discovery
controls:

- Enumerate BlueZ adapters and their known `Device1` objects. Present
  connected devices and, during discovery, nearby discovered devices in the
  application inventory with adapter, address, best available name, and
  available paired/connected/services-resolved state.
- Start/stop discovery on a user-selected adapter with optional filters, and
  update discovered-device results as BlueZ reports object/property changes.
  Discovery is explicit; enumeration of already-known devices does not start
  a scan.
- Let the user explicitly connect or disconnect one selected device. Do not
  call `Pair`, register a pairing agent, or auto-connect discovered devices.
  BlueZ may still initiate its normal system pairing/authorization behavior
  during `Device1.Connect`.
- Expose the remote GATT operations required by the Dmn ANCS client through a
  Dmn-owned GATT abstraction backed directly by BlueZ D-Bus.
- For ANCS, run one selected iPhone session, subscribe to Notification Source
  and Data Source, receive add/modify/remove events, and retrieve bounded
  attributes.
- Request a bounded set of attributes: app identifier, title, subtitle,
  message, and date where available. App display names may be resolved and
  cached in memory through Get App Attributes.
- Deliver normalized events through a C++ callback. The example application
  prints them; desktop notification presentation is a separate adapter.
- Reconnect and rediscover after link loss without assuming that ANCS remains
  continuously published by iOS.

V1 does not implement notification actions, an application-facing
general-purpose arbitrary-GATT API beyond the operations needed by ANCS,
concurrent ANCS sessions to multiple iPhones, application-owned pairing
agents, a persistent notification database, a system-wide daemon/client
protocol, or a guarantee that every iOS application or notification category
exposes a title or message. Device enumeration covers
devices currently known to BlueZ; a device not yet discovered or otherwise
known to BlueZ will not appear until a scan or connection procedure finds it.
During a scan, only devices discoverable/reported by the adapter and BlueZ are
visible; this is not a guarantee to enumerate every radio-visible device.

ANCS is a documented Apple accessory service and uses standard BLE GATT
mechanisms, so a Linux GATT client is technically feasible. This conclusion is
conditional: an ordinary Bluetooth pairing or connection is not sufficient.
The Linux host must establish a usable BLE GATT connection to the iPhone, the
iPhone must publish ANCS on that connection, and iOS must authorize access to
the required characteristics. A Bluetooth Classic profile connection does
not provide ANCS. BlueZ reporting the device as `Connected` alone is not proof
that the ANCS GATT session is usable. `Device1.ServicesResolved=true` is a
separate milestone: it means BlueZ has resolved the device's service tree, not
that ANCS is present, authorized, or usable.

Apple's specification says all three ANCS characteristics require
authorization and does not guarantee that ANCS is always present. It also
describes ANCS as a best-effort notification interface, not a synchronization
service: a new session may report existing notifications, but clients must not
assume a complete snapshot, durable identifiers, or replay after disconnect.
No official Linux support or universal compatibility across iOS versions,
device states, adapters, or pairing/authorization configurations is
established. The client must detect missing service/characteristics and
authorization failures at runtime. Real-device feasibility is a release gate,
not a consequence inferred from pairing or a successful generic Bluetooth
connection.

## 2. Architecture and dependency boundaries

```text
Linux application
  ├── Dmn_BluezClient (adapter/device inventory and discovery)
  └── Dmn_AncsClient (optional selected-device session)
        ├── ANCS event, command, response, and cache logic
        └── Dmn_GattClient (small backend-neutral asynchronous interface)
              └── Dmn_BluezGattClient
                    └── libdbus public C API
                          └── system D-Bus -> BlueZ `org.bluez`
                                └── Bluetooth adapters / devices

Application notification callback
  ├── stdout / application UI (v1 example)
  └── optional desktop-notification adapter (future)
```

The layers have these responsibilities:

| Layer | Responsibility | Must not own |
|---|---|---|
| `Dmn_BluezClient` | Implemented Dmn-owned C++ API for adapter/device snapshots, discovery sessions, live BlueZ state changes, and explicit selected-device connect/disconnect. Uses the generic Dmn D-Bus client API. | ANCS parsing, silent auto-pair/auto-connect, or claiming visibility of devices not known/reported by BlueZ |
| `Dmn_GattClient` | Small asynchronous contract for connecting to a selected device, discovering a service and characteristics, subscribing/unsubscribing, and writing characteristic values. Reports connection, discovery, value, and error events. | BlueZ object paths, ANCS UUIDs, notification meaning, or desktop UI |
| `Dmn_BluezGattClient` | Implements the GATT contract through BlueZ D-Bus objects and methods. Resolves BlueZ object paths, tracks `ServicesResolved`, subscribes to characteristic values, and maps BlueZ errors into typed errors. | ANCS parsing, notification policy, or persistence |
| `Dmn_AncsClient` | Discovers ANCS characteristics, subscribes, parses source events and fragmented data responses, schedules serialized control-point requests, maintains bounded in-memory notification state, and emits typed domain events. | BlueZ-specific D-Bus calls, libnotify, application policy, or durable storage |
| Application adapter | Chooses what to display, filter, or retain. | Protocol parsing and GATT session ownership |

Keep ANCS protocol code testable without Bluetooth hardware. Keep BlueZ support
optional and Linux-specific; no BlueZ headers or D-Bus object paths should
leak into the protocol-neutral ANCS API. The implemented inventory/discovery
client and any future GATT backend belong inside the Dmn codebase as Dmn-owned
C++ components. The inventory/discovery client builds on the reusable
`Dmn_DbusClient` API in the optional `dmn-dbus` library; a future GATT backend
will use the same API. That generic Dmn layer uses the public `libdbus` C API
directly for bus connections, method calls, typed values, properties,
ObjectManager access, and signal dispatch. `libdbus` is the low-level D-Bus
transport dependency, not a Bluetooth wrapper. Do not introduce
`GDBus`/`dbus-glib`, `gattlib`, `tinyb`, or another third-party
Bluetooth/GATT client wrapper into this path.

This backend must use the BlueZ system bus and its documented `org.bluez`
D-Bus interfaces; it must not use raw HCI/L2CAP in place of BlueZ and must not
reuse the existing `Dmn_DBusInput`/`Dmn_DBusOutput` broadcast-signal protocol
as a GATT transport. Those endpoints serve a different purpose; the generic
client capability in the same `dmn-dbus` library supplies method/property/
signal mechanics. BlueZ support remains optional and Linux-specific, and must
not make `libdbus` a dependency of the core `dmn` target when disabled. No
BlueZ or D-Bus declarations should leak through protocol-neutral public
headers. The generic D-Bus client owns libdbus connections, messages,
iterators, pending calls, and signal subscriptions. Follow the repository's
libdbus threading and shutdown requirements: initialize libdbus threading
before concurrent use, dispatch signals on a Dmn-managed execution context,
use bounded waits, and surface connection, method, and decode failures as
typed errors. Do not expose raw `DBusConnection` or `DBusMessage` ownership in
the public API. The generic client contract is specified in
[`dbus-client-spec.md`](dbus-client-spec.md); the existing
`Dmn_DbusInput`/`Dmn_DbusOutput` classes remain byte-signal endpoints and are
not the BlueZ client API.

The inventory is separate from discovery and from a selected-device ANCS
session. It must enumerate every BlueZ-known `Device1` beneath every adapter,
including paired, connected, and previously discovered devices, and update
the application on `InterfacesAdded`, `InterfacesRemoved`, and
`PropertiesChanged`. The application can derive a connected-only view from
the `Connected` property and a current-scan view from devices observed during
its discovery session. The normalized device row contains adapter identity,
address, best available display name (`Alias`, falling back to `Name`, then
address), and available RSSI, paired, connected, and services-resolved
properties. Device object paths are transient and must not be persisted as
identity.

Discovery must be an explicit adapter-scoped operation using
`Adapter1.SetDiscoveryFilter`, `StartDiscovery`, and `StopDiscovery`. The
application selects the adapter and may select LE/BR-EDR/auto transport and
service filters supported by BlueZ. The BlueZ daemon shares physical
discovery sessions and merges filters from concurrent clients; Dmn must release
only the discovery session it started and must not assume its filter excludes
results requested by other clients. Starting discovery does not connect or
pair devices. Stopping discovery must not disconnect devices. Inventory
observation alone must not start discovery or alter devices. A user explicitly
selects a device before any connection attempt.

### 2.1 Application-facing API contract

The optional Linux API is Dmn-owned and typed. Exact C++ signatures are frozen
in Phase 1, but the public surface must expose these operations without
requiring application code to construct D-Bus messages:

- Enumerate adapters and read their available state.
- Read an initial snapshot of BlueZ-known devices and receive add/remove and
  property-change updates. Device values use owned strings/values, not a
  transient BlueZ object path as persistent identity.
- Start discovery on a chosen adapter with explicit transport/filter
  configuration; stop only the discovery session this client started.
- Request connect/disconnect for an explicitly selected device, with
  connection state reported separately from GATT service resolution.
- Create/use the selected device's GATT session for service and characteristic
  discovery, notification subscriptions, reads, and writes required by ANCS.
- Start/stop ANCS independently of adapter inventory and scan lifetime; keep
  ANCS's one-device V1 constraint explicit.
- Observe asynchronous adapter, discovery, device, GATT, and ANCS events
  through documented callback/executor semantics and typed errors.

Do not expose arbitrary BlueZ D-Bus method invocation, raw libdbus messages,
or a generic plugin registry as the application interface. Enumeration must
not silently initiate discovery, pairing, or connection. An application may
display every currently connected device by selecting rows with
`Connected == true`, while showing discovered-but-disconnected devices in a
separate or combined view.

V1 is an in-process library integration. Only after the API and hardware
behavior are stable should a future phase add a daemon, a local IPC client, or
a systemd unit. This avoids requiring a privileged/system service merely to
prove the BLE and ANCS layers.

## 3. BlueZ and ANCS protocol contract

### 3.1 Device and service discovery

- The application selects one device by BlueZ device object path or address;
  the library does not silently connect to an arbitrary nearby phone.
- For nearby-device search, the application explicitly starts discovery on a
  selected adapter using `Adapter1.SetDiscoveryFilter` followed by
  `Adapter1.StartDiscovery`. `StopDiscovery` releases only the discovery
  session acquired by this client. Discovery sessions are shared by BlueZ;
  filters can be merged with other clients' filters, so results may not be
  limited to this application's filter. Start/stop discovery never pairs or
  connects a device.
- `Device1` connection state and discovery results are independent. A device
  found by scanning may remain disconnected; a known/paired device may be
  connected without appearing in the current scan. The API reports both
  states and lets application policy decide which devices to show.
- V1 leaves pairing to the system's normal BlueZ/user flow and does not
  implement an interactive pairing agent. Pairing state is not proof of ANCS
  authorization. Pairing, authorization, and encryption requirements are not
  assumed to be identical across iOS releases and must be surfaced as runtime
  states and errors.
- Establish a BLE GATT connection using the Dmn BlueZ backend's direct
  `libdbus` calls to BlueZ; enumerate the remote GATT objects through
  BlueZ's `org.freedesktop.DBus.ObjectManager` only after observing
  `Device1.ServicesResolved=true`. `org.bluez.Device1.Connect` is a generic
  device/profile operation, not an ANCS-specific connect request; BlueZ may
  connect auto-connectable profiles, and other transports/profiles may need
  their own documented connection operation. Its success and the `Connected`
  property alone are not GATT readiness. BlueZ may report
  `Connected=true` while `ServicesResolved=false`; this is a valid
  intermediate state. The client must await the property through BlueZ's
  property-change/snapshot mechanism with a finite deadline, and must not
  attempt to write the read-only property or assume a successful Connect call
  means service discovery has completed.
- Treat service resolution, service-tree inspection, and ANCS session readiness
  as distinct states:

  | State | Evidence | Does not imply |
  |---|---|---|
  | Link connected | `Device1.Connected=true` | GATT bearer, resolved services, ANCS |
  | Services resolved | `Device1.ServicesResolved=true` | ANCS service presence, characteristic completeness, authorization |
  | ANCS service found | Current remote `GattService1` object has ANCS UUID and belongs to selected Device | Required characteristics or usable permissions |
  | ANCS characteristics found | Current child `GattCharacteristic1` objects match UUIDs and have compatible flags | iOS authorization or successful I/O |
  | ANCS session ready | Required subscriptions/write behavior succeed under current authorization | Durable notification history or a future-session UID |

  Enumerate current `GattService1` and `GattCharacteristic1` objects from
  ObjectManager, filter by the selected device's `Device` property and service
  parent relationship, and match UUIDs rather than relying on object-path
  text, advertised data, `Device1.UUIDs`, or cached paths. BlueZ's GATT D-Bus
  API presents remote services and characteristics as objects; characteristic
  `Flags` and runtime operation results both matter. Re-query after disconnect,
  BlueZ owner restart, or service change; do not persist GATT paths/handles as
  durable identity.
- For the ANCS notification-details client, require Notification Source,
  Control Point, and Data Source. Apple describes Notification Source as
  mandatory and Control Point/Data Source as optional in the service
  definition, but the latter two are necessary for requesting and receiving
  attributes. Check compatible notify/write flags before starting a session;
  still handle authorization and operation errors at runtime.
- ANCS session startup must register property-change handling and subscribe to
  Data Source before enabling Notification Source. Successful
  `StartNotify` on Notification Source is the session boundary at which ANCS
  events may begin; a resolved tree or successful connection does not start
  the ANCS protocol session. Session-scoped notification UIDs and in-flight
  fragmented Data Source responses must be discarded on session teardown.
- `Device1.Connect` does not guarantee selection of the LE bearer for a
  dual-mode device. ANCS requires BLE GATT; establish and record the actual
  bearer used in hardware qualification. Do not claim ANCS compatibility from
  `Connected=true` or `ServicesResolved=true` alone. Any explicit bearer
  preference/control is a separate future API requirement and must be opt-in.
- Match service and characteristic UUIDs at runtime; do not persist GATT
  object paths across disconnects or service changes.
- The ANCS primary service UUID is
  `7905F431-B5CE-4E99-A40F-4B1E122D00D0`.
- The service may be missing or partially published. Notification Source is
  the required baseline characteristic; Control Point and Data Source are
  optional in the Apple specification but both are required for this
  receive-details client. Report an unsupported/incomplete service instead of
  dereferencing missing characteristics.

### 3.2 Characteristics and operations

| Characteristic | UUID | V1 operation |
|---|---|---|
| Notification Source | `9FBF120D-6301-42D9-8C58-25E699A21DBD` | Subscribe with BlueZ `GattCharacteristic1.StartNotify`; consume `Value` updates from `PropertiesChanged`. |
| Control Point | `69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9` | Send Get Notification Attributes and Get App Attributes through `WriteValue`, requesting a write with response (`type = "request"`). |
| Data Source | `22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB` | Subscribe with `StartNotify`; reassemble the response fragments emitted through `Value` updates. |

Register BlueZ property-change handling and the Data Source subscription
before enabling Notification Source. ANCS events may arrive as soon as the
Notification Source subscription is enabled. Subscribe to Data Source before
Notification Source so an immediate event can be serviced without missing its
attribute response.

Treat service-changed indications, characteristic disappearance, disconnect,
BlueZ restart, and `NotAuthorized`/`NotPermitted` failures as recoverable
session state where appropriate. On disconnect, invalidate all characteristic
object paths and outstanding requests, unsubscribe when possible, reconnect
according to bounded retry policy, and rediscover the entire service.

### 3.3 Notification Source event parsing

A Notification Source value is exactly eight bytes:

| Offset | Width | Field | Encoding |
|---:|---:|---|---|
| 0 | 1 | Event ID | `0` added, `1` modified, `2` removed |
| 1 | 1 | Event flags | Bit field defined by ANCS |
| 2 | 1 | Category ID | ANCS category; unknown future values are retained as unknown |
| 3 | 1 | Category count | Current count for that category |
| 4 | 4 | Notification UID | Unsigned little-endian identifier |

Reject malformed lengths and invalid encodings without reading beyond the
received value. Unknown event/category/flag values must not crash the client;
unknown event IDs are reported as unsupported input and ignored. The UID is
valid only as an ANCS request handle, not as a durable identifier.

- **Added:** create/update an in-memory pending record and enqueue a
  Get Notification Attributes request.
- **Modified:** enqueue an attribute refresh for the UID.
- **Removed:** remove the active record and emit a removal event. Do not issue
  an attribute request for a removed UID.
- If a response arrives after its UID was removed, discard it rather than
  resurrecting the record.

### 3.4 Control Point requests and Data Source responses

V1 supports these ANCS commands:

| Command ID | Command | V1 behavior |
|---:|---|---|
| `0` | Get Notification Attributes | Supported for added/modified notifications. Request only selected attributes and specify explicit maximum lengths for variable-length title, subtitle, and message values. |
| `1` | Get App Attributes | Supported optionally to resolve an app identifier to a display name; cache the result in memory only. |
| `2` | Perform Notification Action | Not sent in V1. |

V1 notification attribute IDs are App Identifier (`0`), Title (`1`),
Subtitle (`2`), Message (`3`), Message Size (`4`), and Date (`5`). The
app-attribute ID for Display Name is `0`. The monitor may request the app
identifier and user-selected title/subtitle/message/date fields; it must not
assume every iOS app provides every field. Title, Subtitle, and Message
requests include a 16-bit maximum byte length. Avoid requesting fields the
application does not intend to expose.

Numeric multi-byte values use the ANCS-prescribed little-endian encoding.
A Get Notification Attributes request is command ID, 32-bit Notification UID,
and ordered attribute IDs with any required maximum-length parameters. A Get
App Attributes request is command ID, null-terminated app identifier, then
attribute IDs. Notification-attribute responses contain command ID, UID, then
ordered attribute-ID/16-bit-length/value tuples. App-attribute responses
contain command ID, null-terminated app identifier, then the requested
attribute tuples. Attribute values are byte strings, not null-terminated;
decode textual values as UTF-8 per ANCS and validate them before application
presentation.

If a requested attribute is empty or unavailable, ANCS returns its tuple with
zero length. This tuple still counts toward response completion. The parser
must validate response command/UID/app identifier and expected tuple order,
use lengths from the wire, enforce configured limits, and tolerate unknown
attribute IDs by skipping their bounded values.

ANCS may split a response across multiple Data Source notifications according
to the negotiated GATT MTU. The client reassembles a response incrementally;
fragments may split tuple headers or values. The expected attribute list and
lengths determine completion. Do not assume one Data Source notification is
one complete response.

Allow at most one outstanding Control Point attribute request in V1. Queue
subsequent requests and send the next only after the active Data Source
response is complete or times out. This serialization is a conservative
client policy because ANCS does not define an application-level request ID
for correlating overlapping responses. It is not an Apple requirement.
Perform Notification Action is excluded; if later added, it must not wait for
a Data Source response because ANCS defines none for that command.

ANCS Control Point errors include Unknown Command (`0xA0`), Invalid Command
(`0xA1`), Invalid Parameter (`0xA2`), and Action Failed (`0xA3`). A failed
Control Point command does not produce a corresponding Data Source response.
Surface the error immediately when BlueZ reports it; otherwise expire the
request at its bounded deadline so a missing response cannot block the queue.

All input lengths, decoded strings, queued requests, cached active records,
and reassembly buffers must have explicit finite limits. Malformed, oversized,
timed-out, or uncorrelatable responses produce a typed protocol error and
release the active request without corrupting subsequent requests.

### 3.5 Public behavior and callback contract

Proposed public domain event:

```text
NotificationEvent {
  kind: Added | Updated | Removed | AttributesUnavailable
  uid: uint32
  category: known value or Unknown(raw value)
  flags: raw ANCS flags
  app_identifier: optional string
  app_display_name: optional string
  title: optional string
  subtitle: optional string
  message: optional string
  date: optional string
}
```

The C++ API should expose explicit `start(device selector)`, `stop()`,
current connection/session status, and a callback for state changes and
notification events. Freeze exact signatures, ownership, and error types in
Phase 1 of the implementation plan before implementation. Callbacks run
serially on a documented libDmn-owned execution context, never directly on
the BlueZ D-Bus dispatch path. Callbacks must return promptly; applications
that perform blocking work must hand it to their own executor.
Stop/destruction must stop new events and safely drain/close subscriptions
and pending work.

ANCS does not provide a complete notification-history or synchronization
contract. The API reports events observed in the current ANCS session; it must
not promise a full current-state snapshot, delivery of every iOS notification,
or replay of events missed during a disconnect. A best-effort `PreExisting`
event at session start is an ordinary added notification event with its flag
preserved, not proof that all existing notifications were enumerated.

The default policy is no durable storage and no notification-body logging.
Examples may print content only when explicitly enabled by the user. Callers
own any retention, redaction, filtering, or desktop-notification decisions.
Never promise fields ANCS or the app does not provide; fields are optional.

## 3.6 Design review: simplicity, extensibility, and future change

This design is intentionally small and explicit. It separates protocol logic,
transport details, and application policy so future additions do not force
large rewrites of the core model.

### 3.6.1 Core design constraints

1. The protocol-neutral ANCS layer owns only ANCS state and byte parsing.
   BlueZ, D-Bus, and UI policy remain outside it.
2. Service discovery and session lifecycle are explicit state transitions, not
   hidden side effects in the callback path.
3. One outstanding control-point attribute request is the v1 policy. The
   scheduler is the only code path that may reorder or suppress requests;
   protocol logic does not duplicate queue policy.
4. A notification is considered active only while it remains in the in-memory
   cache keyed by UID; late responses for removed UIDs are dropped rather than
   resurrecting stale state.
5. Unknown or unsupported values are represented as typed protocol states or
   optional values, never as crashes or unbounded memory growth.

### 3.6.2 Extension points that remain stable

- GATT backend: replace `Dmn_BluezGattClient` with another backend without
  changing ANCS parsing or the notification event model.
- ANCS command surface: add new command IDs behind the same request builder and
  scheduler, rather than scattering control-point special cases through the
  parser.
- Attribute vocabulary: extend the attribute ID enum and parser table without
  altering the normalized event contract.
- Notification sink: keep app-facing callbacks and optional desktop adapters
  separate from transport and parsing so a future `libnotify` integration does
  not change protocol internals.
- Session policy: reconnect, retry, and timeout behavior remain layered in the
  session state machine and are not mixed into the parser.

### 3.6.3 Change-pressure boundaries

The design avoids the two common failure modes for future growth:

- hidden coupling to `BlueZ` object paths in the ANCS logic; and
- implicit state spread across parser, scheduler, callback, and backend layers.

Future work remains simple if it follows these rules:

- add a new GATT backend only by implementing the backend-neutral contract;
- add a new control-point operation by extending the enumerated command set and
  request scheduling policy;
- add a new notification attribute by extending the parser table and output
  normalization, not by hard-coding the message body parser;
- add a manager or multi-device client only as a separate orchestration layer,
  not by embedding device selection into the per-device protocol object.

### 3.6.4 TDD contract and acceptance matrix

The implementation for this feature must be driven by hardware-independent
acceptance tests. The following matrix defines the minimum contract before any
real-device compatibility claim is accepted. The current
`dmn-test-ancs-parser` target contains skipped placeholders only; it is a test
scaffold, not evidence that any behavior has been implemented or validated.
The BlueZ inventory integration contract is exercised separately by
`dmn-test-bluez-device-list` against a private test system bus with a fake
BlueZ object tree through the actual Dmn/libdbus implementation; it does not
require a Bluetooth adapter or phone.

| Test name | Behavior under test | Required result |
|---|---|---|
| `DmnAncsParserRejectsMalformedNotificationSource` | Payloads other than exactly 8 bytes and reserved event IDs | Reject/ignore malformed input without crash or memory growth |
| `DmnAncsParserAcceptsKnownNotificationSourceValues` | valid added/modified/removed MEIDs and flags | emits the correct normalized event fields |
| `DmnAncsParserReassemblesFragmentedDataSourceResponse` | split tuples across multiple notifications | final attribute map is correct and ordered |
| `DmnAncsParserDropsStaleRemovedUid` | removed UID response arrives late | stale response ignored and cache not resurrected |
| `DmnAncsRequestSchedulerSerializesAttributeRequests` | two requests queued while one is active | only one request is active at a time; FIFO fairness preserved |
| `DmnAncsRequestSchedulerTimesOutStaleRequest` | Data Source never completes | request expires and queue resumes |
| `DmnAncsSessionReconnectsAfterDisconnect` | disconnect, service loss, and BlueZ restart | cached state invalidated and rediscovery reinitializes safely |
| `DmnBluezGattClientMapsErrorsToTypedFailures` | authorization, missing characteristics, property-update failures | typed library error, no unhandled D-Bus exception |
| `DmnAncsClientHandlesUnknownAttributeId` | response contains unsupported attribute IDs | unknown tuples skipped; known tuples still parsed |
| `DmnBluezDeviceListIncludesAllConnectedDevices` | several adapters with connected and disconnected `Device1` objects | every connected device from every adapter appears exactly once; disconnected devices do not appear |
| `DmnBluezDeviceInventoryIncludesKnownDisconnectedDevices` | known paired/disconnected and previously discovered devices | known objects are available in the full inventory even when not connected; the connected-only view filters by `Connected` |
| `DmnBluezDeviceListTracksConnectionAndObjectChanges` | `Connected` changes and `InterfacesAdded`/`InterfacesRemoved` signals | emitted snapshots add, update, and remove the right device without stale entries |
| `DmnBluezDeviceListUsesStableDisplayFallbacks` | missing `Alias` and/or `Name` properties | display name falls back to `Name`, then address; missing optional properties do not fail enumeration |
| `DmnBluezDeviceListDoesNotChangeDeviceState` | enumerate and watch inventory | no Pair, Connect, Disconnect, or discovery call is issued by listing |
| `DmnBluezDiscoveryStartsAndStopsOwnedSession` | explicit Adapter1 discovery lifecycle | only this client's discovery session is released; discovery never pairs/connects/disconnects |
| `DmnBluezDiscoveryAppliesFilterAndTracksResults` | discovery filters and live results | requested filter is applied; other clients' merged results are not treated as excluded |

The generic client method/property/signal value codec and error/lifecycle
contract are tested separately as specified in
[`dbus-client-spec.md`](dbus-client-spec.md). The tests above are intentionally
deterministic and hardware-independent.
They must contain executable assertions (not `GTEST_SKIP`) before they count
as passing acceptance coverage. The BlueZ backend tests may use a fake
BlueZ object tree on a private test bus, but must exercise the Dmn backend
through the generic `Dmn_DbusClient` API rather than substituting a
third-party Bluetooth wrapper or only a mock client. The device-list
integration test must exercise the actual Linux BlueZ D-Bus adapter against a
private test bus and fake
ObjectManager/Adapter1/Device1 objects; pure data-model tests alone do not meet
that integration requirement. Passing fake-bus tests does not prove
iPhone interoperability or host BlueZ permissions. Real-device qualification
remains a separate gate and compatibility must be scoped to the tested OS,
BlueZ, adapter, iPhone, and iOS combination.

## 4. Session lifecycle

```text
Stopped
  -> WaitingForBlueZ
  -> Connecting
  -> ResolvingServices
  -> SubscribingDataSource
  -> SubscribingNotificationSource
  -> Ready
  -> Reconnecting (disconnect or BlueZ loss)
  -> ResolvingServices ...
  -> Stopped

Any non-recoverable configuration/protocol error -> Failed
```

Each transition is observable through status/callbacks. Recovery uses bounded
backoff and cancellation-aware waits. A changed or absent ANCS service is a
reported capability state, not an infinite retry loop. Stopping cancels the
current attempt and prevents reconnect.

An ANCS session starts when Notification Source notifications are enabled and
ends when they are disabled or the device disconnects. Notification UIDs,
app identifiers, and all received data are session-scoped. Clear notification
and app-attribute caches at every session end; do not persist or reuse them
after reconnect. During one session, cache immutable app display-name
attributes by app identifier to avoid repeated requests.

## 5. Acceptance criteria

### Hardware-independent

- Parse valid Notification Source events with exact little-endian UID handling.
- Reject truncated/oversized events and malformed attribute tuples safely.
- Reassemble Data Source fragments split at every byte boundary, including
  splits inside headers, length fields, and attribute values.
- Handle unknown attribute/category values, empty optional attributes,
  duplicate events, modification, removal while a request is queued, removal
  during a response, and request timeout.
- Prove that only one attribute request is in flight and that the next request
  starts only after completion or timeout.
- Verify all resource limits, callback execution context/order, stop behavior,
  and error recovery using a fake `Dmn_GattClient`.

### BlueZ and real-device

- On Linux, exercise known-device enumeration and live updates against a
  private fake bus containing multiple adapters and connected/disconnected
  devices. Verify the complete known-object inventory, the connected-only
  filtered view, and that connection/object changes are reflected.
- Exercise explicit discovery start/filter/stop through the fake bus. Verify
  only the caller's discovery session is released and listing/scanning never
  initiates pairing or connecting.
- Manually verify the example displays actual connected devices, starts a
  nearby scan only on user request, updates discovered results, and does not
  auto-pair/connect. This opt-in smoke test must not be a default CI dependency.
- Exercise the BlueZ adapter against a fake GATT object tree on a private
  test bus:
  discovery, missing service/characteristic, `StartNotify`, `PropertiesChanged`,
  `WriteValue`, disconnect, authorization errors, and BlueZ restart.
  These tests use the public `dmn-bluez` API over the generic Dmn
  direct-libdbus client and a private test bus; they must not connect to the
  host system bus or rely on a third-party Bluetooth wrapper.
- On each supported Linux/BlueZ and physical iPhone/iOS combination, verify
  pairing/setup, ANCS service visibility, both subscriptions, added/modified/
  removed events, attribute reads, disconnect/reconnect, and the actual
  availability of app display name/title/message.
- Record the tested OS, BlueZ, adapter, iPhone model, and iOS version. Until
  this matrix is populated, compatibility claims remain provisional.

## 6. Out of scope and future extension points

- Notification actions, including positive/negative action labels.
- Application-facing arbitrary BLE/GATT access beyond ANCS's required
  operations and multiple concurrent ANCS sessions.
- Persistent history, cloud forwarding, message synchronization, or automatic
  content logging.
- libnotify, tray/UI integration, and system-wide policy; implement these as
  application adapters over normalized events. The V1 example's connected
  device inventory is in scope, but a polished desktop device manager is not.
- A Dmn-owned long-running daemon, systemd packaging, or a local IPC protocol
  for applications to call that daemon. Consider these only after the direct
  library client works reliably; if added, keep the ANCS/GATT layers inside
  the daemon and expose a separate, versioned local application protocol.
- Alternate GATT backends in V1. Keep ANCS protocol code dependent on the small
  `Dmn_GattClient` contract so a future Dmn-owned backend can be added without
  changing parsing or notification-domain semantics. The V1 BlueZ backend
  itself uses libdbus directly rather than a third-party Bluetooth wrapper.

## 7. Research references

Primary sources consulted:

- Apple, [Apple Notification Center Service Specification](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Specification/Specification.html):
  service characteristics, Notification Source, Control Point, Data Source,
  attribute commands, service availability, and notification actions.
- Apple, [ANCS introduction and encoding rules](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Introduction/Introduction.html)
  and [ANCS appendix](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Appendix/Appendix.html):
  endianness, event/command/attribute identifiers.
- BlueZ, [GATT D-Bus API](https://bluez.readthedocs.io/en/latest/gatt-api/):
  remote service/characteristic objects, `StartNotify`, `StopNotify`,
  `WriteValue`, and characteristic `Value` changes.
- BlueZ, upstream
  [`org.bluez.Device1`](https://github.com/bluez/bluez/blob/master/doc/org.bluez.Device.rst),
  [`GattService1`](https://github.com/bluez/bluez/blob/master/doc/org.bluez.GattService.rst),
  and
  [`GattCharacteristic1`](https://github.com/bluez/bluez/blob/master/doc/org.bluez.GattCharacteristic.rst)
  documentation: connection/bearer behavior, read-only
  `ServicesResolved`, remote GATT object properties, flags, and characteristic
  operations. Confirm details against the deployed BlueZ version.
- BlueZ, [Device D-Bus API](https://bluez.readthedocs.io/en/latest/device-api/)
  and [Adapter D-Bus API](https://bluez.readthedocs.io/en/latest/adapter-api/):
  connection, pairing, service resolution, discovery sessions, and errors.
- Apple, [Core Bluetooth central-role overview](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/PerformingCommonCentralRoleTasks/PerformingCommonCentralRoleTasks.html):
  central devices discover peripherals, connect, discover services, access
  characteristics, and subscribe to updates. This describes Apple's BLE
  central API, not Linux support for ANCS.
- Third-party Bluetooth wrappers such as gattlib and tinyb are intentionally
  not selected. V1 uses the BlueZ D-Bus contract through libdbus directly in
  the Dmn-owned backend; these alternatives are not build or runtime
  dependencies.

The ANCS document is archived and does not provide a current iOS compatibility
matrix or a universal pairing/encryption recipe. Validate current iPhone
behavior empirically and treat authorization/service availability as runtime
conditions.
