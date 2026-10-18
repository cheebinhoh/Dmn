# Dmn Bluetooth GTK UI Storyboard

**Status:** Proposed interaction design for the GTK 4/libadwaita frontend.
These are implementation-oriented wireframes, not pixel-perfect mockups.

![Nine-frame GTK storyboard from first launch and device selection through monitoring, Preferences, recovery, and quit](images/dmn-bluetooth-gtk-ui-storyboard.svg)

This storyboard expands the existing
[wide-window sketch](images/dmn-bluetooth-gtk-app.svg) into nine static frames
depicting deterministic UI states and user-triggered transitions. Read frames
**01–09** in order for the primary journey. The first release should
qualify only one active phone until multi-monitor behavior passes the release
gate; the selector must enforce the configured supported limit.

## Story frames and interaction contract

| Frame | Screen and entry condition | User action and next state | Required behavior |
| --- | --- | --- | --- |
| 01 | **First launch / empty inventory.** The app explains that only live ANCS notifications are available and are held for this session. | **Refresh devices** rereads inventory. **Scan for devices** starts an explicit adapter scan → 02. | Read BlueZ inventory on a worker. Do not pair/connect or scan automatically. Bound scans to 30 seconds, stop only app-owned scans, and surface scan errors. Show loading, empty, and inventory-error states; a Bluetooth error must not look like “no devices.” |
| 02 | **Choose device to monitor.** Show paired and unpaired devices, adapter context, connection facts, and a stable address in row details. Selection is a draft. | Select an eligible device, then **Save selection** → saved selection state. **Discard changes** restores the persisted choice. **Monitor selected** is enabled only when the draft is saved. **Stop scan** releases an active scan. | State the supported active-device limit. In the initial one-phone release, prevent selecting more than one target. Same-name devices on different adapters remain distinguishable. A newly discovered device is eligible only after it appears in scan results. |
| 03 | **Explicit start / system pairing.** The user invokes **Monitor selected** for a saved device. | If unpaired, the app requests pairing and the desktop/BlueZ presents its system flow; user approval continues to 04. Decline/failure returns to the device row with a specific status. | Label the target and explain that pairing is requested now. Do not imitate the system authorization dialog or report pairing success before BlueZ confirms it. A paired device proceeds directly to 04. |
| 04 | **Connecting and ANCS setup.** Show a textual stepper: Pairing (if needed) → Connecting → Waiting for services → Checking ANCS → Subscribing. | Both required subscriptions become active → 05. Failure → 08. User presses **Stop selected** → queued stop state, then 02/09 as appropriate. | Connected and Services Resolved are diagnostic states, not Ready. Keep the window responsive; show progress and an honest queued-stop message because stop does not cancel synchronous startup. |
| 05 | **Ready, waiting for first event.** The selected phone is ANCS Ready, but the session feed is empty. | A new phone notification arrives → 06. | Explain that iOS may ask for notification access only after a new notification; approval happens on the phone and is not inferred from Ready. Do not imply that this app can retrieve older phone notifications. |
| 06 | **Live feed and event details.** The existing main-window sketch represents this state. | Select/activate a row → show available attributes and both host receipt time and phone-provided time where present. Search/filter narrows rows. Clear removes only this session's feed. | Keep feed newest-first by arrival sequence, bounded to 1,000 rows; distinguish Added, Updated, and Removed. A Removed row must not reuse stale message attributes. Missing attributes are labeled unavailable, not fabricated. |
| 07 | **Preferences.** Open from the app menu or header action; use a standard Preferences surface. | Toggle a preference → persist that setting; close Preferences → return to the same app state. | Include only specified settings: **Desktop alerts** (Off by default), **Keep monitoring when the window is closed** (Off by default), and concise privacy/lifecycle explanations. Alerts remain generic and content-free by default. Do not offer a fake “notification history” or content-persistence control. |
| 08 | **Failure and recovery.** Show a persistent status in the selected device context, with concise summary and expandable/copyable payload-free diagnostic detail. | **Retry** starts a new explicit attempt; **Stop** ends the session; missing-ANCS recovery guidance may direct the user to forget the bond on the phone, but never performs that action. | Render missing ANCS, inactive Notifying, connection/pairing failure, and runtime D-Bus timeout as distinct states. Do not falsely claim iOS was prompted, infer the timed-out method, or automatically disconnect/unpair. Use an in-window banner for ongoing state and a transient toast only for short-lived action feedback. |
| 09 | **Stop / close / quit decision.** Shown only when an active monitor would otherwise be stopped or kept alive. | **Stop monitoring and quit** waits for cleanup then exits. **Keep monitoring and hide** is shown only when the explicit preference is on. **Cancel** returns to the app. | Default close behavior stops monitoring and quits. A hidden window is not a daemon. Do not report stopped until teardown completes; never disconnect or remove the bond as a side effect. |

## Transitions and UI state ownership

- Device inventory, selection draft, saved selection, per-device monitor state,
  event store, filters, and Preferences are distinct state. Refreshing inventory
  must not silently change selection; a saved target missing from BlueZ remains
  visible as **Unavailable**.
- The **Save selection** action commits only the device identifiers. It does
  not start pairing or monitoring. A dirty selection disables **Monitor
  selected** until the user saves or discards it.
- Discovery is a separate explicit action from connecting/monitoring. Use
  `Dmn_BluezClient::startDiscovery()`/`stopDiscovery()` on a worker, expose the
  selected powered adapter when multiple adapters exist, and stop discovery
  on timeout, cancellation, error, or shutdown. Do not stop a discovery session
  owned by another client.
- Starting is always an explicit user action. For an unpaired device, only
  **Monitor selected/Start** can request pairing; the platform's system
  authorization UI is outside the app's screen flow. Pairing denial leaves the
  device unpaired and does not trigger an automatic retry.
- The state **Ready** is reached only after BlueZ confirms both required ANCS
  subscriptions are active. **Waiting for first event** is a distinct empty
  feed state. A phone prompt may occur on a new event, not merely on connection.
- Device lifecycle errors are durable until a later user retry or a new
  successful state transition. Short action acknowledgements (saved, cleared)
  may use a toast; connection/readiness/failure states stay visible in the
  device context.
- Stop during synchronous setup means **Stop requested — waiting for current
  setup to finish**. The controller queues stop; it must not offer **Cancel
  setup** unless the backend later implements and tests cancellation.
- At narrow widths, use the same state model in a single-view navigation flow:
  inventory → selected device/feed → event details. Provide an accessible Back
  action and preserve selection/filter context. Preferences and critical error
  recovery remain reachable without a split pane.
- All screen changes driven by monitor callbacks are marshalled to the GTK main
  context. Status changes must be conveyed by text/accessibility announcements,
  not color or icon alone.

## Screens/states missing from the original image set

The original GTK SVG depicts only a wide, already-running dashboard. The new
storyboard adds a representative image for the main journey, but implementers
must also design and test these explicit variants rather than treating the
storyboard as exhaustive:

| Missing variant | Minimum UI treatment |
| --- | --- |
| Inventory loading, no known devices, active scan, no scan results, and inventory/scan failure | Distinct progress, actionable empty state, explicit Scan/Stop scan, and recoverable error with Refresh/Retry. Scanning never begins automatically. |
| Paired vs unpaired, disconnected vs connected, saved vs draft, unavailable saved target | Text labels and actions matching each fact; no state inferred from color. |
| Device details / address disambiguation | Stable adapter and device address available without making the full address the primary row label. |
| OS pairing/authorization prompt and user denial | Identify that the system owns the prompt; app waits for BlueZ result and offers explicit retry. |
| Every setup stage and stop-pending state | Text status progression; never use a generic spinner as proof of readiness. |
| Ready with no events; event feed no-results; Removed event; missing optional attributes; 1,000-row eviction | Separate empty/no-results states, accurate details, and nonintrusive eviction notice. |
| ANCS missing, Notifying inactive, pairing/connection failure, timeout after Ready | Distinct guidance and explicit recovery actions from `gtk-bluetooth-ancs-app-spec.md`. |
| Preferences, close confirmation, hidden-window state, quitting/cleanup | Surface the defaults and lifecycle consequences before the user commits. |
| Narrow-window inventory/feed/details/Preferences | Verify navigation, Back, keyboard focus, text scaling, and no clipped actions. |
| No active monitors / one active monitor / concurrency limit reached | Disable or constrain unsupported selection clearly; never silently start only a subset. |

Desktop alerts are not storyboarded as a required in-window screen: they are
system-owned, best-effort output. Their settings and content-free policy are
represented in Frame 07; a notification popup is not proof of delivery or a
replacement for Frame 06.

## Acceptance and test mapping

| Story behavior | Existing acceptance coverage | Additional implementation test required |
| --- | --- | --- |
| Explicit discovery, pairing, and device selection | GTK-AC-02/08/09, ANCS-GUI-AC-06 | Fake inventory/model tests prove load, explicit scan start/stop/cleanup, draft, save, discard, concurrency cap, and zero Pair/Connect calls before Start. |
| Setup progress and Ready boundary | GTK-AC-02, ANCS-GUI-AC-01/02/05/07 | Controller tests cover every step and assert only confirmed active subscriptions reach Ready. |
| First-event permission explanation | ANCS-GUI-AC-04 | UI-model tests distinguish Ready/no event, event arrival, and user approval not being observable/inferred. |
| Feed, details, filters, privacy | GTK-AC-06 | Model tests cover ordering, removal, absent fields, filtering, bounded eviction, and no content in diagnostics/settings. |
| Preferences and close behavior | GTK-AC-03/04/05/07 | Settings round-trip tests verify defaults, persistence scope, alert privacy; lifecycle tests verify hide vs stop/quit. |
| Failures and recovery | ANCS-GUI-AC-01/02/03/06/07 | Fake-backend view tests cover each distinct failure and ensure retry/stop actions do not unpair or disconnect. |
| Adaptive navigation/accessibility | GTK-AC-06/07 | GTK display tests exercise wide/narrow breakpoints, Back, keyboard operation, accessible labels, and text scaling. |

## Design research and platform basis

- GNOME HIG [Dialogs](https://developer.gnome.org/hig/patterns/feedback/dialogs.html):
  reserve confirmations for consequential actions and avoid disruptive error
  dialogs where an in-context status is sufficient.
- GNOME HIG [Toasts](https://developer.gnome.org/hig/patterns/feedback/toasts.html):
  use transient feedback for actions, not ongoing connection/readiness state.
- GNOME HIG [Notifications](https://developer.gnome.org/hig/patterns/feedback/notifications.html):
  limit event-heavy alerts, offer notification controls, and keep the app's
  event feed authoritative.
- GNOME HIG [Switches](https://developer.gnome.org/hig/patterns/controls/switches.html):
  use switches only for clear binary preferences, with an explicit label.
- GTK [`GtkListView`](https://docs.gtk.org/gtk4/class.ListView.html):
  supports a dynamic device inventory using the standard GTK list model/factory
  pattern.
- libadwaita [`AdwPreferencesWindow`](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/class.PreferencesWindow.html)
  and [`AdwAlertDialog`](https://gnome.pages.gitlab.gnome.org/libadwaita/doc/main/class.AlertDialog.html):
  platform components for settings and responsive confirmation dialogs.
- Existing project [GTK app spec](dmn-bluetooth-gtk-app-spec.md) and
  [ANCS failure-state spec](gtk-bluetooth-ancs-app-spec.md) remain normative
  for API boundaries, lifecycle, exact diagnostics, and privacy.

The system pairing flow is intentionally shown as external to the app:
Bluetooth pairing and permission UI belongs to BlueZ/the desktop environment,
and the app should react to observed backend state rather than imitate or
predict that platform UI.
