# iPhone Notifications on Linux with ANCS and BlueZ

**Status:** Phase 0 inventory/discovery and Phase 1A pairing plus Phase 1B
backend-neutral/BlueZ GATT are implemented and have passed their
hardware-independent private-bus gates. Phase 1C real-device qualification
remains open; no iPhone ANCS compatibility is claimed. The ANCS library and
monitor contracts below are specified, but ANCS implementation has not
started. This is a follow-on to the separately
specified first milestone: the BlueZ inventory and `--discovery` CLI in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). The optional
`dmn-bluez` inventory/discovery API and CLI, including explicit selected-device
connect/disconnect, are implemented and Phase 0 qualified. An explicit
`Device1.Pair` API and `--pair` CLI action are implemented and have focused
private-bus coverage. The backend-neutral GATT contract and an initial BlueZ
GATT backend are implemented and the hardware-independent BlueZ/D-Bus test
gate passes; real-device qualification remains. The ANCS protocol/session
client and notification-monitor CLI are the next implementation increments;
neither exists yet. GATT and ANCS are not prerequisites for shipping the
existing inventory CLI. The first ANCS frontend is a directly linked
terminal application, with a reusable C++ client for a future GTK frontend;
this is not a system daemon. Hardware compatibility with the target iPhone
and Linux/BlueZ combination is a release gate for ANCS, not for inventory or
basic device connection control.

## 1. Goal and scope

The goal is a Dmn-owned C++ API and terminal client that lets a user select an
iPhone by its BlueZ device name, establish the required pairing/connection
when needed, start an ANCS session, and display notification details observed
while the program runs. The first independently useful increment is BlueZ
inventory, explicit discovery, selected-device connection control, and the
`dmn-bluez-discovery` CLI; its contract and build/test gates are specified in
[`bluez-discovery-cli-spec.md`](bluez-discovery-cli-spec.md). The GATT
transport is implemented; the ANCS protocol client and terminal monitor are
specified below and remain to be implemented. The implementation calls BlueZ's documented
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
- Let the user explicitly pair, connect, or disconnect one selected device.
  Pairing uses `Device1.Pair` and BlueZ's configured default agent; Dmn does
  not register its own pairing agent, request trust, or auto-pair/connect
  discovered devices. `Device1.Connect` remains a distinct explicit action
  and may still trigger BlueZ's normal system pairing/authorization behavior.
- Expose the remote GATT operations required by the Dmn ANCS client through a
  Dmn-owned GATT abstraction backed directly by BlueZ D-Bus.
- For ANCS, run one selected iPhone session, subscribe to Notification Source
  and Data Source, receive add/modify/remove events, and retrieve bounded
  attributes.
- Request a bounded set of attributes: app identifier, title, subtitle,
  message, and date where available. App display-name lookup is deferred.
- Deliver normalized events through a C++ callback. The monitor CLI formats
  them; future desktop notification presentation is a separate adapter.
- Provide `dmn-bluetooth monitor DEVICE` as the first notification client. The
  explicit monitor command may discover an unknown target, pair it if needed,
  connect it if needed, and then start ANCS. It displays received notification
  attributes on stdout until Ctrl+C and leaves the BlueZ pairing and connection
  intact on exit.
- On a link/service/backend invalidation, report the loss, stop subscriptions,
  discard session-scoped state, and exit nonzero. Automatic reconnect is
  deferred until real-device behavior and retry policy are qualified.

V1 does not implement notification actions, an application-facing
general-purpose arbitrary-GATT API beyond the operations needed by ANCS,
concurrent ANCS sessions to multiple iPhones, an application-owned pairing
agent, a persistent notification database, a system-wide daemon/client
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
        └── Dmn_GattClient (small backend-neutral bounded-operation interface)
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
| `Dmn_BluezClient` | Implemented Dmn-owned C++ API for adapter/device snapshots, discovery sessions, live BlueZ state changes, and explicit selected-device pair/connect/disconnect. Uses the generic Dmn D-Bus client API. | ANCS parsing, silent auto-pair/auto-connect, or claiming visibility of devices not known/reported by BlueZ |
| `Dmn_GattClient` | Implemented backend-neutral contract for waiting for service resolution, discovering a selected device's services/characteristics, subscribing/unsubscribing, and acknowledged writes. Blocking operations are synchronous and bounded; notification events are asynchronous. | BlueZ object paths, ANCS UUIDs, notification meaning, or desktop UI |
| `Dmn_BluezGattClient` | Initial implementation through BlueZ D-Bus objects and methods. Resolves BlueZ object paths, tracks `ServicesResolved`, subscribes to characteristic values, reports disconnect/object/owner invalidation, and maps BlueZ errors into typed errors. | ANCS parsing, notification policy, or persistence |
| `Dmn_AncsClient` | Discovers ANCS characteristics, subscribes, parses source events and fragmented data responses, schedules serialized control-point requests, maintains bounded in-memory notification state, and emits typed domain events. | BlueZ-specific D-Bus calls, libnotify, application policy, or durable storage |
| Application adapter | Chooses what to display, filter, or retain. | Protocol parsing and GATT session ownership |

Keep ANCS protocol code testable without Bluetooth hardware. Keep BlueZ support
optional and Linux-specific; no BlueZ headers or D-Bus object paths should
leak into the protocol-neutral ANCS API. The implemented inventory/discovery
client and GATT backend belong inside the Dmn codebase as Dmn-owned C++
components. The inventory/discovery client and current GATT backend build on
the reusable `Dmn_DbusClient` API in the optional `dmn-dbus` library. That
generic Dmn layer uses the public `libdbus` C API
directly for bus connections, method calls, typed values, properties,
ObjectManager access, and signal dispatch. `libdbus` is the low-level D-Bus
transport dependency, not a Bluetooth wrapper. Do not introduce
`GDBus`/`dbus-glib`, `gattlib`, `tinyb`, or another third-party
Bluetooth/GATT client wrapper into this path.

The current GATT operations are synchronous with finite caller-supplied
deadlines. Applications must keep them off UI threads. GATT notification
callbacks run serially on Dmn's D-Bus callback worker; a GTK adapter must
marshal callback data onto its main context before touching widgets. The
backend-neutral API owns copied values and opaque identifiers, not BlueZ or
D-Bus types.

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

The optional Linux API is Dmn-owned and typed. Phase 1 has frozen and
implemented the BlueZ inventory, selected-device, and backend-neutral GATT
contracts. The ANCS library signatures must follow this specification before
their implementation begins. Application code must not need to construct
D-Bus messages:

- Enumerate adapters and read their available state.
- Read an initial snapshot of BlueZ-known devices and receive add/remove and
  property-change updates. Device values use owned strings/values, not a
  transient BlueZ object path as persistent identity.
- Start discovery on a chosen adapter with explicit transport/filter
  configuration; stop only the discovery session this client started.
- Request connect/disconnect for an explicitly selected device, with
  connection state reported separately from GATT service resolution.
- Create/use the selected device's GATT session for service and characteristic
  discovery, notification subscriptions, and acknowledged writes required by
  ANCS. ANCS attribute responses arrive through Data Source notifications,
  not a generic GATT characteristic-read operation.
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
- Pairing is a separate, explicit user action. `Dmn_BluezClient::pairDevice`
  and the CLI `--pair DEVICE` call `Device1.Pair` only for a selected device
  whose current `Paired` state is not already true. The operation uses BlueZ's
  default agent and returns BlueZ's typed error if no default agent is
  available or user interaction is cancelled/rejected. Dmn does not implement
  a terminal pairing agent, request `Trusted=true`, or pair as a side effect
  of listing, scanning, or connecting. A successful Pair operation is
  confirmed by an observed `Paired=true` state; it may itself connect the
  device and resolve GATT services as part of BlueZ's documented behavior.
- `Device1` connection state and discovery results are independent. A device
  found by scanning may remain disconnected; a known/paired device may be
  connected without appearing in the current scan. The API reports both
  states and lets application policy decide which devices to show.
- V1 uses the system's normal BlueZ default-agent/user flow and does not
  implement an application-owned pairing agent. Pairing state is not proof of ANCS
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
| Control Point | `69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9` | The initial monitor sends Get Notification Attributes through `WriteValue` with response (`type = "request"`); Get App Attributes is deferred. |
| Data Source | `22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB` | Subscribe with `StartNotify`; reassemble the response fragments emitted through `Value` updates. |

Register BlueZ property-change handling and the Data Source subscription
before enabling Notification Source. ANCS events may arrive as soon as the
Notification Source subscription is enabled. Subscribe to Data Source before
Notification Source so an immediate event can be serviced without missing its
attribute response.

Treat service-changed indications, characteristic disappearance, disconnect,
BlueZ restart, and `NotAuthorized`/`NotPermitted` failures as explicit typed
session state. On disconnect, invalidate all characteristic object paths and
outstanding requests and unsubscribe when possible. The initial monitor ends
the session on invalidation; retry/reconnect policy is deferred.

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
  Get Notification Attributes request unless that UID has already returned a
  request-scoped ANCS command error in this session.
- **Modified:** enqueue an attribute refresh for the UID unless it is marked
  unavailable for the rest of the session.
- **Removed:** remove the active record and emit a removal event. Do not issue
  an attribute request for a removed UID.
- If a response arrives after its UID was removed, discard it rather than
  resurrecting the record.

### 3.4 Control Point requests and Data Source responses

V1 supports these ANCS commands:

| Command ID | Command | V1 behavior |
|---:|---|---|
| `0` | Get Notification Attributes | Supported for added/modified notifications. Request only selected attributes and specify explicit maximum lengths for variable-length title, subtitle, and message values. |
| `1` | Get App Attributes | Reserved for a later increment. The initial monitor does not request this command or resolve app display names. |
| `2` | Perform Notification Action | Not sent in V1. |

Notification attribute IDs are App Identifier (`0`), Title (`1`), Subtitle
(`2`), Message (`3`), Message Size (`4`), and Date (`5`). The initial monitor
requests App Identifier, Title, Subtitle, Message, and Date; it does not
request Message Size or Get App Attributes. The app-attribute ID for Display
Name (`0`) is reserved for a later increment. The monitor must not assume
every iOS app provides every requested field. Title, Subtitle, and Message
requests include 16-bit maximum byte lengths of 256, 256, and 4096 bytes,
respectively. Invoking the monitor explicitly opts into displaying these
available fields.

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
response is complete or a request-scoped ANCS error confirms there will be no
response. This serialization is a conservative
client policy because ANCS does not define an application-level request ID
for correlating overlapping responses. It is not an Apple requirement.
Perform Notification Action is excluded; if later added, it must not wait for
a Data Source response because ANCS defines none for that command.

ANCS Control Point errors include Unknown Command (`0xA0`), Invalid Command
(`0xA1`), Invalid Parameter (`0xA2`), and Action Failed (`0xA3`). A failed
Control Point command does not produce a corresponding Data Source response.
Map a request-scoped ANCS command error to `AttributesUnavailable`, mark that
UID unavailable for the rest of this session, and advance the queue. A
BlueZ/GATT authorization, transport, or invalidation error is a fatal typed
session error; do not keep issuing commands against a failed transport.
If no Data Source response arrives before the bounded deadline, fail the
session with a fatal `Timeout` error. Even if no response fragment was
observed, a late response could not be safely distinguished from the next
request because ANCS provides no request identifier beyond the notification
UID. Do not send another request after this timeout.

All input lengths, decoded strings, queued requests, cached active records,
and reassembly buffers must have explicit finite limits. Malformed, oversized,
or uncorrelatable responses produce a typed protocol error and fail the
session without corrupting later sessions.

### 3.5 Public behavior and callback contract

The exact public notification value and event discriminators are:

```text
enum class Dmn_AncsNotificationKind {
  Added, Updated, Removed, AttributesUnavailable
};

struct Dmn_AncsNotification {
  Dmn_AncsNotificationKind kind;
  std::uint32_t uid;
  std::uint8_t category;        // retain unknown raw values
  std::uint8_t category_count;
  std::uint8_t flags;
  std::optional<std::string> app_identifier;
  std::optional<std::string> title;
  std::optional<std::string> subtitle;
  std::optional<std::string> message;
  std::optional<std::string> date;
};

struct Dmn_AncsConfig {
  std::chrono::milliseconds service_resolution_timeout{30000};
  std::chrono::milliseconds gatt_operation_timeout{10000};
  std::chrono::milliseconds attribute_response_timeout{10000};
};

enum class Dmn_AncsState {
  Stopped, Starting, ResolvingServices, DiscoveringANCS,
  SubscribingDataSource, SubscribingNotificationSource,
  Ready, Stopping, Failed
};

enum class Dmn_AncsErrorKind {
  InvalidArgument, NotReady, Timeout, Unsupported, Authorization, Remote,
  Protocol, ResourceLimit, Backend, Cleanup
};

struct Dmn_AncsError {
  Dmn_AncsErrorKind kind;
  std::string message;
  std::string remote_name;      // empty when not reported by the backend
  bool fatal;
};

struct Dmn_AncsEvent {
  enum class Kind {
    StateChanged, NotificationAdded, NotificationUpdated,
    NotificationRemoved, AttributesUnavailable, Error
  };
  Kind kind;
  std::optional<Dmn_AncsState> state;
  std::optional<Dmn_AncsNotification> notification;
  std::optional<Dmn_AncsError> error;
};
```

The public callback type is
`using EventHandler = std::function<void(const Dmn_AncsEvent &)>`. The client
operations are exactly:

```text
explicit Dmn_AncsClient(Dmn_GattClient &gatt);
Dmn_AncsResult setEventHandler(EventHandler handler);
Dmn_AncsResult start(const std::string &deviceId,
                     const Dmn_AncsConfig &config = {});
Dmn_AncsResult stop();
Dmn_AncsState state() const;
```

`Dmn_AncsResult` is `std::expected<void, Dmn_AncsError>`. The client is
non-copyable and non-movable. These signatures describe the public C++ API;
the CLI command parser and application orchestration are separate interfaces.
`Dmn_GattClient` is borrowed and must outlive the ANCS client;
device/service/characteristic identifiers are opaque GATT identifiers and
valid only for the current connection. `start` does not pair or connect a
device: its caller performs device selection/link setup, and `start` waits for
service resolution, discovers the ANCS objects, and subscribes. `start` is a
bounded synchronous operation returning `Dmn_AncsResult` (an
`std::expected<void, Dmn_AncsError>`). `stop` is idempotent and returns a
typed cleanup result; destruction is best-effort and non-throwing. Copy and
move are disabled.

`Dmn_AncsConfig` contains finite positive
`service_resolution_timeout` (default 30 seconds),
`gatt_operation_timeout` (default 10 seconds), and
`attribute_response_timeout` (default 10 seconds). Values above 600 seconds
or nonpositive values are invalid. `Dmn_AncsState` and `Dmn_AncsErrorKind`
enumerate all state/error values shown above. A `Dmn_AncsEvent` contains
`state` for StateChanged, `notification` for notification events, and `error`
for Error. An AttributesUnavailable event carries both notification metadata
and the request-scoped error when available. An Error event may also carry
the current state. Unknown Notification Source event IDs emit a nonfatal
Unsupported Error event and are ignored. Invalid UTF-8 emits
AttributesUnavailable with a nonfatal Protocol error. Request-scoped ANCS
command failures emit AttributesUnavailable with a Remote error. A fatal
response timeout emits AttributesUnavailable with a fatal Timeout, then Error
and StateChanged(Failed). Error messages and remote names must not include
notification content or raw GATT bytes.
`Dmn_AncsNotification` contains kind, UID, raw category and flags, category
count, and optional app identifier/title/subtitle/message/date strings.
Unknown category/flag values are preserved.

`start` is valid only in `Stopped`; a call in another state returns
`NotReady` without changing state. An invalid config or missing handler returns
`InvalidArgument` without starting. A valid start changes state through
`Starting`, `ResolvingServices`, `DiscoveringANCS`,
`SubscribingDataSource`, and `SubscribingNotificationSource`, and returns only
after `Ready` or a typed startup error. The handler is installed while stopped.
`stop` is idempotent in `Stopped` and may be called from another thread while
`start` is in progress; it requests cancellation, prevents later setup steps,
waits for the current bounded GATT operation to return, and cleans up any
subscription already acquired. A cancelled `start` returns a typed
`NotReady` error and leaves the client `Stopped` if cleanup succeeds, otherwise
`Failed`. After a runtime failure, call `stop` to complete cleanup before
starting a new session. `state()` is safe to call concurrently and returns a
consistent state snapshot. Calling `stop()` in `Failed` retries any outstanding
subscription cleanup and transitions to `Stopped` only after cleanup succeeds.
`stop()` calls are internally serialized; callers
must serialize `setEventHandler()` with `start()` and must not destroy the
client while another public method is running.

`setEventHandler` is called before `start` and returns `NotReady` unless the
client is stopped. Passing an empty handler clears it. Events are delivered
serially on one ANCS-owned worker, never on the GATT callback/D-Bus dispatch
thread.
Callbacks must return promptly, must not throw, and must not call `start`,
`stop`, or destroy the client. Applications that need blocking work marshal
owned event values to their own executor (GTK callers marshal to the GTK main
context). The GATT callback only copies bounded bytes into the ANCS event
queue; protocol parsing, scheduling, and Control Point writes happen on the
ANCS worker. The worker exists before the first subscription so immediate
notifications are not lost. Values received during startup are copied into
the bounded queue and are not processed until both subscriptions have
succeeded; StateChanged(Ready) is dispatched before any notification event.
Unsolicited Data Source values received when no request is active are ignored.
If the second subscription fails, startup rolls back the first subscription
and returns the startup error.
An empty handler removes the handler, and `start` without a handler fails
with `InvalidArgument`. StateChanged events preserve transition order. For a
fatal runtime failure, emit any affected `AttributesUnavailable` event first,
then Error, then StateChanged(Failed); startup failures emit Error then
StateChanged(Failed). Explicit stop emits StateChanged(Stopping) followed by
StateChanged(Stopped) after successful cleanup. A cleanup failure leaves the
client Failed and returns a Cleanup error.

The finite protocol limits are fixed for this increment: Notification Source
is exactly 8 bytes; one GATT callback value/Data Source response is at most
8192 bytes; app identifier 255 bytes; title 256 bytes; subtitle 256 bytes;
message 4096 bytes; date 64 bytes; 256 active notification records; 128
pending attribute requests; and 256 queued GATT events. No response or
reassembly buffer may exceed 8192 bytes. If an event/request/cache limit is
exceeded, fail the session with `ResourceLimit` rather than silently dropping
data or allocating without bound. A timed-out request emits
`AttributesUnavailable`, then a fatal `Timeout` error and fails the session;
no following request is written because a late Data Source response cannot be
unambiguously correlated. A valid Removed event clears queued work for its
UID; an already active response is consumed through its bounded completion
but discarded. Repeated Modified/Added events for one UID coalesce into one
pending request without reordering other UIDs; retain Added until the first
complete record is emitted, then use Updated for subsequent changes.
Malformed framing, mismatched UID/attribute order, or an uncorrelatable
response fails the session with `Protocol`.
Invalid UTF-8 makes that notification's attributes unavailable, emits a
nonfatal typed error, and never exposes the raw bytes as text.

`stop` first prevents new notification intake, then explicitly stops both
subscriptions, drops pending requests and session data, and joins the worker.
It attempts cleanup of both characteristics even if the first StopNotify
fails, and returns the first cleanup error. A failed cleanup remains
observable to the caller; the destructor makes the same best-effort attempt
but cannot report the failure.

ANCS does not provide a complete notification-history or synchronization
contract. The API reports events observed in the current ANCS session; it must
not promise a full current-state snapshot, delivery of every iOS notification,
or replay of events missed during a disconnect. A best-effort `PreExisting`
event at session start is an ordinary added notification event with its flag
preserved, not proof that all existing notifications were enumerated.

The default policy is no durable storage and no notification-body logging.
The explicit `monitor` command opts into displaying available notification
attributes on stdout. The library itself never logs callback contents, and
CLI diagnostics on stderr must not contain notification bodies. Callers own
any retention, redaction, filtering, or desktop-notification decisions.
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
| `DmnAncsParserRejectsMalformedNotificationSource` | Payloads other than exactly 8 bytes and reserved event IDs | Wrong length is a typed fatal protocol error; reserved IDs are reported unsupported and ignored |
| `DmnAncsParserAcceptsKnownNotificationSourceValues` | valid added/modified/removed MEIDs and flags | emits the correct normalized event fields |
| `DmnAncsParserReassemblesFragmentedDataSourceResponse` | split tuples across multiple notifications | final attribute map is correct and ordered |
| `DmnAncsParserDropsStaleRemovedUid` | removed UID response arrives late | stale response ignored and cache not resurrected |
| `DmnAncsRequestSchedulerSerializesAttributeRequests` | two requests queued while one is active | only one request is active at a time; FIFO fairness preserved |
| `DmnAncsRequestSchedulerFailsOnStaleRequestTimeout` | Data Source response never completes by deadline | Typed fatal timeout; no later request is sent because late response correlation is ambiguous |
| `DmnAncsSessionFailsAndCleansUpAfterInvalidation` | disconnect, service loss, and BlueZ restart | session fails, cached state is discarded, subscriptions are cleaned up, and no reconnect is attempted |
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

## 3.7 Primary notification-monitor CLI contract

The primary terminal command is:

```text
dmn-bluetooth monitor DEVICE
    [--adapter ADAPTER]
    [--discovery-timeout SECONDS]
    [--pair-timeout SECONDS]
    [--connect-timeout SECONDS]
    [--services-timeout SECONDS]
```

`DEVICE` is a Bluetooth address or device display name. Names match exactly
against the BlueZ display name (`Alias`, then `Name`, then address). ASCII
letters compare case-insensitively; non-ASCII UTF-8 bytes compare exactly.
There is no substring, prefix, fuzzy, or locale-dependent matching. Addresses
must be six hexadecimal octets separated by colons and compare
case-insensitively. A syntactically valid address is resolved as an address
before attempting name matching. A BlueZ object path is not accepted as a CLI
selector because it is transient.
`ADAPTER` is an adapter address or current adapter object path, consistent
with the `Dmn_BluezClient` selector contract.

`dmn-bluetooth --help` and `dmn-bluetooth monitor --help` print usage to
stdout and exit 0 without contacting D-Bus. Unknown subcommands/options,
missing arguments, and invalid timeout syntax print usage and a concise
diagnostic to stderr and exit 2. The production CLI has no private-bus or
test-mode option; tests inject the private bus through the application
composition/API boundary.

Resolution and link setup have deterministic side effects:

1. If `--adapter` is supplied, resolve and validate it before any device
   operation. Take a fresh BlueZ inventory snapshot. Resolve `DEVICE` against known
   devices, optionally restricted by `--adapter`. Exactly one match is
   required. If more than one matches, fail with the adapter/address
   candidates and require a more specific selector; never choose the first.
2. If there is no known match, select the sole powered adapter or require
   `--adapter` when multiple powered adapters exist. Start explicit discovery
   on that adapter for at most `--discovery-timeout` using BlueZ's default
   `auto` transport and no UUID filter. For a name selector, use the full
   configured window to detect duplicate exact matches; for an address
   selector, discovery may stop as soon as that exact address is observed.
   Stop only this client's session, refresh inventory, and resolve again. Do
   not pair or connect other scan results. Failure to stop the owned scan is
   a startup failure.
3. Pair the selected device with `Device1.Pair` only if the current inventory
   does not report `Paired=true`. This explicit monitor invocation opts into
   pairing, relies on the system BlueZ default agent, and does not install an
   application agent or set `Trusted`. Refresh inventory after Pair because
   BlueZ may replace object paths or connect the device while pairing.
4. Connect only the selected device if it is not already `Connected=true`.
   Refresh and confirm connection state after the operation. Never disconnect
   or unpair on normal exit or failure.
5. Wait for `ServicesResolved=true`, discover the current GATT tree, locate
   the ANCS service and its Notification Source, Control Point, and Data
   Source characteristics by UUID and parent/device relationship, validate
   required flags, subscribe to Data Source first and Notification Source
   second, and report Ready only after both subscriptions succeed. A paired
   or connected state alone is not success.

Default finite timeouts are 30 seconds for discovery, 120 seconds for pairing,
30 seconds for connection, and 30 seconds for service resolution. GATT object
discovery, each subscription, and each acknowledged write use a 10-second
operation deadline. Inventory snapshots use the configured finite
`Dmn_BluezClient` deadline. Values supplied for timeout options are positive
whole seconds; zero, negative, malformed, overflowed, and values above 600
are usage errors. Each ANCS attribute response has a 10-second deadline.
Ctrl+C and SIGTERM request orderly shutdown: cancel future work, stop any
owned discovery session, disable both subscriptions, drain records already
accepted by the CLI output queue, discard remaining session-scoped records,
and exit 0 if cleanup succeeds. A BlueZ operation already in progress is
allowed to finish or reach its bounded deadline before shutdown continues; no
rollback/unpair is attempted. The device remains paired and connected.
Cleanup failures produce a nonzero status.

### Notification output and privacy

The monitor is a live event viewer, not history or synchronization. It
displays only events observed in this run and makes no claim that iOS reports
every existing/later notification or every attribute. Available fields are
app identifier, title, subtitle, message, and date. App display name, Message
Size, action labels, and notification actions are excluded from this
increment. Missing or empty ANCS attributes are omitted.

For a completed Added or Modified attribute response, stdout contains one
record in this shape (UID is exactly eight lowercase hexadecimal digits;
flags are two lowercase hexadecimal digits):

```text
notification added uid=0x12345678 category=email count=2 flags=0x01
app_identifier: com.example.mail
title: Example
message: A message
```

Use header forms `notification added ...`,
`notification updated ...`, `notification removed ...`, and
`notification attributes unavailable event=added|updated ...`. The remaining
header fields are `uid=0x` plus eight lowercase hexadecimal digits,
`category=...`, `count=` plus the decimal raw category count, and `flags=0x`
plus two lowercase hexadecimal digits. Use category labels `other`,
`incoming_call`, `missed_call`, `voicemail`,
`social`, `schedule`, `email`, `news`, `health_fitness`, `business_finance`,
`location`, and `entertainment` for ANCS values 0 through 11; otherwise use
`unknown(raw-value)`. Count and flags are raw values from Notification Source.
Print available fields in this order: app identifier, title, subtitle,
message, date. Values are the UTF-8 strings supplied by ANCS; date is not
localized or reformatted. Omit unavailable attribute lines. A Removed event
prints immediately without attributes. A request-scoped ANCS error or invalid
text attribute prints an `attributes unavailable` record and advances the
queue. If the attribute response times out, print the same record and end the
session with status 8 because a late response cannot be safely correlated
with a later request. A fatal GATT/backend/protocol error also ends the
monitor with status 8. Do not print partial fields as a completed record.
Separate records by a blank line and flush after each.

Output contains no ANSI color/control sequences. Escape newline as `\n`,
carriage return as `\r`, tab as `\t`, and backslash as `\\`; escape other
ASCII C0/DEL bytes as `\xNN`. Escape Unicode control/format characters,
including code points U+0080–U+009F, and line/paragraph separators as
`\u{HEX}`. Preserve valid printable UTF-8. Invalid UTF-8 invalidates that
attribute response and must never be written raw to the terminal.

Status and diagnostics go to stderr; notification fields go only to stdout.
Diagnostics must not include notification content or raw GATT payloads.
The CLI prints stage progress and one `monitor ready` line to stderr only
after both subscriptions succeed. The ANCS callback quickly copies events to
a CLI-owned bounded output queue (maximum 256 events); the command's main
thread formats and writes records. A full output queue or failed stdout write
is a client failure: request session cleanup and exit nonzero rather than
silently dropping records. Invoking `monitor` is the explicit opt-in to
displaying content. There is no content log, persistence, background
retention, or external service upload. The command does not read stdin and
does not provide a JSON/CSV output mode in this increment.

Escape externally supplied adapter/device names and error text in stderr
using the same terminal-safe rules as notification fields; diagnostics must
not permit terminal control injection.

The monitor ignores SIGPIPE so it can detect a closed stdout, stop the ANCS
session, drain records already accepted when possible, and return a failure
status instead of being terminated before cleanup. On signal or runtime
failure, stop event intake first, drain already queued output in FIFO order
while stdout remains writable, then report the final error/exit. After a
failed write, discard remaining queued records and do not retry indefinitely.

### Startup, runtime, and exit behavior

| Exit status | Meaning |
|---:|---|
| `0` | Orderly Ctrl+C/SIGTERM shutdown and successful discovery/subscription cleanup |
| `2` | Invalid command line, selector syntax, or timeout option value |
| `3` | BlueZ/system D-Bus or inventory unavailable; adapter unavailable/ambiguous; no powered adapter for required scan; or discovery failure |
| `4` | Target not found after bounded discovery or target remains ambiguous |
| `5` | Pairing or connection failed, timed out, or could not be confirmed |
| `6` | ANCS service or one of its three required characteristics is absent or incompatible |
| `7` | ANCS subscription/startup authorization or GATT operation failed |
| `8` | Runtime disconnect, BlueZ/service invalidation, attribute-response timeout, malformed protocol stream, resource-limit breach, callback/output failure, or subscription cleanup failure |

Every failure has a concise stderr diagnostic with the selected stage and
typed BlueZ/GATT/ANCS detail where available. The command never reports
success based only on `Connected=true`. Missing default agent or rejected/
cancelled pairing interaction is a pairing failure. Missing ANCS service is
distinct from a service that exists but denies subscription/write
authorization. A cleanup failure takes exit status 8 even if an earlier
startup failure would otherwise use status 5, 6, or 7; report both the
original failure and cleanup failure without including notification content.

There is no automatic reconnect in this increment. On disconnect, BlueZ owner
change, device/service invalidation, or unrecoverable protocol/queue failure,
stop accepting events, attempt both notification cleanups, discard UIDs and
pending responses, emit one failure diagnostic, and exit 8. The user restarts
the command to create a new session. No stale GATT path, UID, request, or
decoded attribute may be reused across sessions.

### Hardware-independent acceptance tests

All tests run without a radio, system BlueZ daemon, paired phone, or access to
the host system bus. Parser/scheduler tests use byte fixtures; session tests
use a deterministic fake `Dmn_GattClient`; CLI end-to-end tests use the
private-D-Bus fake-BlueZ pattern and invoke the actual CLI entry point. No
fixture depends on “iPhone Old”.

The fake BlueZ fixture supplies an ObjectManager, powered adapter, and
Device1 objects whose Pair/Connect methods and Paired/Connected/
ServicesResolved property changes are observable. It supplies synthetic
ANCS GattService1/GattCharacteristic1 objects with configurable UUIDs/flags,
StartNotify/StopNotify behavior, acknowledged Control Point writes, injected
Notification Source events, fragmented Data Source values, authorization
errors, and owner/device invalidation. CLI tests construct the production
command handler with test-only injected D-Bus client configuration; they do
not add a production command-line escape hatch to the system bus. The signal
test runs that same command handler in a child process so it can deliver
SIGINT and verify process status and cleanup.

| Test | Required assertion |
|---|---|
| `DmnAncsClientSubscribesDataSourceBeforeNotificationSource` | Ready follows both successful subscriptions in the required order and is delivered before any queued initial Notification Source event. |
| `DmnAncsClientRequestsOnlyInitialMonitorAttributes` | Added/Modified requests app ID, title, subtitle, message, date with exact endianness/maxima; no display-name/action request. |
| `DmnAncsClientEmitsCompleteOptionalAttributes` | Fragmented responses yield one normalized event; absent/empty fields are missing. |
| `DmnAncsClientFailsOnAttributeResponseTimeout` | No complete response arrives before 10 seconds (injectable clock); emit AttributesUnavailable and fatal Timeout, then send no later Control Point request. |
| `DmnAncsClientDropsRemovedUidResponse` | Removed events are emitted; late results never recreate a live record. |
| `DmnAncsClientRejectsMalformedAndOversizedValues` | Invalid UTF-8 yields a nonfatal AttributesUnavailable event; bad tuple order/length, oversized fragments/responses, and wrong Notification Source length fail with typed fatal errors before unbounded allocation. |
| `DmnAncsClientHandlesRequestScopedAncsError` | Explicit ANCS command error marks only that UID unavailable and allows other queued UIDs to proceed. |
| `DmnAncsClientEnforcesQueueAndCacheLimits` | Pending-event/request and active-record caps fail explicitly, without silent loss or unbounded growth. |
| `DmnAncsClientSerializesCallbacksAndStopsCleanly` | Callbacks serialize off the GATT callback path; stop prevents later delivery and attempts both StopNotify operations. |
| `DmnAncsClientReportsSubscriptionCleanupFailure` | Explicit stop returns cleanup error; destruction is best-effort and non-throwing. |
| `DmnBluetoothMonitorSelectsExactDeviceName` | Exact ASCII-case-insensitive selection works; partial/fuzzy matches fail; duplicate names report candidates. |
| `DmnBluetoothMonitorDiscoversOnlyOnSelectedAdapter` | Unknown target uses bounded discovery only on chosen/default sole powered adapter; scanning stops before Pair/Connect. |
| `DmnBluetoothMonitorPairsAndConnectsOnlySelectedDevice` | A known target skips discovery; pair only when needed, refresh after Pair, connect only target, and leave it paired/connected after exit. |
| `DmnBluetoothMonitorRejectsIncompleteOrUnauthorizedAncs` | Missing characteristics yield status 6; denied startup operations yield status 7; Ready is not printed. |
| `DmnBluetoothMonitorEscapesNotificationTerminalControls` | Newlines/control bytes cannot inject terminal controls or additional records. |
| `DmnBluetoothMonitorEscapesDiagnostics` | Malicious adapter/device names and remote error text cannot inject terminal controls on stderr. |
| `DmnBluetoothMonitorStopsOnSignalAndCleansUp` | SIGINT cleans both subscriptions and exits 0; runtime invalidation/attribute timeout exits 8 without reconnect. |
| `DmnBluetoothMonitorDoesNotLogContentToStderr` | Attributes are stdout-only; stderr contains neither values nor raw payload. |
| `DmnBluetoothMonitorBoundsOutputQueue` | Slow/full output consumer causes explicit failure and cleanup, never silent event loss. |
| `DmnBluetoothMonitorRejectsInvalidTimeouts` | Zero, negative, malformed, overflowing, or above-600 timeout values exit 2 before D-Bus access. |
| `DmnBluetoothMonitorHelpDoesNotAccessBlueZ` | Both help forms print usage and exit 0 without opening a bus connection. |
| `DmnBluetoothMonitorExitsWhenStdoutCloses` | A broken pipe triggers cleanup and nonzero status instead of silently dropping content. |
| `DmnBluetoothMonitorPreservesDiscoveryCliCompatibility` | Existing `dmn-bluez-discovery` behavior and tests remain unchanged. |

These tests establish library/CLI behavior only. A separately recorded
physical-device qualification is required before claiming ANCS support for a
specific iPhone/iOS/BlueZ/adapter combination.

## 4. Session lifecycle

```text
Stopped -> Starting -> ResolvingServices -> DiscoveringANCS
        -> SubscribingDataSource -> SubscribingNotificationSource -> Ready
Ready/Starting -> Stopping (signal or explicit stop) -> Stopped
Any startup/runtime failure -> Failed -> best-effort cleanup
Failed -> explicit stop/retry cleanup -> Stopped
```

The ANCS library does not own BlueZ waiting, pairing, or connection; those are
completed by the application before `start`. Each ANCS transition is
observable through status/callbacks. V1 has no reconnect state or retry loop:
a disconnect, BlueZ owner loss, or service invalidation fails the active
session, triggers best-effort subscription cleanup, clears session-scoped
state, and requires the application to stop/finish cleanup before starting a
new session. The client remains `Failed` until explicit `stop()` completes
cleanup and transitions it to `Stopped`. Stopping cancels the current attempt
and prevents further event delivery. A cancelled startup returns to `Stopped`
directly only when cleanup succeeds.

An ANCS session starts only after both Data Source and Notification Source
notifications are enabled and ends when either is disabled, the device
disconnects, BlueZ invalidates the service tree, or the application stops.
Notification UIDs, app identifiers, pending requests, and all received data
are session-scoped. Clear all state at every session end; do not persist or
reuse it in a later session. App display-name requests/caching are deferred.

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
  starts only after completion or a confirmed request-scoped ANCS error. A
  missing-response timeout is fatal and sends no following request.
- Verify all resource limits, callback execution context/order, stop behavior,
  and typed failure/cleanup using a fake `Dmn_GattClient`.

### BlueZ and real-device

- On Linux, exercise known-device enumeration and live updates against a
  private fake bus containing multiple adapters and connected/disconnected
  devices. Verify the complete known-object inventory, the connected-only
  filtered view, and that connection/object changes are reflected.
- Exercise explicit discovery start/filter/stop through the fake bus. Verify
  only the caller's discovery session is released and listing/scanning never
  initiates pairing or connecting.
- Exercise explicit pairing through a fake `Device1.Pair`: already-paired is
  a no-op, an unpaired target uses only the current object path, the `Paired`
  property is observed before the CLI reports success, and remote
  authentication/default-agent errors produce no success row. Assert that no
  application agent is registered and no `Trusted` property is written.
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
- On each claimed Linux/BlueZ and physical iPhone/iOS combination, verify
  pairing/setup, ANCS service visibility, both subscriptions, added/modified/
  removed events, one complete attribute read, and session cleanup on
  disconnect. Record which of app identifier/title/subtitle/message/date are
  actually provided; app display-name lookup is not part of this increment.
- Record the tested OS, BlueZ, adapter, iPhone model, and iOS version. Until
  this matrix is populated, compatibility claims remain provisional.

The Phase 1 code gate is deliberately separate from the iPhone feasibility
gate: private-bus tests prove the API/CLI contract but cannot establish that
the selected iPhone exposes ANCS or authorizes its characteristics. See the
Phase 1A/1B/1C sequence and hardware record requirements in
[`ancs-bluez-client-plan.md`](ancs-bluez-client-plan.md).

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
- BlueZ, [Agent D-Bus API](https://bluez.readthedocs.io/en/latest/agent-api/):
  default-agent behavior, optional per-application agents, capability and
  authentication callbacks, and agent error outcomes.
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
