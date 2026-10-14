# BlueZ Device Inventory and `--discovery` CLI

**Status:** Initial implementation available. `dmn-bluez` and
`dmn-bluez-discovery` provide read-only system-bus inventory and explicit
adapter-scoped discovery. Private-bus API tests cover snapshots, name fallback,
property and ObjectManager changes, filter selection, and owned start/stop.
Process-level tests run the CLI against a private fake BlueZ service and cover
CSV escaping, read-only inventory, live updates, adapter selection, argument
errors, and SIGINT cleanup. A physical-adapter scan has been exercised, but a
formal smoke-test record and several API/CLI edge-case tests listed below
remain acceptance work; the discovery milestone is not yet complete.
Selected-device connect/disconnect is a proposed, unimplemented follow-on
defined below; it does not alter the current read-only discovery contract.

## 1. Goal and boundaries

Provide a small Linux command-line application that reads BlueZ adapter/device
state over the system D-Bus and, when run with `--discovery`, starts an
explicit scan and prints BlueZ-reported devices as they appear or change. This
is the first Bluetooth deliverable; it does not depend on GATT, ANCS, an
iPhone, or notification behavior.

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
- Display initial known devices and live changes during the scan, identifying
  which adapter reported each device.
- Exit cleanly on Ctrl-C or SIGTERM, stopping only the discovery session
  started by this process.
- Provide private-bus fake-BlueZ tests and an opt-in physical-adapter smoke
  test.

### Out of scope

- Pairing, connecting, disconnecting, removing devices, or changing trust.
- GATT service/characteristic operations, ANCS, notifications, or iPhone
  compatibility.
- Background daemon/service operation, persistence, cross-host discovery,
  desktop UI, automatic scans, or guarantees of complete radio visibility.
- User-configurable filters in the first CLI. The first scan uses BlueZ's
  default `auto` transport and no UUID filter.

### Follow-on: connecting to a selected device

Discovery identifies BlueZ-known devices; it does not connect to them. A
future connection increment should be user/application initiated for one
explicitly selected device, rather than a side effect of scanning, taking a
snapshot, or subscribing to inventory events.

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
of, or in addition to, the generic device-level request. The implementation
must establish and document the exact BlueZ operation for each supported
transport/use case before exposing a stable public API.

The future API should report distinct states rather than collapse them into a
single success flag: connection requested/connected or failed; for GATT,
services unresolved/resolved; and for a selected profile or application
protocol, ready/unsupported/unauthorized/failed. Device object paths are
transient, so a request should be associated with the selected device identity
(adapter identity plus Bluetooth address) and use a current BlueZ object path
only while valid. A disconnect, BlueZ owner change, or object removal
invalidates the in-flight session and any cached GATT object paths.

Pairing is a separate operation and must not be silently inferred from
discovery or selection. Do not add an application-owned pairing agent or call
`Pair` as an automatic fallback. Before implementing `Connect`, specify and
test whether the supported connection flow may trigger BlueZ/system pairing
prompts or other security side effects for each target transport and device
class. Surface authorization, pairing, and policy failures without claiming
that the device is connected or usable.

The proposed next increment is a selected-device connection lifecycle, not a
generic promise that any discovered device can be used:

1. Let the application select a current device from inventory and request
   connect or disconnect explicitly; never connect every scan result.
2. Observe BlueZ connection and service-resolution changes and propagate
   owner-loss, device-removal, timeout, authorization, and remote BlueZ errors.
3. For LE/GATT consumers, add service/characteristic discovery and operations
   as a separate GATT abstraction. Keep UUID-specific protocol logic, such as
   ANCS, above that abstraction.
4. Add private-bus lifecycle/error tests and opt-in hardware tests for each
   supported transport/device class. A successful test for one profile or
   device does not certify the others.

#### Readiness review of the current implementation

The existing layers provide useful prerequisites but do not yet implement
device actions:

| Area | Current state | Follow-on work |
|---|---|---|
| Generic D-Bus transport | `Dmn_DbusClient` supports typed method calls, remote errors, subscriptions, and bounded call timeouts. | Select an operation-specific timeout and preserve typed method errors. |
| BlueZ inventory identity | `Dmn_BluezClient` owns current device snapshots and maps `(adapter identity, device address)` across object-path changes. | Resolve each action against current inventory immediately before calling BlueZ; do not retain a path as durable identity. |
| Connection observation | `Device1.Connected` and `ServicesResolved` are parsed; property updates become device events. | Wait for and verify the requested `Connected` state after the method result; do not treat `ServicesResolved` as generic profile readiness. |
| BlueZ API | Snapshot, event subscription, and owned discovery start/stop are public. No selected-device connect/disconnect operation is exposed. | Add explicit selected-device operations and typed errors; keep scan-only methods side-effect-free. |
| CLI | Supports inventory, `--discovery`, and `--adapter`; there is no target selection or action mode. | Add the mutually exclusive explicit action syntax below and preserve existing invocation behavior. |
| Private fake service | Handles inventory/discovery and counts unexpected methods as forbidden. | Add explicit `Device1.Connect`/`Disconnect` handlers, per-path counters, injected errors, state changes, and call-order assertions. |

No blocking architectural defect was found in the existing transport,
identity, or event layers for this extension. The absent API/CLI actions and
fake-service handlers are scope gaps, not defects in the read-only milestone.
The existing live-device event and stable-identity tests cover key
prerequisites, but do not replace the connection-specific cases below. The
current API/CLI suite also has acceptance gaps for broader adapter and
multi-client behavior; keep those gaps explicit in Section 6 rather than
claiming complete coverage.

#### Proposed CLI contract

The connection increment extends, but does not change, the existing inventory
and scan-only modes:

```text
dmn-bluez-discovery [--help]
dmn-bluez-discovery
dmn-bluez-discovery [--adapter <adapter-address-or-path>] --discovery
dmn-bluez-discovery [--adapter <adapter-address-or-path>] \
  --connect <device-address-or-path> [--discovery]
dmn-bluez-discovery [--adapter <adapter-address-or-path>] \
  --disconnect <device-address-or-path>
```

- `--connect` and `--disconnect` are mutually exclusive, single-use action
  options. Both require exactly one device selector. Duplicate actions,
  missing selectors, extra positional arguments, and unsupported combinations
  fail with usage and a nonzero status.
- Without a connection action, existing behavior is preserved: no
  `--discovery` prints the inventory and exits; `--discovery` by itself scans
  until Ctrl-C/SIGTERM. Neither mode may issue `Connect`, `Disconnect`, or
  `Pair`.
- Device selectors accept an exact Bluetooth address or current BlueZ device
  object path. Address selection uses the stable `(adapter identity, device
  address)` identity. If an address matches multiple adapters, `--adapter` is
  required; an object path is accepted only while it exists in the current
  inventory. Compare Bluetooth addresses case-insensitively after validating
  the address form; object paths remain exact. If `--adapter` accompanies an
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
- Do not invoke `Pair` or register a pairing agent. `Device1.Connect` may cause
  BlueZ/system pairing or authorization behavior; report its actual remote
  error and do not claim success based solely on device discovery or pairing
  state. Document that `Device1.Disconnect` can interrupt all active BlueZ
  profiles for the selected device, including ones started by another
  application; it is an explicit, user-requested operation.
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
- stdout remains RFC 4180 CSV with the existing columns. Print live inventory
  changes as before; emit a final action row (`connected` or `disconnected`)
  only after the method succeeds and the matching `Connected` state is
  observed. If a method fails or times out, write diagnostics to stderr and do
  not emit a success-shaped row. Keep logs and errors free of unrelated device
  payloads.
- Keep signal handlers limited to setting the stop flag. On interruption,
  finish pending cleanup and stop an owned scan in normal application context.
  A connect/disconnect request already sent to BlueZ is not reported as
  cancelled unless the method result and resulting state establish that.

Proposed exit statuses are: `0` success/already-in-requested-state; `2`
invalid CLI; existing discovery/inventory errors retain their current codes;
`9` target not found or ambiguous; `10` connection/disconnection rejected by
BlueZ or bus policy; and `11` requested connection state not observed before
the finite deadline. If a discovery session cannot be stopped, report that
cleanup failure and return the existing discovery-stop error status even if
the device operation otherwise succeeded.

#### Required connection tests

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
| `DmnBluezCli.ConnectInterruptedBeforeMatch` | No target appears; send SIGINT/SIGTERM | Scan cleanup is exactly once; no `Connect` call. |
| `DmnBluezCli.ConnectCleanupOnBluezLoss` | Owner disappears while scanning or connecting | No stale success; owned discovery cleanup is attempted when possible; BlueZ loss is surfaced. |
| `DmnBluezClient.DeviceActionUsesCurrentIdentity` | Device object path changes for the same adapter/address | Resolve and call using the current path; never call a stale path. |
| `DmnBluezClient.ConnectionStateChangesRemainObservable` | Connect/disconnect calls and property changes | `Connected` updates and invalidation remain correctly reflected in owned device snapshots/events. |
| `DmnBluezClient.NoImplicitPairOrConnection` | Snapshot, subscription, scan, and scan stop | Zero `Pair`, `Connect`, or `Disconnect` calls. |

The fake BlueZ service must count and validate `Connect` and `Disconnect`
destination paths, return values/errors, and ordering relative to
`StopDiscovery`. Keep the existing forbidden-method assertions for scan-only
and inventory-only cases. Add tests for connect/disconnect policy errors,
device removal during an action, BlueZ restart, and cleanup failures before
accepting the feature.

## 2. Public API and inventory model

The public header is `include/dmn-bluez-client.hpp`. The initial API provides
an owned adapter/device snapshot, inventory events, and explicit discovery;
subsequent work must preserve these contracts and complete the acceptance
gates below.

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

Required API tests:

- Enumerate zero, one, and multiple adapters and devices across adapters;
  include connected, disconnected, paired, and previously known devices.
- Verify `Alias` -> `Name` -> address fallback and absent optional properties.
- Verify initial snapshot plus `InterfacesAdded`, `InterfacesRemoved`, and
  `PropertiesChanged`, including the subscribe/snapshot race and BlueZ restart.
- Verify one selected adapter receives `SetDiscoveryFilter(auto, no UUID
  filter)`, `StartDiscovery`, and exactly one owned `StopDiscovery`.
- Verify no Pair/Connect/Disconnect/RemoveDevice/SetPowered calls occur for
  snapshot, subscription, start, or stop behavior.
- Verify cancellation, repeated stop, startup error, stop error, disconnect,
  bounded staging-queue overflow, and stale-path invalidation.
- Verify two clients do not cause one client to claim or release the other's
  discovery session. Model merged-filter behavior without assuming exclusive
  discovery ownership.

Required CLI tests:

- Parse `--help`, `--discovery`, and adapter selection; reject unknown,
  duplicate, or missing option values.
- No discovery flag prints snapshot and never calls `StartDiscovery`.
- Discovery mode starts only after initial subscription/snapshot, prints
  subsequent changes, and requests stop on termination.
- Adapter selection rules for zero/one/multiple powered adapters are enforced.
- Error output and exit statuses distinguish argument, adapter, bus/policy,
  BlueZ-service, and discovery-start failures.

The following selected-device tests are required only for the proposed
connection increment above; they are not acceptance criteria for the current
read-only discovery milestone:

- Implement every case in the “Required connection tests” table above,
  including state observation, exact object-path targeting, method ordering,
  timeout, interruption, cleanup, and no implicit pairing.
- Preserve all scan-only/inventory-only forbidden-method assertions after the
  fake service is taught to handle `Connect` and `Disconnect`.

Current process-level CLI coverage verifies RFC 4180 escaping in snapshot mode
without starting discovery, an explicit adapter-address selection, a live
property update during discovery, SIGINT-triggered stop of the owned session,
`--help`, and unknown/duplicate/missing-value argument rejection. API
regression tests also cover preservation of an owned scan after snapshot
failure, scan-result markers across snapshot refresh, owner loss during a
snapshot, snapshot replay without duplicate live events, and device identity
across object-path changes, adapter power changes/removal, and invalidation of
the `Connected` property. The remaining acceptance cases above, including
multiple adapters/devices, broader failure-path, scan cancellation, and
two-client behavior, must be covered before declaring the milestone complete.

Hardware smoke test (opt-in, not CI): on Linux with BlueZ and an adapter,
verify the CLI prints known devices, starts a user-requested scan, updates
results, and stops on Ctrl-C without pairing or connecting. Record Linux,
BlueZ, adapter, and transport details. This qualifies only discovery behavior;
it says nothing about GATT, iPhone ANCS, or universal radio visibility.

The milestone is complete only when `ENABLE_BLUEZ=ON` configures and builds,
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
7. In a separate follow-on change, add explicit target resolution,
   connect/disconnect API operations, the CLI action modes, and the private-bus
   tests in the follow-on contract. Do not weaken the existing read-only
   inventory/discovery guarantees.
8. Only after this milestone, proceed to the separate ANCS/GATT feasibility
   and implementation phases in `ancs-bluez-client-plan.md`.

## 8. Relationship to adjacent specifications

This is the first BlueZ implementation milestone. The longer-term
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md) covers later GATT and
ANCS functionality. The generic D-Bus API contract is in
[`dbus-client-spec.md`](dbus-client-spec.md); the existing D-Bus byte-signal
transport in [`dmesg-dbus-spec.md`](dmesg-dbus-spec.md) is unrelated to BlueZ
device discovery.
