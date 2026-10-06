# Executables and integration fixtures

**Status:** Current checked-in executable behavior; these programs are not
production service contracts.

## DMesg daemon

`src/dmn-dmesg-daemon.cpp` obtains the singleton runtime manager, starts a
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

`src/dmn-kafka-sender.cpp` and `src/dmn-kafka-receiver.cpp` are stand-alone
producer/consumer smoke examples against an external broker. They configure
credentials in source, use a fixed topic, and are unsuitable for deployment.
The sender produces ten values. The receiver polls until SIGINT and prints
received payloads.

## Test/build surface

The default CMake test list covers queue variants, async/pipe/pub-sub,
DMesg/DMesgNet, state, runtime, sockets, and interval-tree. Kafka tests are
registered only when `BUILD_KAFKA_TEST` is enabled. Several DMesg/DMesgNet tests
are disabled or commented out, and a general test target is marked
non-terminating. `dmn-standalone-header-check` also compiles each supported
public header as an independent translation unit; deprecated headers are
excluded because they still depend on the obsolete queue API. Deprecated
components are not registered as normal behavior tests. The focused
`dmn-test-dmesg-conflict` target covers current handler-scoped conflict
behavior and the Phase 1 ordered handler-event observer without the fixed
sleeps used by older DMesg conflict tests. It does not implement or prove the
proposed D-Bus gateway service/client-proxy contract.

## Gaps / improvements

1. Remove secrets from the two Kafka examples, rotate credentials if they were
   valid, and source configuration from environment/secret-management input
   that is not committed.
2. Replace the daemon's placeholder sleep/hook with an explicitly named demo or
   a real daemon lifecycle. Do not present it as a production service.
3. Reject or meaningfully parse command-line arguments; currently they are
   unused.
4. Re-enable disabled tests only after making them deterministic, and document
   the reason for each skipped target. Add coverage for cleanup and failures,
   not just happy-path transport.
