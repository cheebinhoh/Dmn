<!-- Copyright © 2026 Chee Bin HOH. All rights reserved. -->

# Flathub submission notes

Paste into the Flathub submission pull request description.

## Why `--system-talk-name=org.bluez`

Dmn Bluetooth discovers nearby Bluetooth LE devices and connects to paired
iPhones through BlueZ. BlueZ is only reachable over the system D-Bus, and no
portal exists for Bluetooth LE GATT access, so a talk permission on
`org.bluez` is required. No other system bus names are requested.

## Why ANCS

The app reads notifications that an iPhone shares with accessories through
Apple's Notification Center Service (ANCS), a GATT service the iPhone exposes
to a connected Bluetooth LE peer. The user selects a paired iPhone and
explicitly starts monitoring; the iPhone also prompts for notification sharing.
Events are shown in a temporary in-app feed and held only in memory: nothing is
written to disk, sent over a network, or monitored after the app quits.

## Other permissions

- `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11`: display access.
- `--talk-name=ca.desrt.dconf`, `--filesystem=xdg-run/dconf`: GSettings
  persistence of window/preferences.
- No network, no filesystem access, no device nodes.

## Trademarks

"Apple", "iPhone" and "ANCS" are used only to describe compatibility. The
project is not affiliated with or endorsed by Apple Inc.
