# BlueZ Device Inventory and `--discovery` CLI

**Status:** Initial implementation available. `dmn-bluez` and
`dmn-bluez-discovery` provide read-only system-bus inventory and explicit
adapter-scoped discovery. Private-bus API tests cover snapshots, name fallback,
property and ObjectManager changes, filter selection, and owned start/stop.
Process-level tests run the CLI against a private fake BlueZ service and cover
CSV escaping, read-only inventory, live updates, adapter selection, argument
errors, and SIGINT cleanup. A physical-adapter scan has been exercised, but a
formal smoke-test record and several API/CLI edge-case tests listed below
remain acceptance work; the milestone is not yet complete.

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

Current process-level CLI coverage verifies RFC 4180 escaping in snapshot mode
without starting discovery, an explicit adapter-address selection, a live
property update during discovery, SIGINT-triggered stop of the owned session,
`--help`, and unknown/duplicate/missing-value argument rejection. API
regression tests also cover preservation of an owned scan after snapshot
failure, scan-result markers across snapshot refresh, owner loss during a
snapshot, snapshot replay without duplicate live events, and device identity
across object-path changes. The remaining acceptance cases above, including
multi-adapter, broader failure-path, and two-client behavior, must be covered
before declaring the milestone complete.

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
7. Only after this milestone, proceed to the separate ANCS/GATT feasibility
   and implementation phases in `ancs-bluez-client-plan.md`.

## 8. Relationship to adjacent specifications

This is the first BlueZ implementation milestone. The longer-term
[`ancs-bluez-client-spec.md`](ancs-bluez-client-spec.md) covers later GATT and
ANCS functionality. The generic D-Bus API contract is in
[`dbus-client-spec.md`](dbus-client-spec.md); the existing D-Bus byte-signal
transport in [`dmesg-dbus-spec.md`](dmesg-dbus-spec.md) is unrelated to BlueZ
device discovery.
