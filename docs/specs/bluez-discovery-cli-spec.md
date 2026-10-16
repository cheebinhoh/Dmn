# BlueZ Device Inventory and `--discovery` CLI

**Status:** Inventory, adapter-scoped discovery, and explicit selected-device
pair/connect/disconnect are implemented in `dmn-bluez` and
`dmn-bluez-discovery`. Private-bus tests cover their primary lifecycle and
policy-error paths, including the default-agent pairing contract. The Phase 0
acceptance gates are complete. Inventory and scan-only modes remain read-only;
device actions occur only when explicitly requested.
`ServicesResolved` is currently exposed as a reported state, not a connection
success condition. The `dmn-bluez` library now provides bounded service
resolution waits and current GATT service/characteristic discovery through
`Dmn_GattClient` and `Dmn_BluezGattClient`. The CLI-specific
`--wait-services-resolved` and GATT service-list behaviors specified below
remain design requirements and are not implemented.

## 1. Goal and boundaries

Provide a small Linux command-line application that reads BlueZ adapter/device
state over the system D-Bus and, when run with `--discovery`, starts an
explicit scan and prints BlueZ-reported devices as they appear or change. This
is the first Bluetooth deliverable; it does not depend on GATT, ANCS, an
iPhone, or notification behavior.

The CLI is an application of the `dmn-bluez` library, not the library's
implementation boundary. Keep BlueZ inventory, typed state, event delivery,
and discovery-session ownership in the library so a later GTK application can
use the same API without invoking or parsing the CLI. The currently planned
application owns one `Dmn_BluezClient` and at most one discovery session;
multi-process BlueZ activity still exists and must not be mistaken for
exclusive adapter ownership.

“Detected” means a device object reported by BlueZ for an adapter during the
current scan, plus the already-known device objects present in BlueZ's
ObjectManager inventory. It does not mean every radio-visible device. A device
may be filtered by adapter capabilities, BlueZ policy, transport, daemon
state, or scan conditions and therefore never be reported.

This feature uses the generic `Dmn_DbusClient` and typed values from
`dmn-dbus`. BlueZ-specific code belongs in the optional Linux-only
`dmn-bluez` target. It must not route requests through `Dmn_DbusInput` or
`Dmn_DbusOutput`, use raw HCI/L2CAP, add a third-party Bluetooth/GATT wrapper,
or introduce BlueZ dependencies into core `dmn`.

### In scope

- Enumerate every BlueZ adapter and every `Device1` currently exposed by
  BlueZ's ObjectManager, including disconnected/previously known devices.
- Observe `InterfacesAdded`, `InterfacesRemoved`, `PropertiesChanged`, and
  BlueZ service-owner changes; publish owned, typed inventory updates.
- Explicitly start and stop discovery on one selected powered adapter.
- Explicitly pair, connect, or disconnect one selected, currently known device.
- Optionally discover a specific target before connecting, without connecting
  unrelated scan results.
- Display initial known devices and live changes during the scan, identifying
  which adapter reported each device.
- Exit cleanly on Ctrl-C or SIGTERM, stopping only the discovery session
  started by this process.
- Provide private-bus fake-BlueZ tests and an opt-in physical-adapter smoke
  test.

### Out of scope

- Removing devices or changing trust. Pairing and connection control are
  supported only through the explicit actions described below.
- GATT service/characteristic operations, ANCS, notifications, or iPhone
  compatibility.
- Background daemon/service operation, persistence, cross-host discovery,
  desktop UI, automatic scans, or guarantees of complete radio visibility.
- User-configurable filters in the first CLI. The first scan uses BlueZ's
  default `auto` transport and no UUID filter.

### Selected-device connection behavior

Discovery identifies BlueZ-known devices; it does not connect to them.
Connection is user/application initiated for one explicitly selected device,
never a side effect of scanning, taking a snapshot, or subscribing to inventory
events.

There is a generic BlueZ device-level operation, but it is not a complete,
device-independent notion of being connected and ready to use:

| Layer | Shared or device-specific? | Responsibility |
|---|---|---|
| Selection and link request | Shared BlueZ operation | Resolve the selected device from current inventory and request a connection through `org.bluez.Device1`; report the resulting `Connected` state and typed errors. |
| Bearer/service readiness | Transport-dependent | For LE/GATT, observe service resolution and then discover the required GATT services and characteristics. A `Connected` link alone does not establish that a requested service exists or is usable. |
| Profile/protocol session | Profile- or capability-specific | Use the relevant BlueZ profile or GATT operations and validate service UUIDs, characteristic capabilities, authorization, and protocol behavior. ANCS, for example, requires the ANCS GATT service and its required characteristics; a generic connection does not imply ANCS support. |

`Device1.Connect` is the shared BlueZ device-level entry point. BlueZ may
connect auto-connectable profiles as part of that request, so callers must not
interpret it as a request for one arbitrary application protocol or as proof
that every profile is ready. Where a use case needs a particular BR/EDR
profile, that profile's documented BlueZ API and UUID may be required instead
of, or in addition to, the generic device-level request. The current
implementation exposes only the generic device-level operation; support for a
particular profile/transport use case must be established before adding a
profile-specific API.

The current API exposes explicit `pairDevice`, `connectDevice`, and
`disconnectDevice`
operations and resolves a supplied address or current device object path
against current inventory. Adapter selection disambiguates duplicate
addresses. Device object paths are transient, so the library resolves the
current path from the selected device identity (adapter plus address) just
before calling BlueZ. Connection state remains distinct from future GATT or
profile readiness; a disconnect, BlueZ owner change, or object removal
invalidates any later in-flight GATT session and cached GATT object paths.

Pairing is a separate operation and must not be silently inferred from
discovery or selection. The explicit `pairDevice` operation calls
`Device1.Pair` for an unpaired selected device and relies on BlueZ's default
agent; Dmn does not register an application-owned pairing agent. `Connect` may
trigger BlueZ/system pairing prompts or other security side effects depending
on transport and device policy. Surface authorization, pairing, and policy
failures without claiming that the device is connected or usable.

The implemented feature provides a selected-device link lifecycle, not a
generic promise that any discovered device can be used. It resolves a current
inventory target, issues only the explicit action, and confirms the requested
`Connected` state. The backend-neutral GATT abstraction and its initial BlueZ
service/characteristic operations are implemented separately. Remaining CLI
work should add complete owner-loss and device-removal cancellation semantics
for device actions and keep UUID-specific protocol logic such as ANCS above
the GATT abstraction. Hardware tests must remain opt-in and specific to each
supported transport/device class.

#### Current implementation and remaining readiness work

The shared inventory and event layers underpin the implemented device actions:

| Area | Current state | Follow-on work |
|---|---|---|
| Generic D-Bus transport | `Dmn_DbusClient` supports typed method calls, remote errors, subscriptions, and bounded call timeouts. | Retain operation-specific deadlines and preserve typed method errors. |
| BlueZ inventory identity | `Dmn_BluezClient` owns current device snapshots and maps `(adapter identity, device address)` across object-path changes. | Resolve each action against current inventory immediately before calling BlueZ; do not retain a path as durable identity. |
| Connection observation | `Device1.Connected` and `ServicesResolved` are parsed; property updates become device events. | Private-bus CLI tests verify delayed/unknown final state, target removal, and owner loss; do not treat `ServicesResolved` as generic profile readiness. |
| BlueZ API | Snapshot, events, owned discovery, and explicit address/path-resolved connect/disconnect are public. | Private-bus tests verify finite connect/disconnect method timeouts and current-path resolution. |
| CLI | Supports inventory, scan, and explicit `--pair` / `--connect` / `--disconnect`; action output is limited to the selected device and success waits for observed state. | Target-discovery interruption and ambiguous-address cases are covered; continue the full acceptance matrix below. |
| Private fake service | Handles inventory/discovery and device actions, with call/path counts, injected action failures, and property transitions. | Connect/disconnect target removal, owner-loss/restart during connect, and discovery cleanup failures are covered by private-bus tests. |

No blocking architectural defect was found in the transport, identity, or
event layers. The private-bus tests exercise normal connect/disconnect,
idempotent actions, policy failures, method timeouts, delayed state
confirmation, target removal, BlueZ owner loss during an action, target
discovery, scan cleanup ordering, and invalid option combinations. Connection
and disconnection target removal and failure to stop an owned scan before
connecting are covered. The startup/teardown error paths in the single-client
Phase 0 acceptance matrix are covered by the private-bus API and CLI tests.

#### Implemented CLI contract

The connection increment extends, but does not change, the existing inventory
and scan-only modes:

```text
dmn-bluez-discovery [--help]
dmn-bluez-discovery
dmn-bluez-discovery [--adapter <adapter-address-or-path>] --discovery
dmn-bluez-discovery [--adapter <adapter-address-or-path>] \
  --connect <device-address-or-path> [--discovery]
dmn-bluez-discovery [--adapter <adapter-address-or-path>] \
  --pair <device-address-or-path>
dmn-bluez-discovery [--adapter <adapter-address-or-path>] \
  --disconnect <device-address-or-path>
```

- `--pair`, `--connect`, and `--disconnect` are mutually exclusive,
  single-use action options. Each requires exactly one device selector.
  Duplicate actions, missing selectors, extra positional arguments, and
  unsupported combinations fail with usage and a nonzero status.
- Without a connection action, existing behavior is preserved: no
  `--discovery` prints the inventory and exits; `--discovery` by itself scans
  until Ctrl-C/SIGTERM. Neither mode may issue `Connect`, `Disconnect`, or
  `Pair`.
- Device selectors accept a Bluetooth address or current BlueZ device
  object path. Address selection uses the stable `(adapter identity, device
  address)` identity. If an address matches multiple adapters, `--adapter` is
  required; an object path is accepted only while it exists in the current
  inventory. Address comparisons are case-insensitive; object paths remain
  exact. A selector that does not match current inventory is rejected. If `--adapter` accompanies an
  object-path selector, that path must belong to the selected adapter. Do not
  silently select the first match. Adapter matching follows the existing exact
  address/object-path rules.
- `--connect SELECTOR` without `--discovery` connects only a device already
  present in the current BlueZ inventory. If it is not known, fail without
  changing device state.
- With `--connect SELECTOR --discovery`, use a target already in the initial
  inventory immediately; otherwise start the existing selected-adapter scan
  and wait at most 60 seconds for that exact target to be added. Never connect
  whichever device happens to appear first. On timeout, signal interruption,
  BlueZ loss, or scan error before a match, stop only this process's owned
  discovery session and do not issue `Connect`.
- After the target is resolved, stop any scan owned by this process before
  issuing `org.bluez.Device1.Connect`. This keeps scan lifetime and the
  connection request distinct and gives every exit path one cleanup rule.
  Do not stop discovery sessions owned by other BlueZ clients.
- `--disconnect SELECTOR` resolves an existing inventory device and requests
  `org.bluez.Device1.Disconnect`; it does not start discovery. Reject
  `--disconnect` combined with `--discovery`. If already disconnected, report
  that state without making a D-Bus call.
- `--pair SELECTOR` resolves an existing inventory device and requests
  `org.bluez.Device1.Pair` unless `Paired=true`; it does not start
  discovery. Reject `--pair` combined with `--discovery`. An already-paired
  device is an idempotent no-op. Pairing may itself connect the device and
  resolve GATT services, as defined by BlueZ. Use the configured default agent
  for prompts; do not request `Trusted=true`, register a Dmn-owned agent, or
  report success until `Paired=true` is observed. Allow a finite 120-second
  end-to-end deadline for the user-agent interaction and state confirmation.
- `Device1.Connect` may cause BlueZ/system pairing or authorization behavior;
  report its actual remote error and do not claim success based solely on
  device discovery or pairing state. Document that `Device1.Disconnect` can
  interrupt all active BlueZ profiles for the selected device, including ones
  started by another application; it is an explicit, user-requested operation.
- A successful method reply is not itself the CLI's final state. Observe
  `Device1.Connected` until the requested state is reflected, using the
  inventory event stream and one 30-second end-to-end operation deadline for
  the D-Bus method and state confirmation. Configure the method call timeout
  so it cannot exceed the remaining deadline. If state is unknown after the
  method reply, refresh the inventory once before the deadline; if state
  remains unknown or does not reach the requested value, report
  failure/timeout rather than manufacturing a state. A pre-existing target
  already in the requested state is an idempotent no-op. These initial timeout
  values are constants, not new CLI options.
- The existing `--connect` success contract confirms only `Connected=true`.
  Its final row may truthfully contain `services_resolved=false` or an empty
  value; it must not relabel the connection as GATT-ready. The optional
  `--wait-services-resolved` modifier specified below adds a distinct GATT
  service-resolution gate without changing the generic connect default.
- stdout remains RFC 4180 CSV with the existing columns. Explicit
  `--pair`/`--connect`/`--disconnect` actions omit the initial full-inventory
  rows and unrelated device-change rows, printing only the header and the
  selected device's final action row. `--discovery` remains the mode for
  viewing full inventory and live device changes. Emit the final action row
  (`paired`, `connected`, or `disconnected`) only after the method succeeds and
  the corresponding `Paired` or `Connected` state is observed. If a method
  fails or times out, write diagnostics to stderr and do not emit a
  success-shaped row. Keep logs and errors free of unrelated device payloads.
- Keep signal handlers limited to setting the stop flag. On interruption,
  finish pending cleanup and stop an owned scan in normal application context.
  A pair/connect/disconnect request already sent to BlueZ is not reported as
  cancelled unless the method result and resulting state establish that.

Exit statuses are:

| Status | Meaning |
| --- | --- |
| `0` | Inventory/help completed, discovery stopped successfully, or the requested state was observed. |
| `2` | Invalid, unknown, duplicate, or incomplete command-line option. |
| `3` | No powered Bluetooth adapter is available for discovery. |
| `4` | BlueZ is unavailable or its service owner disappeared. |
| `5` | Discovery could not start or could not be stopped cleanly. |
| `6` | Adapter selection is ambiguous. |
| `7` | The requested adapter does not match an available powered adapter. |
| `8` | D-Bus connection, policy, inventory, or other runtime error. |
| `9` | Selected device was not found or its address is ambiguous; also used for a D-Bus invalid-argument error. |
| `10` | BlueZ rejected the requested pairing, connection, or disconnection action. |
| `11` | The selected device did not reach the requested state before its deadline, or was not discovered before the scan deadline. |

If a discovery session cannot be stopped, report that cleanup failure and
return the discovery-stop error status even if the device operation otherwise
succeeded.

#### Connection acceptance matrix

All automated tests use a private D-Bus daemon and fake BlueZ service. They
must exercise the actual CLI/API over `Dmn_DbusClient`, never the host system
bus or a replacement fake `Dmn_BluezClient`.

| Test | Setup/action | Required assertion |
|---|---|---|
| `DmnBluezCli.ConnectKnownDevice` | Connect a selected inventory device | Exactly one `Device1.Connect` for the matching object path; wait for `Connected=true`; final CSV row says `connected`. |
| `DmnBluezCli.ConnectDiscoversSelectedDevice` | Target absent initially; add it during `--discovery` | The CLI connects only the selected address, stops its owned scan before `Connect`, and exits after observed connection. |
| `DmnBluezCli.DiscoveryDoesNotConnectOtherDevices` | Add unrelated devices before the requested target | No unrelated `Connect`; no action if the target never appears. |
| `DmnBluezCli.ConnectAlreadyConnectedIsNoOp` | Selected row has `Connected=true` | Success with no `Connect` call and no redundant scan. |
| `DmnBluezCli.DisconnectConnectedDevice` | Disconnect one connected device | Exactly one `Device1.Disconnect`; wait for `Connected=false`; final CSV row says `disconnected`. |
| `DmnBluezCli.DisconnectAlreadyDisconnectedIsNoOp` | Selected row has `Connected=false` | Success with no `Disconnect` call. |
| `DmnBluezCli.RequiresAdapterForDuplicateAddress` | Two adapters expose the selected address | Ambiguous selection fails; explicit adapter selects only its device. |
| `DmnBluezCli.RejectsInvalidConnectionOptions` | Duplicate action, both actions, missing selector, disconnect+discovery | Usage and invalid-argument exit status; no BlueZ state-changing method. |
| `DmnBluezCli.ConnectRemoteErrorAndTimeout` | Fake `Connect` returns a named error or never reaches `Connected=true` | Typed diagnostic/nonzero exit; no success CSV action. |
| `DmnBluezCli.DisconnectRemoteErrorAndTimeout` | Fake `Disconnect` fails or remains connected | Typed diagnostic/nonzero exit; no success CSV action. |
| `DmnBluezCli.PairsSelectedDeviceUsingBluezDefaultAgent` | Selected device is unpaired; invoke `--pair` | Exactly one current-path `Device1.Pair`; wait for `Paired=true`; final row says `paired`; no agent registration or trust change. |
| `DmnBluezCli.PairAlreadyPairedDeviceIsNoOp` | Selected device already has `Paired=true` | Success with no `Pair` call. |
| `DmnBluezCli.ReportsPairingAgentFailureWithoutSuccessRow` | `Pair` returns an authentication/default-agent error | Preserve the typed remote error and emit no paired success row. |
| `DmnBluezCli.ReportsTargetRemovalAfterConnectWithoutWaitingForTimeout` | Remove the selected device during `Connect` | Return target-not-found promptly; do not emit a connected row. |
| `DmnBluezCli.ReportsTargetRemovalAfterDisconnectWithoutWaitingForTimeout` | Remove the selected device during `Disconnect` | Return target-not-found promptly; do not emit a disconnected row. |
| `DmnBluezCli.DiscoveryStopFailurePreventsConnect` | Target appears during an owned scan; `StopDiscovery` fails | Report cleanup failure and do not issue `Connect`. |
| `DmnBluezCli.ConnectInterruptedBeforeMatch` | No target appears; send SIGINT/SIGTERM | Scan cleanup is exactly once; no `Connect` call. |
| `DmnBluezCli.ConnectCleanupOnBluezLoss` | Owner disappears while scanning or connecting | No stale success; owned discovery cleanup is attempted when possible; BlueZ loss is surfaced. |
| `DmnBluezClient.DeviceActionUsesCurrentIdentity` | Device object path changes for the same adapter/address | Resolve and call using the current path; never call a stale path. |
| `DmnBluezClient.ConnectionStateChangesRemainObservable` | Connect/disconnect calls and property changes | `Connected` updates and invalidation remain correctly reflected in owned device snapshots/events. |
| `DmnBluezClient.NoImplicitPairOrConnection` | Snapshot, subscription, scan, and scan stop | Zero `Pair`, `Connect`, or `Disconnect` calls. |

The fake BlueZ service counts and validates `Connect` and `Disconnect`
destination paths, injected action errors, property transitions, and scan-stop
ordering. Keep forbidden-method assertions for scan-only and inventory-only
cases. Rows above describe the complete acceptance target; cases noted as
remaining work in the implementation table must be added before claiming full
lifecycle acceptance. The BlueZ API/CLI CTest executable and standalone CLI
tests carry both `dmn` and `cli` labels and can be run with `ctest -L cli`.

### ServicesResolved CLI gate and GATT service reporting

**Status:** CLI behavior designed; not implemented. The library-level service
wait and GATT service/characteristic discovery are implemented.

`Connected` and `ServicesResolved` represent different milestones. BlueZ
documents `Device1.Connect` as a generic request to connect supported,
auto-connectable profiles; it may return before the remote GATT service tree
has been completely resolved. `ServicesResolved` is a read-only BlueZ property
that reports whether service discovery has resolved. BlueZ, not Dmn, sets
this property. A `Connected=true, ServicesResolved=false` row is therefore a
valid intermediate state, not proof that the connection failed and not proof
that services are absent.

GATT adds a service/attribute model on top of an ATT bearer: a remote GATT
server exposes services, each service exposes characteristics, and
characteristics may expose descriptors. UUIDs identify attribute types;
object paths and ATT handles identify current instances. For ANCS, the iPhone
acts as the GATT server/Notification Provider (NP) and the Linux host acts as
GATT client/Notification Consumer (NC) over BLE. A scan advertisement is not
the authoritative full GATT database: the client connects first, waits for
BlueZ's service resolution, and then inspects the BlueZ remote GATT objects.

BlueZ represents remote services as `org.bluez.GattService1` objects and
characteristics as `org.bluez.GattCharacteristic1` objects. The service
exposes its UUID and owning Device path; a characteristic exposes its UUID,
parent Service path, flags, and optional cached value. Dmn must enumerate
current objects through BlueZ's ObjectManager after resolution, associate
services to the selected device using their Device property and/or current
object-path ancestry, then associate characteristics with their parent
service. `Device1.UUIDs` may be used as a summary, but it is not a replacement
for inspecting the service/characteristic objects required by an application.
No client method should try to write `ServicesResolved` or invent a D-Bus
`DiscoverServices` call: the standard BlueZ GATT API publishes the resolved
remote tree and does not define such a client-side method.

`ServicesResolved=true` is a transport/service-discovery milestone only. It
does not assert that ANCS exists, that all ANCS characteristics exist, that
their flags permit the required operations, that iOS has authorized access,
or that notifications can be subscribed to. ANCS readiness is a later,
service-specific state requiring the ANCS service UUID and the required
characteristic UUIDs, successful authorization, and working subscriptions.
The Apple specification makes Notification Source mandatory within ANCS but
Control Point and Data Source optional at the profile level; this Dmn
notification-details client requires all three.

#### Requirements and design

1. Preserve the current default: `--connect DEVICE` succeeds after the
   requested BlueZ method succeeds and `Connected=true` is observed. Its final
   CSV row reports the actual `services_resolved` value (including `false` or
   unknown). Do not silently make all BR/EDR/profile connections wait on a
   GATT-specific property.
2. Add `--wait-services-resolved` as an explicit modifier valid only with
   `--connect`; it may be combined with `--discovery`. Reject it with
   `--disconnect` or without `--connect`. It does not pair, register a
   `GattProfile1`, force a bearer, or trigger a second connection request.
3. The modifier uses one finite 60-second end-to-end deadline for the method
   call, observing `Connected=true`, and observing `ServicesResolved=true`.
   Start the deadline before `Connect`, use the remaining time for method and
   snapshot calls, and wait on the existing serialized device event stream.
   Refresh the snapshot at most once when the state is unknown or an event
   was missed. If the device is already connected but unresolved, do not call
   Connect again; wait for resolution. A device already connected and resolved
   is an idempotent success.
4. Fail explicitly on BlueZ owner loss, target removal, disconnect before
   resolution, malformed state, or deadline expiry. Never emit a success row
   with `services_resolved=true` unless that value was actually observed in a
   current snapshot or property update. A caller that did not request the
   modifier retains the existing connect semantics.
5. Append a distinct terminal `services-resolved` action row, preserving the
   current CSV columns, only after both `Connected=true` and
   `ServicesResolved=true` are observed. This makes the stronger result
   distinguishable from the existing `connected` action while keeping the
   schema stable. Intermediate BlueZ property events may continue to emit
   `changed` rows.
6. Keep service enumeration separate from the boolean gate. A subsequent
   `--list-services DEVICE` read-only mode should require the device to be
   connected and resolved, query the current ObjectManager tree, and print
   current service UUIDs (and optionally their characteristics) in a
   documented service-specific CSV format. It must not connect, pair, scan, or
   mutate the device. This is the CLI-facing proof of what BlueZ discovered;
   a `true` readiness bit alone is not a service inventory.
7. For a selected ANCS session, require both the resolution gate and a fresh
   GATT object-tree query. Match the exact ANCS service and characteristic
   UUIDs; validate flags before use; never persist object paths or handles
   across disconnect, service change, or BlueZ owner change. Report separately
   `services-unresolved`, `service-absent`, `characteristic-missing`,
   `unauthorized`, and `ready` (or equivalent typed states).
8. Do not treat `Device1.Connect` as a BLE/ANCS-specific bearer request.
   BlueZ documents bearer selection for dual-mode devices; the generic action
   may select BR/EDR. Resolution of a GATT tree proves a GATT database was
   exposed, but hardware qualification must still establish that the intended
   iPhone BLE bearer was used. Do not write experimental bearer-preference
   properties implicitly; any future bearer-selection control needs its own
   explicit requirements and tests.

#### Additional acceptance cases

- `DmnBluezCli.WaitForServicesResolvedAfterConnect`: fake BlueZ initially
  reports connected with `ServicesResolved=false`, then emits true; no second
  Connect call occurs and the final `services-resolved` row reports both
  booleans true.
- `DmnBluezCli.WaitForServicesResolvedOnAlreadyConnectedDevice`: initially
  connected/unresolved device waits and succeeds on a true update with zero
  Connect calls.
- `DmnBluezCli.ServicesResolvedTimeoutAndInvalidation`: unresolved state
  times out or becomes unknown; return nonzero and emit no
  `services-resolved` success row.
- `DmnBluezCli.ServicesResolvedStopsOnDisconnectRemovalOrOwnerLoss`: resolve
  wait exits promptly and surfaces the condition; it never uses stale paths.
- `DmnBluezCli.WaitServicesResolvedRejectsInvalidMode`: reject the option
  without connect and with disconnect before opening a BlueZ action.
- `DmnBluezClient.GattObjectTreeIsScopedToCurrentDevice`: expose service and
  characteristic UUIDs for one device only; ignore objects for other devices
  and invalidate path-derived values after removal/restart.
- `DmnBluezCli.ListServicesRequiresResolvedDevice`: list service rows only for
  a connected/resolved target, distinguish an empty service list from an
  unresolved/error state, and make no state-changing D-Bus calls.
- `DmnBluezGatt.AncsServiceAndCharacteristicsAreMatchedByUuid`: detect absent,
  incomplete, and complete ANCS trees and check characteristic capabilities
  before treating the service as usable.

All tests use private D-Bus and fake BlueZ objects. Physical iPhone tests are
opt-in and must record the Linux/BlueZ version, adapter, iPhone model, and iOS
version. The GATT/ANCS requirements and research context live in
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md).

## 2. Public API and inventory model

The public header is `include/dmn-bluez-client.hpp`. The implemented API
provides an owned adapter/device snapshot, inventory events, explicit
discovery, and explicit selected-device connect/disconnect. Subsequent work
must preserve these contracts and complete the remaining acceptance gates
below.

- An initial snapshot of adapters and devices.
- A subscription or equivalent owned event stream for adapter/device
  additions, changes, removals, BlueZ owner loss/restart, and terminal errors.
- An explicit discovery operation for one adapter with idempotent stop and
  RAII cleanup. It must not be started implicitly by constructing the client,
  enumerating inventory, or subscribing to updates.
- Typed errors preserving generic D-Bus failure kind and remote BlueZ error
  name/message.

All public values own their strings and properties. Do not expose
`DBusConnection`, `DBusMessage`, D-Bus iterators, or raw object-path pointers.
Callbacks execute serially on a BlueZ-owned worker outside the libdbus
dispatch worker; callbacks should not block on D-Bus calls.

An adapter row contains its transient object path, address when available,
best available name, and `Powered` state. A device row contains its transient
object path, parent adapter identity, Bluetooth address, display name, and
optional `Alias`, `Name`, `RSSI`, `Paired`, `Connected`, and
`ServicesResolved` values. The display name is `Alias`, then `Name`, then
address. Missing optional properties do not fail enumeration. Preserve
unknown/missing state as optional rather than manufacturing false values.

The stable display identity for a row is `(adapter identity, device address)`,
not the BlueZ object path; object paths can change after disconnect,
rediscovery, or daemon restart. The CLI and API must key updates consistently
with that identity and must surface object removal instead of retaining stale
rows.

### Snapshot/event race

Install required signal subscriptions before requesting
`GetManagedObjects`. Buffer owned inventory events received before the
snapshot completes, then reconcile/replay them against the snapshot in bus
order. After the method reply, drain already-queued callbacks before closing
the staging window so signal delivery on a separate callback worker cannot
race snapshot completion. This prevents a device added or removed between
subscription and snapshot from being silently lost or resurrected as stale.
Bound the staging queue; overflow is an explicit resync/error condition, not
silent loss.

Replayed changes are folded into the returned snapshot; they are not also
delivered as live post-snapshot callbacks. Signals received after
reconciliation are delivered normally. This gives snapshot consumers one
authoritative initial view without duplicate add/change rows.

Observe `org.freedesktop.DBus.NameOwnerChanged` for `org.bluez`. On owner loss,
invalidate the current snapshot and report a service-unavailable state. On a
new owner, resubscribe/reload the full ObjectManager snapshot before reporting
inventory ready. The initial CLI may terminate with an error on BlueZ loss
rather than implementing automatic reconnection, but it must not continue
printing stale inventory as current.

## 3. Discovery semantics

1. The caller selects one powered adapter. If the CLI omits `--adapter`, it
   may select the sole powered adapter. If there are zero powered adapters,
   report a clear error; if there are multiple powered adapters, require an
   explicit selector. A selector may be an exact adapter address or object
   path; ambiguous and unknown selectors fail explicitly.
2. Subscribe to inventory changes and obtain the initial snapshot before
   calling `Adapter1.SetDiscoveryFilter` or `Adapter1.StartDiscovery`.
3. Use the BlueZ system bus, service `org.bluez`, the selected adapter's
   `Adapter1`, `SetDiscoveryFilter` with `Transport = "auto"` and no UUID
   restriction, then `StartDiscovery`.
4. Treat successful `StartDiscovery` as acquisition of this client's
   discovery session. Do not infer exclusive hardware ownership or use the
   `Discovering` property as proof that this process owns the scan.
5. Show known devices from the initial snapshot and emit live add/update/remove
   changes. Devices observed during this session are tagged as scan results;
   connected/paired state remains independent of whether they were seen in
   this scan.
6. On normal stop or process shutdown, call `StopDiscovery` only if this
   process successfully started a session. Stop is idempotent from the Dmn
   API perspective. Surface stop failure during explicit stop; during
   process teardown report it to stderr without skipping remaining cleanup.
7. Never call `Pair`, `Connect`, `Disconnect`, `RemoveDevice`, or
   `SetPowered`. Scanning and inventory observation must not change device
   pairing, connection, trust, or adapter power state.

BlueZ shares discovery sessions and merges filters across clients. Dmn releases
only the session acquired by this client and does not claim that its filter
excludes results requested by another client. With `Transport = "auto"` and
no UUID filter, this milestone requests the broadest default scan supported by
the selected adapter; it still cannot promise all nearby devices.

## 4. CLI contract

Executable: `dmn-bluez-discovery`, built only when `ENABLE_BLUEZ=ON`.

```text
dmn-bluez-discovery [--discovery] [--adapter <address-or-object-path>]
                    [--help]
dmn-bluez-discovery [--adapter <address-or-object-path>]
                    --connect <device-address-or-path>
                    [--discovery] [--wait-services-resolved]
dmn-bluez-discovery [--adapter <address-or-object-path>]
                    --disconnect <device-address-or-path>
dmn-bluez-discovery --list-services <device-address-or-path>
```

- Without `--discovery`, print the current known adapter/device inventory and
  exit. This is read-only and must not start a scan.
- Print an RFC 4180 CSV header followed by one row per known device. Columns
  are `action`, `adapter_name`, `adapter_address`, `adapter_path`,
  `adapter_powered`, `device_address`, `name`, `alias`, `reported_name`,
  `paired`, `connected`, `services_resolved`, `rssi`, `device_path`, and
  `seen_during_discovery`. Unknown optional values are empty fields; booleans
  use `true`/`false`. Quote and escape commas, quotes, and line breaks.
- With `--discovery`, emit the initial inventory rows, acquire discovery on
  the selected adapter, then print subsequent device rows with `added`,
  `changed`, or `removed` in the `action` column until Ctrl-C or SIGTERM.
  Mark devices first observed during this scan.
- Order the initial snapshot deterministically by adapter identity then device
  address. Live events print as they occur and are not reordered.
- Print usage for malformed/unknown arguments and return a nonzero exit code.
  Distinguish no usable adapter, ambiguous adapter selection, D-Bus/policy
  errors, BlueZ unavailable, and discovery-start failure in stderr and exit
  status. Do not silently fall back to the session bus or another adapter.
- Use a signal-safe stop request; perform D-Bus stop/cleanup in normal
  application context, never in a POSIX signal handler. Avoid logging secrets
  or unrelated notification/device payloads.
- CSV is the stable portable output for this milestone; JSON, shell
  completion, configurable filters, and daemonization are deferred.
- `--wait-services-resolved` and `--list-services` are planned additions only;
  their normative behavior is defined in “ServicesResolved gate and GATT
  service reporting” above. They are not currently accepted CLI options.

## 5. Build contract

- `ENABLE_DBUS` remains independent and optional.
- `ENABLE_BLUEZ` defaults off, is Linux-only, and requires
  `ENABLE_DBUS=ON`; configuration must fail clearly otherwise.
- `dmn-bluez` links to `dmn-dbus`; `dmn-bluez-discovery` links to
  `dmn-bluez`. Do not make libdbus, BlueZ, or Bluetooth hardware dependencies
  of core `dmn`.
- Install the BlueZ public header/library and CLI only when BlueZ is enabled.
- The inventory/discovery API must be usable without `ENABLE_ANCS`.
- Public headers compile independently and do not include libdbus headers.

## 6. Tests and acceptance gates

Use a private D-Bus daemon and fake BlueZ service; never use the host session
or system bus in automated tests. Integration tests must exercise the actual
public `Dmn_BluezClient` implementation over `Dmn_DbusClient`, not substitute
a fake client for the code under test.

The minimum acceptance gates for the single-client CLI/library increment are
grouped below. A gate passes only when each named case for that gate has an
executable assertion and passes; a physical scan cannot substitute for the
private-bus contract tests.

1. **Inventory correctness and identity:** pass
   `DmnBluezDeviceList.EnumeratesEmptyAndMultipleAdapterInventories`,
   `DmnBluezDeviceList.IncludesKnownDisconnectedDevices`,
   `DmnBluezDeviceList.FallsBackFromAliasToNameAndAddress`,
   `DmnBluezDeviceList.TracksObjectManagerRemovalAndAddition`,
   `DmnBluezDeviceList.TracksPropertyUpdatesWithSourcePath`,
   `DmnBluezDeviceList.InvalidatedConnectionPropertyBecomesUnknown`,
   `DmnBluezDeviceList.SnapshotReplayIsNotDeliveredAsLiveDuplicate`, and
   `DmnBluezDeviceList.ReloadsInventoryAfterBluezOwnerRestart`. This confirms
   what the CLI can list, how it reports unknown state, and that live
   connection-state changes can support a future explicit device action.
2. **CLI selection and read-only policy:** pass
   `DmnBluezCli.SnapshotPrintsPortableCsvAndDoesNotStartDiscovery`,
   `DmnBluezCli.RequiresExplicitAdapterWhenMultipleArePowered`,
   `DmnBluezCli.ExplicitAdapterSelectsOnlyMatchingPoweredAdapter`,
   `DmnBluezCli.NoUsableOrUnpoweredAdapterDoesNotStartDiscovery`,
   `DmnBluezCli.RejectsInvalidArgumentsWithUsage`,
   `DmnBluezCli.HelpPrintsUsageWithoutOpeningBluezConnection`, and the
   forbidden-method assertions in API/CLI tests. The CLI selects
   deterministically and cannot accidentally alter device state while
   listing or scanning.
3. **Owned scan lifecycle and failure handling:** pass
   `DmnBluezDiscovery.StartsAndStopsOnlyItsOwnedSession`,
   `DmnBluezDiscovery.StartFailuresDoNotAcquireOwnedSession`,
   `DmnBluezDiscovery.StopFailureRetainsOwnershipForRetry`,
   `DmnBluezDiscovery.RejectsUnpoweredAdapterWithoutCallingBluez`,
   `DmnBluezDeviceList.SignalQueueOverflowInvalidatesAndResynchronizes`,
   `DmnBluezDeviceList.StagingOverflowRejectsSnapshotAndAllowsRetry`,
   `DmnBluezCli.ScanStopsOwnedSessionOnInterrupt`,
   `DmnBluezCli.ScanStopsOwnedSessionOnSigterm`,
   `DmnBluezCli.ReportsBluezLossDuringScanWithoutStaleSuccess`,
   `DmnBluezCli.ReportsDiscoveryStartFailure`, and
   `DmnBluezCli.ReportsDiscoveryStopFailureOnTermination`. This verifies
   cleanup and resynchronization rather than only the successful scan path.

Required API tests:

- `DmnBluezInventory.EmptyAndMultiAdapterSnapshots`: an empty ObjectManager
  returns empty adapter/device collections; a two-adapter snapshot returns
  every adapter and every paired, connected, disconnected, and previously
  known device once, with each device associated with its parent adapter.
- `DmnBluezInventory.PreservesOptionalStateAndDisplayNameFallback`: verify
  `Alias` -> `Name` -> address fallback, absent optional values, and
  `Connected`/`Paired` distinctions without replacing unknown with false.
- `DmnBluezInventory.ReconcilesLiveObjectAndPropertyChanges`: after snapshot,
  add/remove adapter and device objects and change/invalidate properties;
  event payloads and `devices()` must reflect changes without stale rows.
- `DmnBluezInventory.ReconcilesSnapshotRaceAndOwnerRestart`: a signal arriving
  during `GetManagedObjects` is folded into one authoritative snapshot, not
  duplicated as a live event; owner loss invalidates state and owner restart
  reloads inventory before `ServiceAvailable`.
- `DmnBluezDiscovery.AppliesFilterAndOwnsSelectedAdapterSession`: only the
  selected powered adapter receives `SetDiscoveryFilter` with `Transport=auto`
  and no UUID restriction, followed by `StartDiscovery`; successful start
  gives this client one session and stop calls `StopDiscovery` exactly once.
- `DmnBluezDiscovery.StartFailureDoesNotCreateOwnedSession`: injected filter
  or start error is returned unchanged; start failure does not call stop and a
  subsequent stop is a no-op.
- `DmnBluezDiscovery.StopFailureRetainsOwnedSession`: failed stop is returned,
  ownership remains available for retry, and only a successful retry clears
  ownership.
- `DmnBluezDiscovery.RepeatedStopIsNoOp`: after successful stop, repeated
  stops issue no additional BlueZ method calls.
- `DmnBluezDiscovery.RejectsUnavailableOrUnpoweredAdapter`: missing,
  unknown, or unpowered adapter is rejected before any D-Bus discovery method.
- `DmnBluezReadOnlyOperations.NeverChangeDeviceOrAdapterState`: inventory,
  event subscription, scan start, and scan stop issue no `Pair`, `Connect`,
  `Disconnect`, `RemoveDevice`, or `SetPowered` call.
- `DmnBluezDiscovery.RecoversExplicitlyAfterSignalQueueOverflow`: bounded
  signal delivery overflow invalidates the old view and reports/resynchronizes
  rather than presenting stale inventory as current.

Required CLI tests:

- `DmnBluezCli.SnapshotIsReadOnlyAndCsv`: no discovery flag emits the documented
  RFC 4180 header and every inventory device, then exits without a BlueZ state
  change or `StartDiscovery`.
- `DmnBluezCli.SelectsOnlyPoweredAdapter`: select by exact adapter address and
  object path; with one powered adapter omission is accepted; with none return
  the no-usable-adapter status; with multiple powered adapters omission is
  rejected and an explicit selector starts on exactly that adapter.
- `DmnBluezCli.RejectsUnknownOrUnpoweredAdapter`: unknown or unpowered
  selection exits with the documented adapter status and performs no scan.
- `DmnBluezCli.RejectsMalformedOptions`: `--help`, `--discovery`, unknown,
  duplicate, and missing-value behavior is deterministic; invalid input
  prints usage and returns the documented usage status.
- `DmnBluezCli.StartsAfterSnapshotAndStopsOnSignal`: snapshot rows precede
  discovery; live changes are emitted; SIGINT/SIGTERM cleanup stops this
  process's session once and exits normally.
- `DmnBluezCli.PropagatesDiscoveryStartAndStopFailures`: injected method and
  policy failures produce diagnostics/nonzero status, never success-shaped
  output; teardown stop failure is reported.
- `DmnBluezCli.ExitsOnBluezLossWithoutStaleInventory`: service loss during a
  scan is reported as unavailable, stale rows are not emitted as current, and
  owned stop is attempted when BlueZ permits it.

The selected-device tests in the connection acceptance matrix apply to the
implemented connect/disconnect increment. Private-bus cases cover successful
connection, already-connected no-op, discovery of a specific target,
disconnection, policy errors, invalid arguments, address ambiguity,
current-object-path resolution, target removal during an action, BlueZ owner
loss/restart during an action, delayed/unknown final state, and finite method
timeouts. Preserve all scan-only/inventory-only forbidden-method assertions
after the fake service handles `Connect` and `Disconnect`.

Current process-level CLI coverage verifies RFC 4180 escaping in snapshot mode
without starting discovery, explicit adapter-address and object-path
selection, a live
property update during discovery, SIGINT/SIGTERM stop of the owned session,
`--help`, and unknown/duplicate/missing-value argument rejection. API
regression tests also cover preservation of an owned scan after snapshot
failure, scan-result markers across snapshot refresh, owner loss during a
snapshot, snapshot replay without duplicate live events, and device identity
across object-path changes, adapter power changes/removal, and invalidation of
the `Connected` property. Added private-bus coverage now exercises empty and
multi-adapter inventories, exact adapter selection when several are powered,
unpowered/no-adapter failures, discovery method targeting, filter/start/stop
failure behavior, retry after stop failure, BlueZ loss during scanning, and
generic signal-queue overflow followed by inventory resynchronization. The
bounded snapshot-staging overflow path is also tested: it invalidates the
partial view, returns a resource-limit error, and permits an explicit fresh
snapshot retry. The private test CLI uses a compile-time shortened operation
deadline for deterministic timeout coverage; the installed CLI retains the
30-second operation deadline. Additional process-level tests verify action
target removal, BlueZ loss/restart, and timeout without emitting success-shaped
CSV; API tests verify finite method timeouts for connect and disconnect. The
required single-client Phase 0 API/CLI gates and build matrix have passed. Two
concurrent Dmn client instances are outside the planned single-client
application acceptance gate, but must be tested before the library claims
safe concurrent discovery ownership.

Hardware smoke test (opt-in, not CI): on Linux with BlueZ and an adapter,
verify the CLI prints known devices, starts a user-requested scan, updates
results, and stops on Ctrl-C without pairing or connecting. Record Linux,
BlueZ, adapter, and transport details. This qualifies only discovery behavior;
it says nothing about GATT, iPhone ANCS, or universal radio visibility.

Recorded physical smoke test (2026-10-16): Pop!_OS 24.04 LTS, Linux
7.1.5-76070105-generic, BlueZ 5.72, Intel USB Bluetooth controller (HCI 5.4;
host metadata did not expose a more specific adapter model). The CLI printed
16 inventory rows, started discovery, emitted one live update, and exited
successfully after Ctrl-C. No pairing or connection action was requested.
Discovery was already active before the run and remained active afterward;
the CLI stopped its owned session without stopping the other discovery
session.

The inventory/discovery milestone is complete only when `ENABLE_BLUEZ=ON` configures and builds,
the private-bus API and CLI tests pass, standalone headers compile, and the
opt-in adapter smoke test has been recorded for at least one supported Linux
environment. Do not block this milestone on iPhone or ANCS feasibility.

## 7. Implementation sequence

1. Freeze the adapter/device/event/error model and CLI output/argument
   contract. Add fake-BlueZ fixtures and executable tests before implementation.
2. Close only generic D-Bus client acceptance gaps required by the BlueZ
   operations; retain all existing `dmn-dbus` endpoint/client regressions.
3. Add the optional `ENABLE_BLUEZ` CMake boundary and `dmn-bluez` target with
   the D-Bus ObjectManager snapshot, property/signal subscriptions, owner
   tracking, reconciliation, and typed inventory API.
4. Implement explicit per-adapter discovery-session ownership and finite,
   observable start/stop/error behavior.
5. Add `dmn-bluez-discovery`, its argument/output/termination tests, and
   installation rules. Verify snapshot mode is read-only and scan mode never
   pairs or connects.
6. Run private-bus tests and builds with `ENABLE_DBUS=OFF`,
   `ENABLE_DBUS=ON`/`ENABLE_BLUEZ=OFF`, and both features enabled. Record the
   opt-in adapter smoke test before marking this milestone complete.
7. Connection control is implemented separately from inventory/discovery.
   Continue closing its owner-loss/removal/timeout acceptance gaps without
   weakening scan-only/inventory-only read-only guarantees.
8. Implement the designed `--wait-services-resolved` CLI gate and GATT service
   inventory only after their private-bus tests and output contract are frozen.
9. Then proceed to ANCS per
   `ancs-bluez-client-spec.md`; do not equate a resolved service tree with
   ANCS readiness or iPhone compatibility.

## 8. Relationship to adjacent specifications

This is the BlueZ inventory/discovery and selected-device connection
specification. The longer-term
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md) covers later GATT and
ANCS functionality. The generic D-Bus API contract is in
[`dbus-client-spec.md`](dbus-client-spec.md); the existing D-Bus byte-signal
transport in [`dmesg-dbus-spec.md`](dmesg-dbus-spec.md) is unrelated to BlueZ
device discovery.

## 9. Research references

Primary research references:

- BlueZ, [Device API](https://bluez.readthedocs.io/en/latest/device-api/)
  (upstream source:
  [org.bluez.Device1](https://github.com/bluez/bluez/blob/master/doc/org.bluez.Device.rst)):
  `Connect` and `Disconnect` semantics, dual-mode bearer selection, read-only
  `Connected`, `UUIDs`, and `ServicesResolved` properties.
- BlueZ, [GATT API](https://bluez.readthedocs.io/en/latest/gatt-api/)
  (upstream sources:
  [GattService1](https://github.com/bluez/bluez/blob/master/doc/org.bluez.GattService.rst)
  and
  [GattCharacteristic1](https://github.com/bluez/bluez/blob/master/doc/org.bluez.GattCharacteristic.rst)):
  remote service and characteristic object properties, flags, value updates,
  and read/write/notification methods.
- Bluetooth SIG, [Core Specification 5.4](https://www.bluetooth.com/specifications/specs/core-specification-5-4/)
  and [Generic Attribute Profile (GATT)](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/generic-attribute-profile--gatt-.html):
  GATT client/server model, attribute discovery, and characteristic
  procedures.
- Apple, [ANCS Introduction](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Introduction/Introduction.html),
  [ANCS Specification](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Specification/Specification.html),
  and [ANCS Appendix](https://developer.apple.com/library/archive/documentation/CoreBluetooth/Reference/AppleNotificationCenterServiceSpecification/Appendix/Appendix.html):
  NP/NC roles, service/characteristic UUIDs, authorization, commands,
  fragmentation, and session lifetime.
- Apple, [Core Bluetooth central-role tasks](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/PerformingCommonCentralRoleTasks/PerformingCommonCentralRoleTasks.html):
  connect to an advertising peripheral, then discover services and
  characteristics; this documents Apple's Core Bluetooth workflow, not Linux
  support or iPhone interoperability guarantees.

BlueZ's D-Bus documentation is authoritative for BlueZ API behavior but can
differ by installed BlueZ version, so the target host version must be recorded
in hardware qualification. `ServicesResolved=true` means BlueZ reports service
discovery resolved; it is not evidence by itself that ANCS is present or
authorized.
