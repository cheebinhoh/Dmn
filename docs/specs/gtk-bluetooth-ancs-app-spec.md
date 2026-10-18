# GTK Bluetooth and ANCS Application Specification

**Status:** Historical proposal for GTK failure handling. The optional GTK
application is now partially implemented; its current behavior and build
contract are maintained in the
[GTK application specification](dmn-bluetooth-gtk-app-spec.md). Treat this
document's proposed workflows and acceptance criteria as design history or
future work, not as a description of the shipped UI.

## 1. Purpose and boundaries

Provide a responsive GTK application for explicitly selecting an iPhone,
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

This is a documented failure mode, not the current result for every device
that previously exhibited it: on 2026-10-18, `dmn-bluetooth-cli`
successfully received notifications from “iPhone Old” after earlier
no-service observations. Keep the failure handling for future/intermittent
stale-GATT states, but do not describe “iPhone Old” as currently unable to use
ANCS.

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

1. **ANCS-GUI-AC-01:** A connected device with `ServicesResolved=true` but no ANCS service is
   displayed as connected but not ready; show the missing-service recovery
   guidance and do not claim that iOS notification access was requested.
2. **ANCS-GUI-AC-02:** A successful `StartNotify` reply with `Notifying=false` is shown as a
   subscription failure; the GUI does not enter **Ready**.
3. **ANCS-GUI-AC-03:** A D-Bus timeout after **Ready** is shown as a runtime session failure
   without guessing the timed-out method or exposing notification content.
4. **ANCS-GUI-AC-04:** The GUI distinguishes ANCS-ready from waiting for the first notification
   and explains that iOS may prompt on receipt of a new notification.
5. **ANCS-GUI-AC-05:** All synchronous Bluetooth operations leave the GTK main thread responsive;
   worker callbacks are marshalled before widget access.
6. **ANCS-GUI-AC-06:** Retry, disconnect, pair, and monitor-stop actions are explicit and scoped
   to the selected device. The GUI never automatically forgets the bond.
7. **ANCS-GUI-AC-07:** Automated fake-backend and UI-model tests cover each failure state,
   recovery instructions, and the transition to ready only after both
   notification subscriptions are confirmed active.

## 7. Failure-state test and verification plan

Keep these tests in the frontend-independent status/error mapper and fake
monitor integration layer. Do not make a human connect an iPhone to exercise
routine failure rendering.

1. **Missing ANCS service.** Test fixture: BlueZ reports connected and
   services resolved, while fake GATT omits ANCS. Assert the controller maps
   the typed `AncsStartup` failure to **Connected, not ready**, does not show
   the iOS permission prompt as already triggered, and renders the complete
   forget/re-pair warning without issuing a forget/unpair request. Verify
   with a model test and a GTK view/status test driven by the fake result.
2. **Inactive `Notifying`.** Test fixture: `StartNotify` succeeds but the
   notification-source characteristic remains inactive. Assert the state is
   subscription failure, never **Ready**, and diagnostic wording remains
   uncertain about cause. Verify no auto-disconnect or bond removal call is
   sent.
3. **Timeout after Ready.** Test fixture: first transition both subscriptions
   to active/Ready, then deliver the typed timeout. Assert the UI leaves Ready,
   records runtime phase and typed timeout, does not name an unknown method,
   and offers explicit Retry. Feed a synthetic secret string in another
   payload and scan test logs/error summaries to prove it is not emitted.
4. **First event / iOS prompt.** Test fixture: monitor reaches Ready and emits
   no event, then receives a new ANCS event. Assert the waiting state explains
   that iOS may prompt, does not claim prompt approval, and transitions to
   event received only after the callback. Verify both no-event and event
   cases without a phone.
5. **Recovery and lifecycle.** Test user-triggered Retry, Stop, close, and
   shutdown after each injected error. Assert no automatic forget/unpair,
   no duplicate callbacks after teardown, and no Ready state from BlueZ
   `Connected` or `ServicesResolved` alone.

Run focused model/controller tests without a display. Run the small GTK
presentation test under the frontend's display-test setup (Xvfb in CI if
required) and assert status through its accessible label/action state. Real
iPhone testing remains a separate opt-in compatibility check and cannot
replace these deterministic tests.
