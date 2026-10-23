<!-- Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved. -->

# Executables and integration fixtures

**Status:** Current checked-in executable behavior; these programs are not
production service contracts.

## DMesg daemon

Application entry points live in `src/app/`; reusable library implementations
remain directly under `src/` or in component subdirectories such as `src/kafka/`.
Production executable target rules are defined in `src/app/CMakeLists.txt`.
With the current CMake directory layout, uninstalled executable binaries are
placed under `build/src/app/`; installation places enabled applications in
`bin/`.
The private-bus tests compile the Bluetooth application entry points from
`src/app/` with test-only definitions; their fixtures and CTest registration
remain in `test/CMakeLists.txt`.
The BlueZ inventory/device-actions command uses
`src/app/dmn-bluez-cli.cpp` and builds as `dmn-bluez-cli`; the selected iPhone
ANCS monitor uses `src/app/dmn-bluetooth-cli.cpp` and builds as
`dmn-bluetooth-cli`. Their source and executable names make the command-line
frontends distinct from future graphical application sources.
Each installed BlueZ/Bluetooth CLI has a matching section-1 manual under
`docs/man/`.

`src/app/dmn-dmesg-daemon.cpp` obtains the singleton runtime manager, starts a
separate `Dmn_Proc` that sleeps for ten seconds and then registers a SIGTERM
hook, and enters the runtime main loop. The source explicitly marks this as a
test harness; it does not construct a DMesgNet node or wire production I/O.
Arguments are currently unused.

The proposed per-host daemon gateway and local D-Bus client architecture is
documented in
[`dmesgnet-dbus-node-gateway-spec.md`](dmesgnet-dbus-node-gateway-spec.md);
that design is not implemented by this executable or by the
`Dmn_DMesgDbus` facade. Implementation should add a distinct D-Bus RPC service
and client-proxy API rather than turning this signal-handling stub or the
existing facade into the gateway.

## Kafka sender and receiver

`src/app/dmn-kafka-sender.cpp` and `src/app/dmn-kafka-receiver.cpp` are
standalone producer/consumer smoke examples. They read broker credentials from
environment variables, use a fixed topic, and are not production applications.
The sender produces ten values. The receiver polls until SIGINT and prints
received payloads.

## Test/build surface

The default CMake test list covers queue variants, async/pipe/pub-sub,
DMesg/DMesgNet, state, runtime, sockets, and interval-tree. Kafka tests are
registered only when `BUILD_KAFKA_TEST` is enabled. Several DMesg/DMesgNet tests
are disabled or commented out, and a general test target is marked
non-terminating. `dmn-standalone-header-check` also compiles each supported
public header as an independent translation unit. Removed legacy components
are not registered as normal behavior tests. The focused
`dmn-test-dmesg-conflict` target covers current handler-scoped conflict
behavior and the Phase 1 ordered handler-event observer without the fixed
sleeps used by older DMesg conflict tests. It does not implement or prove the
proposed D-Bus gateway service/client-proxy contract.

## Gaps / improvements

1. Replace the daemon's placeholder sleep/hook with an explicitly named demo or
   a real daemon lifecycle. Do not present it as a production service.
2. Reject or meaningfully parse command-line arguments; currently they are
   unused.
3. Re-enable disabled tests only after making them deterministic, and document
   the reason for each skipped target. Add coverage for cleanup and failures,
   not just happy-path transport.
