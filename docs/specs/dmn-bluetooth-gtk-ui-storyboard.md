# Dmn Bluetooth GTK UI Storyboard

**Status:** Implementation-accurate overview of the current GTK 4/libadwaita
application. The frames show existing controls and behavior, not planned
Preferences, alerts, event filtering, or a separate event-detail view.

![Five implementation-accurate frames show inventory, selection, connection, monitoring, and the live event feed](images/dmn-bluetooth-gtk-ui-storyboard.svg)

The application uses a resizable two-pane layout at wide window sizes. Below
the libadwaita breakpoint, it shows either the device list or selected-device
page, with a Back button. The app menu contains About and Quit.

## Current screens and interaction contract

| Frame | Current UI state | User action and behavior |
| --- | --- | --- |
| 01 | **Inventory and empty detail.** The device pane contains adapter selection, Refresh/Scan/Stop scan, sortable Name/Status headings, and the device list. The detail pane prompts the user to choose a device. | Scanning, pairing, and connection are never initiated on launch. Monitoring resumes only for a saved device with previously enabled monitoring intent, after BlueZ confirms it is connected. |
| 02 | **Device selection.** Device rows show a checkbox, name, paired/connection status, and per-device Connect/Disconnect and Start/Stop monitoring buttons. Save selection and Discard changes share a row with Selected only at wide sizes; those controls adapt to narrow widths. | Checkbox edits change the draft only. Selected only filters by the draft. Activating the device name opens its detail page; checking it does not navigate. |
| 03 | **Connected, not monitoring.** The selected-device page shows current device status and an empty session feed. Start monitoring is available only after BlueZ reports Connected for a saved, paired device. | Connect only connects. The app subscribes to ANCS only after the user explicitly starts monitoring; that intent is remembered and restored after relaunch once the device is connected. Stop monitoring clears the intent but leaves the Bluetooth connection intact. |
| 04 | **Monitor setup and ready.** Setup progress and monitor state are shown in the device row and selected-device status. Once ready, the feed explains that only new notifications received during this monitor session appear. | Monitor start/stop runs away from the GTK thread. A disconnect or disappearance observed during inventory polling stops an active monitor. Polling resumes monitoring only when the device has previously saved monitoring intent and is confirmed connected. |
| 05 | **Live event feed.** Entries show host receipt time, event kind, and available event text, newest first. | A new event or changed viewed device scrolls the feed to the top. Text can be selected; right-click offers Copy only, and Ctrl+C copies the selection. Routine inventory refreshes preserve the rendered feed and current text selection. |

### Narrow-window behavior

At narrow widths, the sidebar and detail page are navigated one at a time.
Device rows place their selection/name, status, and actions in separate
horizontal bands; scan and selection controls stack to avoid clipping. The
Back button returns to the device list. The same saved/draft selection and
per-device lifecycle rules apply in either layout.

### State ownership and lifecycle

- Stable adapter/device addresses identify saved devices. A saved device
  missing from BlueZ remains visible as unavailable.
- Selection, Bluetooth pairing, connection, and ANCS monitoring are separate
  states. Saving selection never pairs, connects, or starts a monitor.
- Connect requires a saved, paired device. Start monitoring additionally
  requires BlueZ-confirmed Connected state and does not perform discovery,
  pairing, or connection setup.
- Stop monitoring ends ANCS subscriptions but does not disconnect or unpair.
  Disconnect first stops an active monitor, then asks BlueZ to disconnect.
- An explicit monitoring choice persists across app exits. On relaunch, and
  after a device reconnects, the app resumes only those saved devices for
  which monitoring intent remains enabled; it never connects devices
  automatically.
- Event data is held only in bounded process memory. Removed events do not
  display stale attributes. Closing the app waits for monitor/scan cleanup and
  then quits; it does not disconnect or unpair devices.
- Device inventory polling does not recreate an unchanged event feed, which
  avoids clearing a text selection during routine refreshes.

## Not implemented in the current UI

The following remain proposed work in
[the application specification](dmn-bluetooth-gtk-app-spec.md): Preferences,
desktop alerts, event search/filtering, a separate event-details view,
notification clearing, confirmation dialogs, and configurable
keep-monitoring-on-close behavior. They are intentionally absent from these
current-state mockups.

## GNOME HIG considerations

- The adaptive layout uses a breakpoint and a Back action instead of retaining
  a narrow split pane. List rows and controls stack at narrow widths.
- Buttons and checkboxes have descriptive accessible names; monitor and
  connection state is communicated as text, not color alone.
- Ongoing monitor status remains in the selected-device context rather than
  being represented by transient feedback.
- The event feed is plain selectable text, with a Copy-only context menu to
  avoid presenting irrelevant editing actions.

See the application's [implementation status and proposed roadmap](dmn-bluetooth-gtk-app-spec.md)
for build requirements, GNOME/GTK references, and work not represented here.
