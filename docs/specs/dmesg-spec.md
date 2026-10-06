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
  reads, writes, per-topic counters, conflict state, and an optional
  publisher-ordered handler-event observer configured at registration.
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
Accepted-message and event callbacks from initial playback are queued in the
handler context before the playback-complete marker. Registration then waits
for a handler-context task behind those callbacks, so `openHandler()` returns
only after initial-playback callback work has completed.
The handler's `FilterTask` runs synchronously in the publisher context because
it decides whether delivery is accepted. Accepted notifications are queued to
the handler's async context for `AsyncProcessTask`; with no such callback, they
are placed in the handler's read queue.

Handler writes stamp the current wall-clock time, source-write-handler name,
topic (when configured and absent), source identifier (when absent), and next
handler-local topic counter. A conflicted topic rejects a non-forced write.
The publisher advances its global topic counter for accepted normal messages
and marks stale/conflict messages as conflicted. The writer enters conflict;
subscribed handlers with an established counter for that topic can also enter
conflict when they receive the conflict-marked message. Playback bypasses
ordinary publisher conflict detection. Force messages replace the counter and
clear the relevant handler conflict state.

Handlers opened from `HandlerSpec` may receive a
`HandlerEventCallbackTask`; it is installed before registration and initial
playback. It reports eligible message delivery plus actual per-topic conflict
entry/resolution transitions created in publisher order and queued to that
handler's async context. Each handler has a monotonically incremented conflict
generation; message events include the current generation but do not change
it. A resolution event includes a repair/accepted message when available;
explicit clear-only resolution has no message. The observer supplements, and
does not replace, the legacy conflict callback; both it and `AsyncProcessTask`
run in handler context. Exceptions from these handler-context callbacks are
logged and isolated from publisher processing. These callbacks run serially
with other handler-context work, so they must not synchronously call APIs that
wait on either async context, re-enter the handler, or close it from within the
callback. `FilterTask` runs on the publisher context and must not synchronously
wait on or re-enter that context.

`Dmn_Pub` still invokes its generic subscriber filter and `notify()` inline in
publisher context. DMesg's handler filter is part of `notify()` and also runs
there; accepted application callbacks are dispatched to handler context.

The publisher owns the authoritative per-handler counter/conflict snapshots
and mutates them only in its serialized context. Each handler keeps a local
mirror, updated by ordered snapshots queued from the publisher; public handler
state access and writes execute on that handler's async context. A forced
handler write waits for publisher notification to finish so its
conflict-resolution snapshot is queued before the handler processes its next
task.

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
