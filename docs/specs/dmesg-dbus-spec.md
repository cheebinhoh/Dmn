# Shared Design: DMesg transport over the Linux D-Bus message bus

**Status:** Shared transport contract for two alternative APIs; no production
implementation is included.
The wire primitive was prototyped against a private `dbus-daemon`: a broadcast
signal carried a binary `ay` payload byte-for-byte, including NUL and high-bit
bytes. This confirms the D-Bus mechanism, not production readiness, Dmn
integration, or multi-host behavior.

The two construction alternatives and their implementation order are
specified in
[`dmesgnet-dbus-injection-spec.md`](dmesgnet-dbus-injection-spec.md) and
[`dmesgnet-dbus-facade-spec.md`](dmesgnet-dbus-facade-spec.md). Both are
required: first implement and validate the direct-injection transport (Option
A), then build the facade (Option B) as a composition wrapper over that tested
path. They use the same signal, endpoint implementation, limits, security
model, and host-local scope; only the public construction/API boundary differs.

## 1. Decision and intended meaning

The design is **DMesg carried over D-Bus**, not D-Bus method calls carried over
DMesgNet. The two alternatives are:

1. **Direct I/O injection:** callers create D-Bus-backed
   `Dmn_Io<std::string>` input/output endpoints and pass them to
   `Dmn_DMesgNet(node, input, output)`.
2. **Facade wrapper:** callers construct `Dmn_DMesgDbus(node, config)`;
   the wrapper creates those same endpoints, owns an internal
   `Dmn_DMesgNet`, and forwards a deliberately selected set of application
   messaging APIs. It does not inherit from or expose the internal
   `Dmn_DMesgNet`.

Do not call either design `Dmn_DBusGateway`: that would describe the different
API-gateway pattern in which applications explicitly tunnel individual D-Bus
method calls.

The proposed component reuses `Dmn_DMesgNet`'s existing DMesg serialization,
membership, and local publisher behavior, but supplies D-Bus-backed
`Dmn_Io<std::string>` endpoints. Its first scope is **inter-process
communication on one D-Bus daemon**. The default Linux session and system
buses are local to a host. D-Bus does not federate separate host daemons, and
this adapter does not make the existing DMesgNet election a consensus
protocol.

```text
process A                                      process B
Dmn_DMesgDbus facade                           Dmn_DMesgDbus facade
  Dmn_DMesgNet (private)                         Dmn_DMesgNet (private)
  D-Bus output endpoint                          D-Bus input endpoint
       |                                              ^
       +---- org.dmn.DMesg1.Transport.Message(ay) ---+
                           |
                    local dbus-daemon
                    routes matching signals
```

Each participating process serializes its normal `DMesgPb` envelope to bytes
and emits one fixed D-Bus signal containing those bytes. Each other Dmn
instance subscribed to that signal reads the byte array and gives the string
to its existing `Dmn_DMesgNet` input path. D-Bus handles local
inter-process routing; Dmn retains its existing message envelope, counters,
topic logic, and heartbeat behavior.

## 2. What this provides—and what it does not

### V1 provides

- A reusable D-Bus-backed `Dmn_Io<std::string>` input/output implementation
  that can be supplied to `Dmn_DMesgNet`.
- Local same-daemon, one-to-many delivery of serialized DMesg messages using
  standard D-Bus signal routing.
- Existing Dmn application handlers and DMesg protobuf topics across separate
  processes, without each application implementing its own D-Bus methods,
  object paths, match rules, and serialization.
- D-Bus daemon authentication and policy for admission to the local bus,
  subject to the administrator's bus configuration.

### V1 does not provide

- Cross-host communication between independent D-Bus daemons, a cluster-wide
  D-Bus name namespace, or a transparent replacement for the standard bus.
- An exactly-once, durable, ordered, or acknowledged transport. Signal
  publication has no per-receiver acknowledgement.
- A quorum, consensus, global commit, partition-safe unique-master guarantee,
  or exactly-once state mutation. Current `Dmn_DMesgNet` has none of these.
- D-Bus method-call tunnelling, service activation on another host, portable
  D-Bus unique names, or export of a process's local D-Bus connection.
- Permission to bypass local D-Bus security policy. Bus policy and transport
  admission are necessary security controls, not proof of application-level
  authorization for every DMesg topic.

If the requirement is that applications on different machines share a
DMesg-derived state, use a separately configured inter-host transport through
`Dmn_DMesgNet` (for example its existing socket or Kafka adapters). A local
D-Bus adapter can coexist with that transport as a local IPC boundary, but
does not replace it. A single centrally hosted D-Bus daemon reachable over a
network is a different, centralized deployment with its own availability,
authentication, and policy risks; it is not the default v1 architecture.

## 3. Repository fit

The current code supports the adapter approach without changing DMesg message
schemas:

- `Dmn_DMesgNet` already accepts `std::shared_ptr<Dmn_Io<std::string>>` for
  both input and output and parses/serializes complete `DMesgPb` strings.
- `Dmn_Io<T>` already supplies the required `read()`, `write()`, and
  `shutdown()` seam. It has no delivery acknowledgement, source identity, or
  mandated concurrency behavior.
- `Dmn_DMesg` already owns local process-level publication, handlers,
  per-topic counters, and playback. D-Bus is only an inter-process carrier.
- `DMesgPb` includes its own topic, source, counter, type, and body. No D-Bus
  payload field or new protobuf envelope is needed: the D-Bus `ay` contains
  the serialized bytes exactly.
- `Dmn_DMesgNet` runs a blocking input worker and shuts its input adapter down
  before the final outbound Destroyed heartbeat. The D-Bus input and output
  endpoints therefore must be separate adapter objects/connections: shutting
  down the input side must not disable the output side before that final
  heartbeat.
- DMesgNet's source-write-handler marker suppresses a node's own transport
  echo. Stable node identifiers still must be unique per `Dmn_DMesgNet`
  instance on the shared bus.

Do not modify `DMesgBodyPb`, `DMesgPb`, or introduce a D-Bus-specific
application payload. This component transports the existing DMesg wire format.

### Construction alternatives

The direct injection contract is in
[`dmesgnet-dbus-injection-spec.md`](dmesgnet-dbus-injection-spec.md). The
facade wrapper API and lifecycle contract is in
[`dmesgnet-dbus-facade-spec.md`](dmesgnet-dbus-facade-spec.md). Both must use
the following common transport requirements. Option A and all its required
unit/integration tests are the first implementation milestone; Option B begins
only after that milestone passes and reuses its endpoint implementation.

## 4. Common transport contract

### 4.1 Fixed D-Bus signal

Use a single versioned broadcast signal:

```text
Object path:  /org/dmn/DMesg1
Interface:    org.dmn.DMesg1.Transport
Member:       Message
Signature:    ay
```

The one argument is the complete serialized `DMesgPb` byte sequence. Use a
signal rather than a D-Bus method call because DMesg publication is
one-to-many and `Dmn_Io::write()` returns no remote reply. A per-message
method call would invent a delivery-acknowledgement meaning that the current
Dmn API cannot uphold and would serialize each publication through a
request/reply exchange.

No well-known service name is required to emit or receive the signal. This
avoids a singleton-name ownership race between ordinary Dmn participant
processes. Each input connection installs one exact match rule for signal
type, object path, interface, and member; it must wait until the daemon has
accepted the match before the endpoint reports ready. No wildcard body/path
match or monitor/eavesdrop permission is required.

The D-Bus daemon's sender unique name is connection-scoped and ephemeral.
The D-Bus adapter must not rewrite `DMesgPb.sourceIdentifier` or treat the
D-Bus sender as a persistent Dmn node identity. Dmn node identity remains
configuration and the existing DMesg protocol; the D-Bus sender is useful only
for local bus policy/diagnostics.

### 4.2 Input endpoint contract

- `read()` blocks until a matching signal payload has been queued, shutdown
  has been requested, or a terminal I/O error occurs. It returns an engaged
  string, including for a valid zero-length payload (which `Dmn_DMesgNet` will
  reject as malformed). It drains already queued values, then throws
  `std::system_error(std::errc::operation_canceled)` after explicit shutdown.
  On bus failure it drains already queued values and then throws
  `std::system_error` with the actual connection error code; do not disguise
  disconnection as normal shutdown.
- The D-Bus dispatch callback must not block waiting for the DMesgNet consumer.
  It copies the byte array into a bounded queue and returns promptly.
- The input queue is finite. On overflow, drop the newest payload, increment a
  visible drop/error counter, and emit a rate-limited diagnostic using the
  existing `Dmn_DMesgNet` stderr-reporting convention. Do not block the shared
  D-Bus dispatch thread or silently claim delivery. Enforce both a
  message-count cap and total queued-byte cap. DMesgNet's
  existing counter/conflict behavior may detect some resulting gaps but is
  not a reliable delivery recovery protocol.
- Reject a byte array above `max_message_bytes` before copying it into the
  queue. Count and report malformed signal signatures and oversized payloads.
- `shutdown()` is idempotent, removes the match/filter, wakes `read()` with
  the explicit cancellation outcome above, and joins its dispatch activity
  before releasing its D-Bus connection. Once
  shutdown begins, no callback may access the destroyed adapter.

### 4.3 Output endpoint contract

- `write(const std::string&)` accepts one item into a bounded writer queue.
  The worker submits exactly one `Message(ay)` signal with identical bytes;
  embedded NUL bytes are valid and must not be treated as a C-string
  terminator.
- Enforce `max_message_bytes` before queue admission. Queue-full, shutdown,
  and already-known terminal worker failures throw `std::system_error`;
  oversized writes throw `std::system_error` with
  `std::errc::message_size`. Do not silently fragment.
- A successful write means only that the local endpoint queue accepted the
  message. It does not mean libdbus or the daemon sent it, or any subscriber
  received, deserialized, or applied it.
- After acceptance, worker-side libdbus/send failures cannot be returned by
  the original `write()`. They must transition the endpoint to failed state,
  increment an observable error count, and produce a rate-limited diagnostic
  using the existing `Dmn_DMesgNet` stderr-reporting convention. Later writes
  must fail synchronously.
- `dbus_connection_send()` admits a message to libdbus's outgoing queue; it
  does not itself write the message to the bus. The worker must use a
  bounded read/write/dispatch loop (for example,
  `dbus_connection_read_write_dispatch()` with a finite timeout) or
  non-blocking watch handling, not
  `dbus_connection_flush()`, which blocks until the entire outgoing queue is
  empty. Bound application-queued bytes and stop feeding libdbus while its
  outgoing byte count is at `max_queued_bytes` plus at most one
  `max_message_bytes` item.
- On shutdown, stop admission and attempt to submit application-queued
  messages until a finite deadline (proposed default: 1 second). At deadline,
  report the remaining application-queue count/bytes and close the connection.
  Messages already accepted into libdbus may still be discarded on close;
  this is best effort and not a remote-delivery guarantee. The endpoint
  destructor must not wait indefinitely for a blocked flush.
- `shutdown()` is idempotent and closes only the output endpoint. It must
  remain usable for the final DMesgNet Destroyed heartbeat until the
  `Dmn_DMesgNet` destructor itself finishes.

### 4.4 Threading and lifecycle

Use one private D-Bus connection for the input/dispatch side and a separate
private connection for output. Initialize libdbus threading once before any
libdbus use in the process; if initialization fails, endpoint construction
fails explicitly. One input worker owns dispatch for its connection. One
output worker owns send, watch/read-write, timeout, and disconnect processing
for its connection. Do not call libdbus while holding an endpoint queue mutex.

Expose a thread-safe, read-only `Dmn_DMesgDbusIoStatus` endpoint status
snapshot with these proposed fields:

| Field | Meaning |
|---|---|
| `messages_received` | Valid signal payloads admitted to the input queue. |
| `messages_written` | Items accepted by the output queue. |
| `messages_queued_to_libdbus` | Items accepted by `dbus_connection_send()` into libdbus's outgoing queue; not bytes written to the bus. |
| `malformed_received` | Matching signals rejected for malformed body/signature. |
| `oversized_received` | Matching signals rejected for exceeding the payload limit. |
| `input_allocation_errors` | Valid matched signals that could not be copied/enqueued due to allocation failure. |
| `input_queue_drops` | Valid matching signals dropped because the input queue was full. |
| `output_queue_rejections` | Writes rejected because the output queue's message-count or byte cap was full. |
| `output_worker_errors` | Worker-level libdbus/send/disconnect failures; not a count of messages lost inside libdbus. |
| `pending_output_messages` | Items waiting in the adapter's bounded output queue, excluding libdbus's internal queue. |
| `pending_output_bytes` | Payload bytes waiting in the adapter's bounded output queue. |
| `libdbus_outgoing_bytes` | Bytes reported by libdbus as pending output; not a per-message delivery count. |
| `shutdown_unsent_messages` | Adapter-queued messages not submitted before the finite shutdown deadline. |
| `shutdown_unsent_bytes` | Payload bytes discarded from the adapter queue at shutdown deadline. |
| `libdbus_bytes_discarded_on_shutdown` | Pending libdbus output bytes observed immediately before connection close; not a message count. |
| `terminal_error` | Optional first terminal I/O error, retained until endpoint destruction. |

Counters and shutdown totals reset only at construction; pending queue values
change during operation. Reads are thread-safe. Status does not assert peer
receipt. V1 uses the repository's
existing stderr diagnostic convention rather than adding a user callback
whose blocking, reentrancy, and exception behavior would need a separate
contract. Direct-injection users must be able to query endpoint snapshots;
the facade must expose both snapshots through a read-only status API.

Each concrete endpoint destructor must call idempotent `shutdown()` explicitly
while its derived type is active; the base `Dmn_Io<T>` destructor cannot
dispatch to the derived override. On teardown, follow the existing
`Dmn_DMesgNet` lifecycle: its destructor
shuts down input and unblocks `read()`, then sends its final system message
through the still-open output endpoint. Do not share one adapter object for
both input and output; input shutdown must not close output before the final
heartbeat. For Option B, declare endpoint owners before the internal
`Dmn_DMesgNet` member so reverse member destruction destroys the node first.

## 5. Bus policy and security

- Use libdbus public APIs; do not depend on private `dbus-daemon` internals.
- The bus performs local connection authentication (typically EXTERNAL on
  Unix sockets) and applies its configured send/receive policy. Supply example
  least-privilege policy for the exact interface/path/signal; do not recommend
  opening unrestricted system-bus access.
- Treat every signal body as untrusted, even after local bus authentication.
  Validate size before allocation/copy. DMesgNet currently has no general
  authenticated peer identity or complete validation of remote DMesg claims.
- Never execute D-Bus methods based solely on an inbound DMesg payload. V1
  carries DMesg data only; it is not a generic D-Bus method tunnel.
- Do not interpret sender unique names, claimed `sourceIdentifier`, node
  lists, or master identifiers as credentials.
- The session bus may permit arbitrary same-user peers. Application-level
  topic authorization, if required, must be separately defined and tested.
- Messages can be observed by all bus clients authorized by the daemon's
  signal policy. Do not transport secrets or privileged control data without
  an approved access-control model.

## 6. Delivery, ordering, and resource limits

D-Bus broadcast signal routing is asynchronous best effort from the
application's perspective. There is no per-subscriber ACK, replay log,
exactly-once guarantee, or delivery ordering guarantee across independent
sender connections. A single connection's messages may be ordered by that
connection and daemon processing, but v1 must not promise a total order across
publishers.

V1 default limits are proposed, not yet implementation constants:

| Limit | Proposed default | Required behavior |
|---|---:|---|
| Serialized `DMesgPb` payload | 1 MiB | Reject before send/copy. |
| Input queue | 1,024 messages or 16 MiB, whichever is reached first | Drop newest on full; increment counter and rate-limit diagnostics. |
| D-Bus connection count | 2 per Dmn_DMesgNet instance | Separate input and output lifetimes. |
| Output queue | 1,024 messages or 16 MiB, whichever is reached first | Reject new writes synchronously on full; no silent loss. |
| libdbus outgoing queue | 16 MiB plus at most one maximum-size message | Stop submitting while the outgoing byte cap is reached. |
| Worker I/O wait | At most 50 ms per loop iteration (proposed internal bound) | Let shutdown/disconnect state be observed promptly; do not expose as config in v1. |
| Shutdown drain | 1 second | Stop submitting at deadline and report unsent queue count/bytes and pending libdbus bytes. |
| Topic/message rate | No global default yet | D-Bus daemon limits remain; add app-level rate limiting only with a defined requirement. |

The transport wire limit must include D-Bus framing overhead and the
`Dmn_DMesgNet` protobuf. If deployed daemon limits are lower than the proposed
payload cap, configuration must be reduced at startup or construction fails;
never silently truncate.

DMesgNet's heartbeat, per-topic counters, conflict handling, and cached
playback remain as they are. They do not turn D-Bus signals into a reliable
transport. In particular, cache playback is DMesg application behavior, not
D-Bus daemon replay. The local D-Bus bus itself does not persist messages for
future subscribers.

## 7. Multi-host boundary and consensus

The default system/session D-Bus instance is a host-local message bus. Two
machines with separate daemons have separate unique-name spaces, ownership
queues, and routing tables. A signal emitted on host A's daemon is not
automatically forwarded to host B's daemon.

D-Bus address syntax can describe transports beyond the default Unix socket,
but configuring clients to reach one shared remote daemon would create a
centralized bus endpoint, not a federation of independent buses. Such a
deployment changes authentication, confidentiality, availability, bus policy,
and failure domains and is not included in this v1 design.

For multi-host DMesg:

1. Keep `Dmn_DMesgNet` on its chosen network transport between nodes.
2. Optionally use a separate local D-Bus adapter/facade to let local
   applications communicate with a host-local Dmn service.
3. Specify any bridge between the local D-Bus DMesg instance and the
   inter-host Dmn_DMesgNet instance as a separate component, with loop
   prevention, identity, authorization, and failure tests.

Neither `Dmn_DMesgDbus` nor existing `Dmn_DMesgNet` supplies consensus.
`Ready`, `masterIdentifier`, heartbeat observations, counters, or signal send
completion are not quorum/commit evidence. A distributed exclusive owner or
consensus-backed application state requires a separately specified and
implemented consensus protocol.

## 8. Compatibility and rollout

- No protobuf schema change is required: the signal carries the existing
  serialized `DMesgPb`.
- Existing in-process `Dmn_DMesg` and network `Dmn_DMesgNet` users are
  unchanged.
- Only instances constructed with the D-Bus endpoints—by direct injection or
  inside `Dmn_DMesgDbus`—attach to the D-Bus signal interface.
- The adapter protocol is versioned by the D-Bus interface name
  (`org.dmn.DMesg1.Transport`). Incompatible transport changes use a new
  interface version; do not reinterpret the existing `ay` payload.
- Mixed deployments with an older participant simply do not receive or
  understand this signal; announce version/availability through separate
  observability rather than silently falling back to another transport.

## 9. Acceptance criteria

The implementation is ready for a same-host, non-privileged pilot only when:

1. The documented fixed signal carries exact `DMesgPb` serialization,
   including embedded NUL and arbitrary byte values.
2. Two private-bus participants exchange independent messages without
   observing their own transport echo.
3. `read()` blocks, wakes on shutdown, never spins on an empty result, and
   safely joins dispatch before object destruction.
4. Output remains available long enough for the base destructor's final
   Destroyed message.
5. Match installation is confirmed before readiness; malformed signatures,
   queue overflow, send failure, and oversize input are observable.
6. The tests use a private/session bus only, never the host's system bus.
7. Documentation and diagnostics make clear that signal send is not remote
   receipt and that the adapter is host-local, best-effort IPC.

System-bus deployment requires a separate least-privilege policy review.
Cross-host use requires a separate transport/federation design. Neither
condition is met merely by passing the same-host acceptance tests.

## 10. Research and prototype references

Official D-Bus references, reviewed 2026-10-04:

- [Message Bus](https://dbus.freedesktop.org/doc/dbus-specification.html#message-bus):
  bus-local names, routing, activation, and connection lifetime.
- [Message Bus Message Routing](https://dbus.freedesktop.org/doc/dbus-specification.html#message-bus-routing):
  broadcast signals, match rules, and policy-controlled delivery.
- [D-Bus addresses](https://dbus.freedesktop.org/doc/dbus-specification.html#addresses):
  address-based connection setup; a non-Unix address does not itself federate
  separate daemons.
- [Authentication](https://dbus.freedesktop.org/doc/dbus-specification.html#auth-protocol):
  connection-level authentication, distinct from DMesg payload identity.
- [Message protocol](https://dbus.freedesktop.org/doc/dbus-specification.html#message-protocol):
  message types, headers, signatures, and body encoding.
- [libdbus message API](https://dbus.freedesktop.org/doc/api/html/group__DBusMessage.html):
  public API for signal construction and byte-array arguments.
- [libdbus connection API](https://dbus.freedesktop.org/doc/api/html/group__DBusConnection.html):
  `dbus_connection_send()` queues output, while `dbus_connection_flush()`
  blocks until that queue is empty. The connection API also provides
  `dbus_connection_read_write_dispatch()` to perform bounded I/O and dispatch;
  the adapter must not flush synchronously in `write()` or teardown.
- Repository contracts: [`Dmn_DMesgNet`](dmn-dmesgnet-spec.md) and
  [`Dmn_Io`/pipelines](io-pipelines-spec.md).

Prototype evidence: a temporary C program using the installed libdbus sent a
private-bus signal with signature `ay`; a separate subscriber verified the
exact five bytes `00 01 7f 80 ff`. The temporary program was deleted after the
run. This is a transport-mechanism experiment, not a checked-in test or
implementation.
