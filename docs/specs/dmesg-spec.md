# DMesg messaging and protobuf schema

**Status:** Current DMesg contracts are extracted from the handler API,
publisher implementation, protobuf utility macros, and checked-in schemas.
The related network bridge has a separate specification at
`../specs/dmn-dmesgnet-spec.md`.

## Component responsibilities

- `Dmn_DMesg` publishes `DMesgPb` values through `Dmn_Pub`, maintains
  per-topic counters and last-value cache, and controls handler registration.
- `Dmn_DMesgHandler` is an I/O-style publisher/subscriber endpoint. It supports
  optional topic selection, filter callback, async-process callback, buffered
  reads, writes, per-topic counters, and conflict state.
- `Dmn_DMesgHandlerProxy` is a weak-pointer proxy. The publisher owns active
  handlers and `closeHandler()` unregisters and invalidates the supplied proxy.
- `dmn-dmesg-pb-util.hpp` provides protobuf field-setting macros; it does not
  validate the whole message.

## Message contract

`DMesgPb` contains a protobuf timestamp, topic, running counter, source
identifier, source-write-handler identifier, message type, playback/conflict/
force flags, and a body oneof. The body schema reserves `sys` for node and
cluster metadata, `lock_table` for DLock snapshots, and application message
body fields at field number 5 or later. The type enum similarly reserves
library values before application-defined values. Wire field numbers and
existing enum values must remain stable; new fields should be additive.

The sys body contains the node's own record and a repeated node list. A node
record includes initialization/update timestamps, identifier, state, and
master identifier. The lock-table body is defined by `dmn-dlock.proto`.
Generated code is built from `src/proto/` by the CMake protobuf generation
macro.

## Handler and conflict behavior

Handler registration completes in the publisher's async context and performs
initial last-message playback before marking playback complete. A handler's
`read()` waits for that initialization and then blocks on its internal queue.
An async-process callback, when configured, processes accepted notifications
instead of queueing them.

Handler writes stamp the current wall-clock time, source-write-handler name,
topic (when configured and absent), source identifier (when absent), and next
handler-local topic counter. A conflicted topic rejects a non-forced write.
The publisher advances its global topic counter for accepted normal messages
and marks stale/conflict messages as conflicted; the originating handler is
blocked until conflict state is reset. Playback bypasses ordinary publisher
conflict detection. Force messages replace the counter and clear the relevant
handler conflict state.

System messages use the reserved sys topic/type and have a separate internal
publish path. System delivery is opt-in through `Handler_IncludeSys`; a handler
may disable topic filtering with `Handler_NoTopicFilter`.

## Protobuf helper requirements

The macros expect the generated header and mutate messages in place. Timestamp
helpers convert `timeval` microseconds to nanoseconds. The message-type macro
for a text body uses `assert`, so the precondition is not checked in release
builds. Macro arguments can be evaluated more than once; callers must avoid
side-effecting arguments.

## Gaps / improvements

1. Define and validate required fields and valid combinations of `type`, body
   oneof, topic, counters, and flags at the message boundary. Current protobuf
   parsing alone does not establish semantic validity.
2. Specify counter wraparound. `incrementByOne()` wraps unsigned maximum to
   one; comparisons and conflict behavior around wrap are not a full sequence
   number protocol.
3. `writeAndCheckConflict()` forces blocking and may be used only when the
   publishing path is serviced; document its wait/deadlock context.
4. `Dmn_DMesgHandler::read()` translates every queue exception into
   `nullopt`, conflating shutdown with other failures. Preserve failure detail
   or constrain the exception source.
5. Make timestamp conversion use a named helper with single evaluation and
   explicit range/normalization checks. Replace assert-only body validation
   where malformed application input is possible.
6. Add schema compatibility tests for unknown fields, enum evolution,
   malformed messages, required semantic shape, counter wrap, and playback/
   force/conflict interaction.
