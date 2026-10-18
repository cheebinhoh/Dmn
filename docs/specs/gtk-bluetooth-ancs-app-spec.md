# GTK+ Bluetooth and ANCS Application Specification

**Status:** Proposed GUI frontend. The reusable BlueZ/GATT/ANCS libraries and
the `dmn-bluetooth-cli` command-line application are implemented; this document
specifies GTK+ behavior, including user-visible handling of observed
real-device failures. It does not claim that a GTK application has been
implemented.

## 1. Purpose and boundaries

Provide a responsive GTK+ application for explicitly selecting an iPhone,
connecting it through BlueZ, starting an ANCS session, and displaying
notification events. Use the existing public `Dmn_BluezClient`,
`Dmn_BluezGattClient`, and `Dmn_AncsBluezMonitor` APIs directly. Do not launch
`dmn-bluetooth-cli` as a subprocess, parse its terminal output, or duplicate the
Bluetooth/ANCS protocol.

End-user executables and test-instrumented application variants belong under
`src/app/`; their build rules belong in `src/app/CMakeLists.txt` or the
corresponding test CMake configuration. Reusable protocol, BlueZ, and ANCS
implementations remain in the parent `src/` component files and library
targets. The current terminal frontend is `src/app/dmn-bluetooth-cli.cpp`.

The application must not pair or connect without a user action, must not show
notification content automatically on launch, and must not persist
notification content by default. Pairing and connection are left intact when
the user stops monitoring or the application exits.

## 2. User-visible lifecycle

Show selected-device setup as distinct states rather than treating BlueZ
connection as ANCS readiness:

1. **Not selected / disconnected**
2. **Connecting** (including pairing only when the user requested it)
3. **Waiting for services**
4. **Checking ANCS service and characteristics**
5. **Subscribing**
6. **Ready** (both required ANCS notifications are confirmed active)
7. **Failed / stopped**

Display BlueZ `Connected` and `ServicesResolved` as diagnostic facts, not as
synonyms for **Ready**. A device can be connected and have resolved services
without exposing ANCS. Likewise, successful `StartNotify` method completion
alone is not proof of an active subscription; the backend must confirm the
characteristic's `Notifying` state.

The **Ready** view may show normalized ANCS events and the attributes provided
by the phone. Notification display remains user-controlled and ephemeral.
Missing optional attributes must be shown as unavailable rather than as a
failed session.

## 3. Observed failures and required GUI behavior

These cases have occurred on real iPhones. They are distinct failure points;
the GUI must preserve the specific diagnostic and must not report success for
any of them.

### 3.1 Resolved GATT tree has no ANCS service

The phone was paired and BlueZ reported `Connected=true` and
`ServicesResolved=true`, but the resolved GATT tree contained no ANCS service
UUID. The monitor then failed with:

```text
ANCS startup failed: ANCS service was not found in the resolved GATT services
```

This is an ANCS startup failure, not a pairing or connection failure. No ANCS
subscription or notification-access prompt has occurred yet. Do not tell the
user to approve notification access as though the prompt has already been
requested.

For one observed stale-GATT incident, disconnecting/reconnecting from Pop!_OS
alone did not restore ANCS. Forgetting Pop!_OS from the iPhone, then
reconnecting and pairing again, restored ANCS discovery. The GUI must explain
this evidence-based recovery path, while making clear that it removes the
existing bond and requires pairing again. Do not automatically forget,
unpair, disconnect, or retry pairing on the user's behalf.

Suggested message:

> ANCS was not present in the services BlueZ resolved. The device is connected,
> but notification monitoring cannot start. In one observed recovery,
> disconnecting on Pop!_OS was insufficient; forgetting Pop!_OS on the iPhone
> and pairing again restored ANCS. This removes the existing pairing.

### 3.2 `StartNotify` succeeds but notification delivery is inactive

BlueZ can return success from `StartNotify` while the characteristic's
subsequent `Notifying` property remains false. The observed diagnostic is:

```text
BlueZ accepted StartNotify but reports notification delivery is not active;
the connected bearer may not provide live GATT
```

Treat this as a subscription/readiness failure. Do not enter **Ready** or
display the iOS notification-access prompt as already triggered. The diagnostic
suggests the connected bearer may not support live GATT, but does not prove
the root cause. Offer the user an explicit disconnect/reconnect action and a
retry; if it recurs, preserve the failure detail for troubleshooting. Do not
automatically remove the bond.

### 3.3 D-Bus method timeout after becoming ready

A run has reached **Ready** and later failed with:

```text
ANCS: D-Bus method call timed out
monitor failed: D-Bus method call timed out
```

This indicates a D-Bus operation did not reply before its deadline. The
current diagnostic does not identify which method timed out, so the GUI must
not assert that a particular ANCS write or notification caused it. Mark the
session failed, stop presenting it as live, and offer an explicit retry.
Preserve the error text and setup/runtime phase in diagnostic output without
logging notification bodies.

## 4. iOS notification-access prompt

ANCS service discovery, active subscriptions, and iOS notification permission
are separate milestones. On the previously qualified iPhone, after ANCS
subscriptions became active, delivery of a **new** notification caused iOS to
ask whether Pop!_OS could receive notifications. No event arrived before
approval; notification delivery worked after the user allowed access.

The GUI must:

- explain that iOS may prompt only after ANCS is active and a new notification
  arrives;
- never claim that pairing, connection, service resolution, or `StartNotify`
  acceptance proves permission or delivery;
- keep the session status clear while waiting for the first event or user
  approval; and
- leave the decision to the user on the phone.

The prompt timing is observed behavior, not a guarantee for every iOS version,
device state, or notification category.

## 5. Threading, callbacks, and privacy

- Run synchronous BlueZ/GATT/monitor startup and stop operations on a worker,
  never on the GTK main thread.
- Marshal copied monitor events and progress updates onto the GTK main context
  before changing widgets.
- Keep widget and selection state owned by the GTK thread; do not access GTK
  objects from D-Bus or ANCS callback threads.
- On stop, disconnect, startup failure, or shutdown, stop the monitor and
  release callback subscriptions without unpairing or disconnecting the
  device implicitly.
- Keep notification contents in memory only by default. Do not put titles,
  messages, or other notification attributes in logs, error dialogs, or
  telemetry. Diagnostics may include device identity, lifecycle stage, typed
  failure, and non-content BlueZ state.

## 6. Acceptance criteria

1. A connected device with `ServicesResolved=true` but no ANCS service is
   displayed as connected but not ready; show the missing-service recovery
   guidance and do not claim that iOS notification access was requested.
2. A successful `StartNotify` reply with `Notifying=false` is shown as a
   subscription failure; the GUI does not enter **Ready**.
3. A D-Bus timeout after **Ready** is shown as a runtime session failure
   without guessing the timed-out method or exposing notification content.
4. The GUI distinguishes ANCS-ready from waiting for the first notification
   and explains that iOS may prompt on receipt of a new notification.
5. All synchronous Bluetooth operations leave the GTK main thread responsive;
   worker callbacks are marshalled before widget access.
6. Retry, disconnect, pair, and monitor-stop actions are explicit and scoped
   to the selected device. The GUI never automatically forgets the bond.
7. A fake-backend/UI test covers each failure state, recovery instructions,
   and the transition to ready only after both notification subscriptions are
   confirmed active.
