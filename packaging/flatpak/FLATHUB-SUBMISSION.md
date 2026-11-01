<!-- Copyright © 2026 Chee Bin HOH. All rights reserved. -->

# Flathub submission notes

Paste into the Flathub submission pull request description.

## Why `--system-talk-name=org.bluez`

Dmn Bluetooth discovers nearby Bluetooth LE devices and connects to paired
iPhones through BlueZ. BlueZ is only reachable over the system D-Bus, and no
portal exists for Bluetooth LE GATT access, so a talk permission on
`org.bluez` is required. No other system bus names are requested.

## Why `--talk-name=org.freedesktop.Notifications` (COSMIC)

The COSMIC frontend currently sends desktop alerts directly to the standard
session-bus `org.freedesktop.Notifications` service; it does not use the
notification portal. This permission is therefore needed for the alert
feature. Alerts are enabled by default, can be disabled from the app menu, and
contain only a generic title and an aggregated notification count. They do not
include device names or notification content. The permission grants access to
that one notification service, not unrestricted session-bus access.

The GTK manifest does not request this permission. Its notification behavior
is separate from the COSMIC frontend.

## Why ANCS

The app reads notifications that an iPhone shares with accessories through
Apple's Notification Center Service (ANCS), a GATT service the iPhone exposes
to a connected Bluetooth LE peer. The user selects a paired iPhone and
explicitly starts monitoring; the iPhone also prompts for notification sharing.
Events are shown in a temporary in-app feed and held only in memory: nothing is
written to disk, sent over a network, or monitored after the app quits.

## Other permissions

- `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11`: display access.
- COSMIC additionally requests `--device=dri` for graphics acceleration.
- The GTK manifest requests `--talk-name=ca.desrt.dconf` and
  `--filesystem=xdg-run/dconf` for GSettings persistence; the COSMIC manifest
  does not request these.
- No network, no filesystem access, no device nodes.

## Reviewing without Bluetooth hardware

Reviewers do not need an iPhone, a Bluetooth adapter, or BlueZ. The GTK and
COSMIC frontends share one engine and both have a demo mode backed by its
synthetic backend:

```bash
dmn-bluetooth-gtk --demo
dmn-bluetooth-cosmic --demo
```

It shows fake phones that can be selected, saved, connected and monitored, and
produces synthetic notifications and generic desktop alerts. Demo mode never
reads or writes the user's saved devices. The engine tests use the same
backend and need no host Bluetooth dependency:

```bash
cd src/app/gui/cosmic && cargo test --workspace --locked
ctest -R dmn-test-bluetooth-cosmic-engine   # needs -DENABLE_APP_BLUETOOTH_COSMIC=ON
```

## Trademarks

"Apple", "iPhone" and "ANCS" are used only to describe compatibility. The
project is not affiliated with or endorsed by Apple Inc.
