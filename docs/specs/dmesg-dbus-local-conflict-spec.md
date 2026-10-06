# Local DMesg Clients over D-Bus: Handler and Conflict Contract

**Status:** Phase 1 (the DMesg handler-event observer) is implemented. The
in-process gateway core, D-Bus service, and client proxy remain proposed. This
specification defines two independent local application processes
communicating through one daemon-owned `Dmn_DMesg` publisher and a D-Bus
service. The local gateway does not require inter-node transport.

The later network integration keeps the same daemon-side service and handlers,
but replaces the local `Dmn_DMesg` instance with one
`Dmn_DMesgNet` instance, which derives from `Dmn_DMesg` and uses its same
publisher for network input/output. See
[`dmesgnet-dbus-node-gateway-spec.md`](dmesgnet-dbus-node-gateway-spec.md)
for the larger per-host architecture.

## 1. Scope and topology

The minimum topology is:

```text
Application A process                  Daemon process                Application B process
+------------------------+             +----------------------+      +------------------------+
| Dmn_DMesgDbusClient    |<-- D-Bus -->| D-Bus gateway service|<---->| Dmn_DMesgDbusClient    |
| local handler proxy    |             | server-side handlers |      | local handler proxy    |
+------------------------+             |          |           |      +------------------------+
                                       |    Dmn_DMesg          |
                                       +----------------------+
```

The diagram's D-Bus arrows mean local method calls and directed asynchronous
events, not a shared broadcast `Dmn_DMesgDbus` mesh. Both client processes
connect to one daemon service. The daemon creates one
`Dmn_DMesg::Dmn_DMesgHandler` for each opened client handler, on one
`Dmn_DMesg` publisher. The server handler proxy remains inside the daemon.
No client owns a publisher, a cluster node ID, or a server handler pointer.

For this first slice, use `Dmn_DMesg` directly so tests isolate the
cross-process handler contract from network behavior. The gateway service's
publisher dependency should be narrow enough to accept the production
daemon-owned `Dmn_DMesgNet` through its `Dmn_DMesg` base in the later
integration; do not build a second publisher or a message relay between two
publishers.

The existing `Dmn_DMesgDbus` does not implement this topology. It creates a
complete `Dmn_DMesgNet` participant over the existing broadcast signal
transport. A client that constructs it becomes another DMesgNet participant;
it does not attach to the daemon's publisher or call a daemon service.

## 2. Design alternatives and recommended simple v1

The alternatives differ in how much state and reliability the gateway adds:

| Option | Design | Benefits | Costs/risks |
|---|---|---|---|
| **A. Direct `Dmn_DMesgDbus` in every app** | Each app is a full node on the existing local signal bus; a daemon would need a second network node and a bridge between them. | Reuses the current facade with little new API. | Does not create a daemon client relationship; several DMesgNet identities per host; bridge must map counters, identity, system traffic, echoes, and conflicts across two publishers. Not a simple gateway. |
| **B. One daemon publisher plus D-Bus client proxies (recommended)** | Each app has only a proxy. The daemon owns one `Dmn_DMesg` publisher and one server handler per client handler. | One shared local counter/cache/conflict domain; few components; directly reuses DMesg handler semantics; later the same publisher can be the base of one `Dmn_DMesgNet`. | Requires a small local D-Bus service/client API and an ordered handler-event seam. |
| **C. Stateless D-Bus message relay** | Clients send raw messages; daemon forwards payloads without creating server-side DMesg handlers. | Fewer handler objects. | Cannot faithfully represent handler-local counters, conflict state, initial playback, handler ownership, or repair notifications; effectively reimplements DMesg semantics in the gateway. |
| **D. Poll-only client status** | Client periodically calls `GetStatus`/`isInConflict`; no server-pushed conflict events. | Avoids a transition callback and event stream. | Conflict visibility is delayed by polling; transient enter/resolve transitions may disappear; polling load scales with clients; not equivalent to notification semantics. |

**Choose Option B, but keep v1 deliberately small:**

- One service and one `Dmn_DMesg` publisher in the daemon.
- One server-side handler per client handler; no second publisher, message
  bridge, D-Bus byte-signal `Dmn_DMesgNet`, persistence, or reconnect replay.
- One D-Bus method path for `OpenHandler`, `ActivateHandler`, `Publish`,
  `RequestTopicResync`, and `CloseHandler`. Bind each handler directly to the
  authenticated D-Bus unique name; do not add a separate session abstraction
  in v1.
- One directed asynchronous event path for `Message`, `ConflictChanged`, and
  `ResyncRequired`, with a bounded per-handler queue.
- One narrow DMesg handler event hook for ordered message delivery and actual
  conflict entry/resolution transitions. Do not reproduce conflict detection
  or counter logic in the gateway.
- No automatic retry after a timed-out `Publish` in v1. The caller treats the
  result as unknown and queries handler status; safe retry requires a later,
  explicitly designed deduplication contract.
- If a client event queue overflows, mark that handler `ResyncRequired` and
  require reopen/snapshot; do not implement an unbounded replay log.

This is simpler than a transparent bridge and simpler than introducing a
durable event cursor. It is still an actual proxy architecture: apps call the
daemon, and the daemon applies work to its own publisher.

## 3. First-slice API and wire responsibilities

The proposed client library component is `Dmn_DMesgDbusClient`. Its
`openHandler(name, topic, options)` returns a local handler proxy with
`write()`, `read()` (or a local callback), `isInConflict(topic)`,
`getTopicRunningCounter(topic)`, and `close()`. These operations are local
library APIs mapped to D-Bus methods/events; the client does not serialize C++
callbacks or filters. Any optional filter executes in the client process.

The daemon generates opaque handler IDs. Each D-Bus method binds the claimed
handler ID to the authenticated caller's current unique bus name. A client
cannot select the server handler name or write
`sourceWriteHandlerIdentifier`, `runningCounter`, `force`, `playback`,
`conflict`, or system-message fields.

Minimum operations for the selected v1:

| Operation | Contract |
|---|---|
| `OpenHandler(topic, options)` | Create one server handler with a daemon-generated unique name; wait for a handler-context barrier after initial playback callbacks have populated its staged queue before returning its opaque ID. Do not block the shared D-Bus dispatch thread. The handler is owned by the calling D-Bus unique name. |
| `ActivateHandler(handler)` | Begin delivery after the client knows its ID; flush staged initial playback/events in order. |
| `Publish(handler, operation_id, application_payload)` | Validate an application-only payload, write through the server handler using the synchronous conflict-checking path, and return the local publisher result plus the current conflict generation. `operation_id` correlates this reply with its call only; it is not a deduplication key and does not appear in transition events. |
| `RequestTopicResync(handler, topic)` | Ask the daemon to publish its current cached value with the existing DMesg force/reset path, if policy allows. This is a local repair request, not a global winner or network commit. |
| `GetHandlerStatus(handler, topic)` | Return the daemon-side handler's current counter, conflict state, and conflict generation. It repairs proxy uncertainty; it is not a history of missed transitions. |
| `CloseHandler(handler)` | Stop admission, report/discard queued client events according to the close contract, then close the server handler. |

The exact D-Bus service name, signatures, protobuf-vs-explicit-field encoding,
error names, limits, and installation policy must be frozen before
implementation. The gateway protocol is a separate versioned service from
`org.dmn.DMesg1.Transport.Message(ay)`; do not tunnel service calls through
that signal.
`Dmn_DMesg::openHandler()` waits for publisher-side registration and playback,
but may return before the queued handler-context callbacks complete. The
gateway must perform an additional handler-context barrier before declaring
the staging queue populated; run the blocking open and barrier on a worker
that does not block shared D-Bus dispatch. `ActivateHandler` is still required
to ensure the client receives its handler ID before the daemon delivers staged
events.

## 4. Existing DMesg conflict behavior relevant to the gateway

`Dmn_DMesg` has two related kinds of local state:

- The publisher keeps one per-topic global running counter and last-message
  cache.
- Every handler keeps its own per-topic counter and a set of topics for which
  that handler is in conflict.

These are in one process today. The gateway must explicitly carry the
handler-specific outcomes to the corresponding remote client; a D-Bus method
reply for one client does not update another client's proxy automatically.

### 4.1 Handler write detects a stale counter

When the server handler writes, `writeDMesgInternal()` stamps the message,
sets the server-owned source-write-handler name, and increments that
handler's local counter. `Dmn_DMesg::publishInternal()` compares the resulting
counter with the publisher's next counter. If stale or explicitly conflicted,
it marks the message conflicted and marks the originating handler in conflict.
The conflict notification is then sent through the publisher to other
subscribed handlers. A handler in conflict rejects later ordinary writes
until repaired/resolved.

`writeAndCheckConflict()` is the current synchronous API suitable for a
gateway `Publish` operation: it waits for the local publisher path and reports
whether the writing handler is conflicted. The gateway must catch and map
the existing "handler already in conflict" exception separately from a
first-attempt conflict. Do not claim that either result says what a remote
node received.

### 4.2 Conflict notification affects other local handlers

When another handler receives a conflict-marked message for a topic, its
`notify()` path does not send that message through the ordinary read queue or
`AsyncProcessTask`. If that handler has already established a counter for the
topic, DMesg marks it conflicted and invokes its conflict callback. A handler
with no counter for the topic is not marked conflicted by this path.

Thus a conflict may affect more than the writer. Every client whose
server-side handler transitions to conflict must receive its own
handler-scoped event. The gateway must not notify only the client that sent
the write, nor broadcast one unscoped event to all clients. A handler not
marked conflicted may optionally receive a separately named
`ConflictObserved` diagnostic, but it must not be represented as that
handler's state transition.

`setConflictCallbackTask()` already provides an asynchronous conflict hook,
but its current contract only reports conflict entry and its callback type is
private. It is not sufficient by itself to keep a remote proxy's
`isInConflict()` state synchronized through repair. The conflict callback runs
on the handler's async context and must not synchronously wait on that same
handler context or close the handler from within the callback.

### 4.3 Conflict resolution and delivery observer

DMesg can clear a handler's conflict state when it accepts a suitable newer
message, processes a force message, executes `resolveConflict()`, or performs
the publisher-level last-message reset. In several notify paths, force and
playback update counters/clear conflict but are deliberately not delivered
through the ordinary handler read/callback path. The code had no
conflict-resolution callback carrying the state transition and any repair
value to a D-Bus proxy. Phase 1 adds the following observer seam; it does not
implement the D-Bus proxy itself.

**Implemented DMesg seam:** `Dmn_DMesg::HandlerSpec` accepts an optional
`m_handler_event_fn`. Both `openHandler(HandlerSpec)` and
`openHandlerWithFactory()` install it before handler registration or initial
playback. The factory overload assigns it after the factory returns. This
leaves existing handler constructors and user conflict callbacks
source-compatible. The public event shape is:

```cpp
enum class Dmn_DMesg::HandlerEventType {
  kMessage,
  kConflictEntered,
  kConflictResolved,
};

struct Dmn_DMesg::HandlerEvent {
  HandlerEventType m_type;
  std::string m_topic;
  std::uint64_t m_handler_running_counter;
  std::uint64_t m_conflict_generation;
  std::optional<DMesgPb> m_message;
};

using HandlerEventCallbackTask =
    std::function<void(const HandlerEvent &)>;
```

The event is created in the serialized publisher context and its callback is
queued to the handler's async context in publisher event order. It must only
enqueue bounded work; it must not block, call D-Bus, wait, perform network I/O,
or synchronously query or re-enter the same handler or publisher. Callback
exceptions are caught and reported without interrupting publisher processing.
The handler filter remains synchronous in publisher context because it gates
delivery. The contract is:

- Emit an ordinary message event for each eligible handler delivery. Emit a
  conflict event only on a real state transition (not for every repeated
  conflict message).
- Emit transition events after the handler's conflict set/counter has been
  updated.
- Configure the observer through `HandlerSpec` before registration; it cannot
  be replaced on a live handler.
- Create events at normal handler delivery and at the actual state transition
  in the serialized publisher context for publisher-mediated writes,
  notifications, force/reset repair, and explicit handler resolution; queue
  callbacks to the handler context in that order. `AsyncProcessTask`, the event
  observer, and the legacy conflict callback all execute in handler context.
  The gateway must not expose handler-only `resolveConflict()` in v1; it clears
  state only through observable publisher-mediated repair.
- Preserve transition order per handler; attach a monotonically increasing
  per-handler conflict generation before queuing the observer callback.
  Increment it once for each actual topic conflict-state transition; ordinary
  message events do not change it. A clear-all operation emits one resolved
  event for each topic that was actually conflicted.
- For message events, include the delivered `DMesgPb`. For conflict entry,
  include the conflict-causing message when available.
- For resolution, include the repair/accepted message when available. A
  clear-only `resolveConflict()` has no repair value and must be identified as
  such; the gateway must not claim the client has received a replacement
  value.
- Keep existing `setConflictCallbackTask()` behavior source-compatible; the
  gateway observer supplements rather than replaces that callback. The
  legacy callback remains scheduled for each conflict notification, while the
  observer emits only on conflict entry.
- The gateway callback owns queue-overflow accounting and must expose event
  loss as `ResyncRequired`. An observer exception is logged but cannot itself
  identify gateway queue state.

For the gateway v1 surface, cover every reachable state mutation path:
conflict insertion, accepted normal-message clearing, and forced-message
clearing through `resetConflictStateWithLastTopicMessage()`. Do not expose
handler-only `resolveConflict()` or client force writes through the gateway.
Because the observer is a public DMesg API, every state-changing method,
including explicit handler resolution, emits a transition event. Avoid
duplicating DMesg's conflict algorithm in the gateway.

## 5. Required gateway event and ordering contract

The daemon translates each ordered handler message or state transition into a
directed event for the client connection owning that server handler. Minimum
event fields:

| Field | Meaning |
|---|---|
| `handler_id` | Server-issued routing identity, bound to and validated against the current D-Bus peer. |
| `event_sequence` | Strictly increasing per handler, assigned by the daemon before queue admission. |
| `event_type` | `Message`, `ConflictEntered`, `ConflictResolved`, or an explicit gateway control/error event; conflict state is never inferred from message flags alone. |
| `topic` | Topic for the delivery or handler-local state transition. |
| `handler_running_counter` | Counter snapshot at delivery/transition; diagnostic/local handler state only. |
| `conflict_generation` | Current monotonically increasing per-handler state-transition generation; meaningful for state transitions. The method reply and status snapshot report the current generation too. |
| `message` | Validated application delivery, optional conflict cause or repair value, or absent for a control/error event. |
| `delivery_status` | Local queue/send/drop state; must not imply application consumption. |

Conflict events and ordinary DMesg delivery events for one handler share one
ordered event stream. DMesg creates events on the publisher-serialized path
and queues observer invocations to the handler context in that order. The
observer assigns one per-handler event sequence before events enter the
bounded queue. `AsyncProcessTask`, the observer, and the legacy conflict
callback all execute on the handler context; the synchronous handler filter is
the only application callback on the publisher context. The D-Bus worker sends
events in `event_sequence` order; the client rejects duplicates, buffers a
bounded out-of-order event if the transport permits it, and reports sequence
gaps. The local D-Bus transport does not itself guarantee the proxy consumed
an event.

The handler-context callbacks may only copy bounded events into the gateway's
per-handler event queue. They must not make a synchronous D-Bus call, wait for
the client, call blocking network I/O, synchronously query or re-enter the same
handler, or close it from within the callback.

Closing a handler first marks its event route as closing under the same
synchronization used for queue admission, then unregisters the DMesg handler.
Unregistration does not cancel handler-context callbacks already queued or in
flight; those callbacks must safely observe the closed route and discard or
account for late events without touching freed client state. Stop the event
sender and synchronize in-flight sends before releasing the route and its
connection-owned queue.

If a `Publish` result and a `ConflictEntered` event race at the client, the
method reply reports only the result of that call. It also carries the
server-side conflict generation observed when the reply is formed. Transition
events remain authoritative for ordered proxy-state changes and carry their
own event sequence and conflict generation. The client ignores an older
generation and must not clear a later conflict because an earlier success
reply arrives late. Operation IDs correlate method calls and replies only;
they do not identify causal events in v1.

## 6. Two-client conflict walk-through

Let applications A and B each open a write/subscription handler for topic
`orders`. The daemon creates server handlers `H_A` and `H_B`; the generated
names never leave the daemon. Initial playback synchronizes each handler's
counter to the publisher's current topic value.

### 6.1 Conflict entry

1. `H_B` has a topic counter behind the publisher's next counter. Competing
   writes can create this stale-counter condition; deterministic tests arrange
   the counter/schedule explicitly and do not assume arbitrary concurrent
   calls must conflict.
2. B publishes through the stale `H_B` using
   `writeAndCheckConflict()`. The publisher marks that write conflicted and
   the source handler enters conflict.
3. The source handler enters conflict. Any other subscribed handler with an
   established topic counter can also enter conflict when it processes the
   conflict notification. A handler with no prior counter does not enter
   conflict under current DMesg behavior.
4. The handler-event observer on each affected server handler enqueues a
   `ConflictEntered` event addressed to that handler's owning client. A and B
   may both be notified, but only if each respective handler actually changed
   to conflicted.
5. Each `Publish` reply reports its local result: accepted,
   conflicted/already-conflicted, or rejected before publication. It says
   nothing about remote hosts.
6. The proxy marks only the identified handler/topic conflicted. Its later
   normal writes fail locally with a clear conflict status until resolution.

When multiple writes race, the accepted result depends on the serialized
publisher queue order. The gateway must not promise that request arrival time,
D-Bus sender, or wall-clock timestamp wins.

### 6.2 Conflict resolution

1. A client may request topic resynchronization only if service policy allows
   it. The daemon uses the existing publisher-level
   `resetConflictStateWithLastTopicMessage(topic)` path; it does not accept a
   client-authored `force` or `playback` message.
2. The cached value is republished through DMesg's force/reset path. Local
   handlers update their counters and clear conflict state according to the
   existing DMesg semantics.
3. Each handler whose conflict state actually clears emits a
   `ConflictResolved` transition. The event carries the cached repair message
   if the observer can provide it, plus the updated local counter.
4. The gateway routes that event only to the corresponding client handler.
   It is not a global repair acknowledgement: another host may not receive it,
   may retain a conflicting value, or may independently select another
   cached value.
5. A late `ConflictEntered` event cannot overwrite a later
   `ConflictResolved` event because the client orders them by
   `event_sequence`.

If there is no cached value, the resync request returns an explicit
`NoCachedValue` result and does not clear conflict state. If the local
publisher's cached value is not authoritative for the application, the client
must use an application merge/repair operation rather than treating this path
as arbitration.

### 6.3 Remote conflict after network integration

When the daemon later owns one `Dmn_DMesgNet`, inbound remote messages enter
that same publisher. Any server-side handler conflict transitions caused by
that publisher use the same observer and per-client event stream. The local
event does not assert who caused a remote conflict; source metadata remains
untrusted unless the inter-node transport independently authenticates it.

The gateway event tells a local application what its own handler state did.
It does not relay node-wide master state as a decision, nor does it imply that
the remote writer was notified, the remote publisher rolled back, or all
nodes selected the same repair.

## 7. Client proxy state machine and errors

For each `(handler_id, topic)`, the client proxy tracks:

```text
Unknown -- initial counter/status --> InSync
InSync -- ConflictEntered ---------> InConflict
InConflict -- ConflictResolved ----> InSync
any live state -- bus disconnect --> Disconnected/Unknown
```

The proxy also records the last applied event sequence. On reconnect, it must
not silently assume its previous conflict/counter state is current. The first
slice may require handlers to be reopened and initial playback repeated; it
does not require durable events or a reconnect cursor.

Method error/result categories must keep these outcomes separate:

- malformed/unauthorized/unsupported application message;
- per-client or global ingress queue full;
- accepted by the daemon's local DMesg publisher;
- conflict on this server-side handler;
- handler already in conflict and rejected before publish;
- no cached value for a resync;
- operation ID correlation in the current call only (not retry deduplication);
- handler closed or D-Bus service disconnected;
- asynchronous event queue overflow or D-Bus send failure.

If an event queue overflows, the gateway must not silently drop a conflict
transition while continuing to report that the client proxy is synchronized.
Mark that handler stream as requiring resynchronization, stop or coalesce
ordinary message delivery only under an explicitly specified policy, and
surface a queryable `EventsLost`/`ResyncRequired` outcome. A conflict-entered
and conflict-resolved pair may not be coalesced away unless a later authoritative
snapshot includes the final state and counter. Because a full queue cannot
reliably carry its own overflow event, expose this state through the handler
status/reply path and require the client to reopen or obtain a current snapshot
before treating the stream as synchronized again.

## 8. Security, isolation, and resource bounds

- Bind every method and event to the authenticated local D-Bus sender; never
  authorize by client-supplied handler ID alone.
- Restrict topic read/write permissions by the daemon's policy. D-Bus peer
  authentication is not topic authorization.
- Reject client-authored `sys`, `conflict`, `force`, `playback`, counters,
  timestamps, server source-write-handler IDs, and node/master claims.
- Return sanitized application data and gateway conflict metadata; do not
  expose other client's opaque handler IDs or private credentials.
- Bound message size, pending Publish requests, per-handler event count/bytes,
  total per-client memory, and handler count.
- Never block the shared D-Bus dispatch worker or DMesg publisher callback on
  a slow client. Queue overflow must be visible.
- A client disconnect closes its handlers and frees connection-owned resources.
  It does not undo an already accepted DMesg publication or any remote effect.
- The system bus needs a separately reviewed least-privilege policy. Tests use
  only a private bus.

## 9. Implementation plan and unit/integration tests

Implement in narrow phases; do not begin cross-host behavior until the
two-client local conflict contract is deterministic.

### Phase 0 — Freeze behavior and test seams

- Specify the handler-event observer's public types, callback execution
  context, ordering, repeated-conflict behavior, repair payload, and exception
  handling.
- Specify D-Bus operation signatures, application message encoding,
  authorization, limits, event ordering, and error/result names.
- Keep current `Dmn_DMesgDbus` signal facade unchanged.

**Exit:** A review can trace every `m_topic_in_conflict` insert/erase/clear
through one observer contract; no path silently loses a transition.

### Phase 1 — Add and test the DMesg conflict-state observer (complete)

The additive `Dmn_DMesg::HandlerEvent` and callback are carried in
`HandlerSpec`; both HandlerSpec-based open paths install the callback before
registration and initial playback. Each actual topic conflict-state
transition increments the handler's generation once. Ordinary eligible
deliveries share the same serialized callback path. Observer exceptions are
logged and isolated from publisher processing; queue-overflow accounting
remains the gateway callback's responsibility. The legacy conflict callback
is unchanged.

`test/dmn-test-dmesg-conflict.cpp` covers stale local write notification to
the source and another subscribed handler, topic/cause/generation snapshots,
force/reset resolution with the cached repair message, clear-only explicit
resolution, a second writer's repeated conflict without duplicate observer
transition, initial playback ordered before a subsequent live delivery, and a
newer accepted message resolving before its delivery event. A forced handler
write also resolves with its submitted repair message. A throwing observer is
isolated while delivery continues. The existing test in the same target covers
topic-counter eligibility and legacy conflict callback attribution.

Follow-on direct `Dmn_DMesg` coverage: clear-all with multiple conflicted
topics emits one resolved transition per topic.

**Exit:** A direct unit test can observe exact entry and resolution transitions
without sleeps or D-Bus.

### Phase 2 — In-process gateway connection core

Implement connection-to-handler ownership, per-handler event sequencing,
bounded event queues, activation handshake, and disconnect/close cleanup
behind a transport-independent internal interface.

Required tests:

- Two fake clients create distinct server handlers on one publisher.
- Initial playback is staged, then activation delivers it before live events.
- A write from A reaches B's subscribed handler once with server-stamped
  identity/counter; A does not receive its own normal write.
- Force and playback control fields from clients are rejected.
- A timed-out Publish is reported as unknown; the client does not retry it
  automatically and can query the handler's current state.
- Per-handler conflict entry is routed only to affected client handler IDs.
- Conflict resolve updates proxy state only after its ordered event; event
  sequence is monotonic and per-handler.
- Queue overflow reports `ResyncRequired`; no client remains marked in-sync
  after a lost state transition.
- Close racing with queued conflict/delivery events does not use freed client
  state or deliver to a reused handler ID.

**Exit:** Conflict events are correctly routed without D-Bus or thread sleeps.

### Phase 3 — Private-bus client/daemon integration

Implement the D-Bus service and `Dmn_DMesgDbusClient` with distinct request,
dispatch, and event paths. Use a private `dbus-daemon` fixture.

Required tests:

- Two separate client connections open handlers, publish, subscribe, and
  receive messages through one daemon publisher.
- Controlled stale writes produce the expected `Publish` result and
  `ConflictEntered` events for each actually affected handler.
- `isInConflict()` in each client proxy changes only after its matching event.
- A resync request republishes the daemon cache; each affected proxy receives
  ordered `ConflictResolved` with the repair value and updated counter.
- A conflict in A does not mark unrelated topics/handlers or clients in
  conflict; topic-filtered handlers follow current DMesg behavior.
- Event-before-reply and reply-before-event schedules converge to the same
  proxy state using conflict generations and event sequence.
- D-Bus event send/queue failure and slow client do not block DMesg publisher;
  the affected stream reports `ResyncRequired`.
- Unauthenticated caller, foreign handler ID, unauthorized
  topic, malformed protobuf, invalid field combinations, oversized message,
  and unsupported operations are rejected.
- Client disconnect closes handlers; daemon shutdown disconnects clients and
  joins dispatch/event workers before destroying publisher state.
- Multiple independent test runs never use the developer's session/system bus.

**Exit:** The two-client local IPC contract passes with no inter-node
transport configured.

### Phase 4 — Attach the one daemon node to `Dmn_DMesgNet`

Construct one network adapter pair and one daemon-owned `Dmn_DMesgNet`; pass
the same object as the D-Bus service's `Dmn_DMesg` publisher. Do not add a
second D-Bus-backed node or bridge handler-to-handler messages between
publishers.

Required tests with controllable fake I/O:

- Local Publish uses the same publisher and is sent to network output once.
- Remote input is delivered to matching local client handlers and not echoed
  back through DMesgNet due to the existing source-write-handler suppression.
- A network-originated conflict triggers handler-scoped local conflict
  events; repair triggers resolution events with correct local counter.
- Lost, duplicate, stale, or reordered network messages do not cause the
  gateway to assert remote consensus or suppress required conflict events.
- Input shutdown wakes the network reader before final Destroyed output; local
  D-Bus service stops new calls and drains/closes handlers in safe order.
- Two daemon instances connected by deterministic fake transport produce two
  node identities total, independent of local client count.

**Exit:** Both local IPC and node-to-node dissemination use one `Dmn_DMesgNet`
publisher per daemon; the service reports only local acceptance and observed
transport/DMesg state.

### Phase 5 — Qualification and rollout

- Add real inter-node transport tests separately from deterministic core tests.
- Review transport authentication, node ID/incarnation handling, topic ACLs,
  connection security, system-bus policy, resource limits, and overload behavior.
- Document that DMesgNet's current conflict/playback/master rules are
  best-effort state synchronization and not consensus.
- Keep current direct `Dmn_DMesgDbus` users supported as independent-node
  participants; introduce the client proxy as a separate API/build component.

**Exit:** No release claim implies cross-host exactly-once delivery,
global conflict arbitration, acknowledged application, or consensus.

## 10. Related specifications

- [Per-host daemon gateway architecture](dmesgnet-dbus-node-gateway-spec.md)
- [Current DMesg handler and protobuf behavior](dmesg-spec.md)
- [Current DMesgNet behavior and limitations](dmn-dmesgnet-spec.md)
- [Current D-Bus byte-signal endpoints and direct participant facade](dmesg-dbus-spec.md)
