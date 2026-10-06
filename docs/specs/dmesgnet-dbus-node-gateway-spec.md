# Per-Node DMesg Gateway over Local D-Bus

**Status:** Proposed architecture; its local-conflict DMesg handler-event
observer foundation is implemented, but the gateway is not. This specification
defines a per-host daemon that exposes one `Dmn_DMesgNet` node to local
applications over D-Bus and connects that node to other host daemons through a
separately configured inter-node transport.

This is a design for messaging and best-effort state dissemination. It does
not turn `Dmn_DMesg` or the current `Dmn_DMesgNet` master-selection heuristic
into consensus, durable replication, or an acknowledged cluster commit
protocol.

## 1. Purpose and repository fit

The current `Dmn_DMesgDbus` is a composition facade that owns a complete
`Dmn_DMesgNet` participant and a pair of D-Bus broadcast-signal endpoints. It
is suitable for applications that want to participate directly in the
same-host D-Bus DMesg mesh. It is **not** a proxy to a daemon-owned node:
every instance has its own node identity, local publisher, heartbeat state,
topic counters, and conflict state.

The existing D-Bus signal transport is host-local. The existing
`Dmn_DMesgNet` accepts one input and one output `Dmn_Io<std::string>` pair;
it does not itself combine a local bus with a second inter-host network.
Consequently, placing the current `Dmn_DMesgDbus` beside a separate network
`Dmn_DMesgNet` does not create a transparent gateway. A forwarding layer would
need its own identity, loop prevention, counter mapping, and conflict contract.

The recommended design instead uses one `Dmn_DMesgNet` per physical host.
That object is both the host's one cluster participant and the local
`Dmn_DMesg` publisher. A new D-Bus service adapts remote client requests to
server-side DMesg handlers on that same object. Local clients use a
client/proxy API; they do not construct a `Dmn_DMesgNet` and do not join the
cluster as separate nodes.

```text
Host A                                                     Host B
+-------------------------------+                         +-------------------------------+
| app process                   |                         | app process                   |
| Dmn_DMesgDbusClient           |                         | Dmn_DMesgDbusClient           |
|       | local D-Bus RPC/events|                         |       | local D-Bus RPC/events|
|       v                       |                         |       v                       |
| dmn-dmesg-daemon              |                         | dmn-dmesg-daemon              |
| D-Bus service                 |                         | D-Bus service                 |
|       | server-side handlers   |                         |       | server-side handlers   |
|       +------ Dmn_DMesg -------|                         |       +------ Dmn_DMesg -------|
|                    |          |                         |                    |          |
|              Dmn_DMesgNet     |<-- inter-node transport -->| Dmn_DMesgNet     |
|                    |          |                         |                    |          |
+-------------------------------+                         +-------------------------------+
```

The daemon owns the only network-facing DMesgNet identity on its host. Its
server-side handlers are ordinary `Dmn_DMesg` handlers registered with that
node. Therefore a locally submitted client message, a message received from a
remote node, and a client subscription all pass through one publisher and one
per-host topic-counter/cache domain.

## 2. Goals and non-goals

### Goals

- Give local application processes a handler-like DMesg API backed by one
  daemon-owned `Dmn_DMesgNet` node.
- Keep local IPC and inter-host transport separate, independently configurable
  failure domains.
- Represent each physical host as one `Dmn_DMesgNet` member, regardless of
  its number of local applications.
- Preserve current DMesg handler delivery, local conflict indication,
  last-value playback, and DMesgNet forwarding behavior where applicable.
- Make message acceptance, queueing, transport submission, and remote
  application distinct observable outcomes.
- Define how client identity, request outcomes, handler lifetime, conflicts,
  and shutdown cross the D-Bus boundary.

### Non-goals

- Reusing `Dmn_DMesgDbus` as a daemon RPC client without changing its contract.
- Forwarding arbitrary D-Bus method calls across hosts.
- Exposing raw D-Bus connections, `Dmn_DMesgNet`, or daemon handler pointers to
  client processes.
- Exactly-once delivery, durable queues, cluster-wide total ordering,
  linearizable state, or replicated commit acknowledgements.
- Treating `Ready`, `masterIdentifier`, `DMesgPb.runningCounter`, `force`,
  `playback`, or D-Bus method success as proof of consensus.
- Undoing an already accepted publication or revoking its effects because a
  local client disconnected; connection-owned handlers are still closed on
  disconnect.

## 3. Recommended component and ownership model

### 3.1 Host daemon

The daemon owns, in lifetime-safe order:

1. one inter-node input adapter and one inter-node output adapter;
2. one `Dmn_DMesgNet` constructed with those adapters and a stable configured
   host/node identifier;
3. one local D-Bus service that creates and closes server-side
   `Dmn_DMesgHandler` instances on that `Dmn_DMesgNet`;
4. per-client handler ownership, bounded request/delivery queues, and D-Bus
   connection bookkeeping.

The daemon's D-Bus server does not own a second D-Bus-backed `Dmn_DMesgNet`.
The server handlers are registered directly on the network node's inherited
`Dmn_DMesg` publisher. Do not bridge between two publisher instances in the
recommended design.

The daemon executable in `src/dmn-dmesg-daemon.cpp` is currently only a
runtime signal-handling test stub. It is not the proposed production service
and must not be treated as an existing gateway implementation.

### 3.2 Client process

A client uses a new D-Bus client/proxy component, tentatively named
`Dmn_DMesgDbusClient`. It owns its local D-Bus connection and exposes a
deliberately bounded handler API. It does not own a `Dmn_DMesg`,
`Dmn_DMesgNet`, network node identity, or local cluster-election state.

The client may expose local blocking `read()` or callback delivery, but
user-supplied callbacks execute in the client process, never inside the
daemon's serialized DMesg publisher callback. C++ callbacks, lambdas, and
`std::function` values are not serialized over D-Bus. Client filters that
cannot be represented declaratively are applied in the client process.

An opened client handler maps to exactly one server-side handler. The daemon
generates its internal handler name from the authenticated D-Bus connection
and a server-issued handler token; it does not trust a client-provided
`sourceWriteHandlerIdentifier` or permit handler-name collisions.

For the first local-IPC milestone, use the simpler connection-bound model:
handlers belong directly to the authenticated D-Bus unique name until
`CloseHandler` or disconnect. Do not add a separate `OpenSession`/`CloseSession`
layer until a concrete use case requires multiple logical identities on one
D-Bus connection. The detailed first-slice
API and conflict event contract is in
[`dmesg-dbus-local-conflict-spec.md`](dmesg-dbus-local-conflict-spec.md).

### 3.3 D-Bus service boundary

The gateway service uses a distinct, versioned service/interface from the
existing broadcast transport. A proposed v1 namespace is:

```text
Well-known name:  org.dmn.DMesg1.Node
Object path:      /org/dmn/DMesg1/Node
Interface:        org.dmn.DMesg1.Node
Client events:    org.dmn.DMesg1.Client
```

These identifiers are a proposal to freeze before implementation. The
existing `org.dmn.DMesg1.Transport.Message(ay)` broadcast signal and its
`Dmn_DMesgDbus` semantics remain unchanged. Do not use that raw broadcast
signal as the gateway RPC protocol.

For the first local-IPC milestone, the service offers a small, versioned set
of operations:

| Operation | Purpose and required behavior |
|---|---|
| `OpenHandler(topic, options)` | Create one daemon-side DMesg handler. Capture its initial last-value playback and subsequent deliveries into a bounded per-handler queue. Return a server-generated handler ID bound to the caller's D-Bus unique name. |
| `ActivateHandler(handler_id)` | Begin delivering queued initial playback and live notifications after the client has received the handler ID. Preserve queue order and report overflow. |
| `Publish(handler_id, operation_id, application_message)` | Validate and submit one application message through the server-side handler. Return local publisher acceptance/conflict, the current conflict generation, or an explicit error. The operation ID correlates this call with its reply only; v1 does not deduplicate retries or attach the ID to transition events. |
| `RequestTopicResync(handler_id, topic)` | Ask the daemon to republish its cached value using the existing DMesg force/reset path, if policy allows. |
| `GetHandlerStatus(handler_id, topic)` | Return a local handler counter/conflict/generation snapshot for initial state or recovery after event loss. |
| `CloseHandler(handler_id)` | Stop new writes, close the daemon-side handler, and release its queued messages. Closing is not a distributed release or cancellation. |

The daemon sends client events only to the corresponding client's current
unique D-Bus destination. Each event includes the server-issued handler ID
and a per-handler event sequence starting at one. State-transition events
also include the transition type, topic, post-transition counter, and conflict
generation. A message event carries a validated application `DMesgPb`; an
error event has an explicit type and reason. A recipient
must not infer that another local client received or applied the same event.

`OpenHandler` and `ActivateHandler` are separate to avoid losing initial
playback or delivering it before the client knows the handler ID. The daemon
buffers notifications received during this interval, subject to finite bounds.
Activation drains that buffer in sequence order before later live events; if
the buffer limit is exceeded, activation reports an explicit overflow and
requires the client to reopen/resynchronize rather than silently skipping
messages.
`Dmn_DMesg::openHandler()` waits for initial-playback handler-context work
before returning. The service can therefore reply only after those callbacks
have populated its staged queue, but it must perform the blocking open on a
worker rather than the shared D-Bus dispatch thread. `ActivateHandler` remains
necessary because the client must receive its handler ID before the service
starts delivering the staged events.

### 3.4 D-Bus method and delivery meanings

- A successful `Publish` reply means that this daemon accepted the operation
  at the local DMesg publisher, or reports the exact local conflict/rejection
  result. It does not mean that an inter-node adapter submitted the message,
  another daemon received it, or any remote application applied it.
- A successful `OpenHandler` reply means the local handler was created and
  initial playback was staged. It does not guarantee an event will reach a
  disconnected or overloaded client.
- A successful D-Bus send/queued client event does not acknowledge client
  consumption. If an application needs read acknowledgement or replay across
  reconnect, define a separate bounded cursor protocol; do not infer it from
  `Dmn_DMesgHandler::notify()`.
- D-Bus calls are local RPC only. No D-Bus method invocation is forwarded to
  remote nodes.

The first implementation should be explicitly best-effort and must not retry a
timed-out `Publish` automatically. The operation ID matches the call and
reply; it is not an idempotency key and does not correlate transition events.
If the reply is lost, the client treats the publish outcome as unknown,
queries handler status, and does not claim the write was absent or executed
exactly once. Retry deduplication can be added later as a separately bounded
contract if retry support is required.

The D-Bus dispatch callback validates and admits requests to a bounded ingress
queue, then returns to dispatch. A daemon request worker serializes each
handler's writes, performs the DMesg operation, and completes the correlated
method reply. An ingress-queue admission reply, if exposed separately, must be
named as such and must not be reported as DMesg publisher acceptance.

## 4. Message validation, identity, and system traffic

The client API accepts application messages, not arbitrary trusted
`DMesgPb` control frames. Before submitting through a handler, the service
must:

- reject `type == sys`, reserved system topics, missing/empty application
  topics, unsupported message types, and invalid body/type combinations;
- reject client-supplied `conflict`, `force`, and `playback` flags;
- replace timestamp, running counter, and source-write-handler identity using
  the server-side handler path;
- derive source identity from authenticated server policy or a validated
  client principal plus handler identity, not an untrusted payload;
- enforce payload, topic, client, handler, and queue limits before
  allocating unbounded state.

In v1, clients cannot publish, forge, or reconcile `sys` heartbeats, node
lists, master claims, `Destroyed` messages, or Dmn control flags. The daemon's
single `Dmn_DMesgNet` owns its own sys lifecycle and sends that traffic only on
the inter-node transport. A separate read-only status operation may expose
sanitized node status; do not relay internal sys frames as client-owned
application messages.

Authenticate the local bus connection using the bus's peer credentials and
bind every handler ID to the calling unique name. Apply explicit
authorization for permitted topics and operations; bus authentication alone
does not authorize arbitrary topic writes. The session bus is generally a
same-user trust boundary. A system-bus deployment requires a separate
least-privilege policy and must restrict who can own the service name, call
methods, and receive directed events. A payload source string is not a
credential.

For inter-node traffic, configure stable node IDs and an authenticated
transport or trusted deployment perimeter. Current `Dmn_DMesgNet` generic I/O
does not expose authenticated sender identity, validate all remote message
shapes, or protect claimed node IDs. The daemon topology does not fix those
existing protocol gaps. Do not expose an unauthenticated UDP socket as a
production cluster identity boundary.

## 5. Conflict semantics across the layers

The local two-client conflict event contract is specified in
[`dmesg-dbus-local-conflict-spec.md`](dmesg-dbus-local-conflict-spec.md).
There are two distinct DMesg conflict scopes in this design. The local client
is not a third scope: its remote handler state is maintained by the daemon's
`Dmn_DMesg` handler on its behalf. Inter-node transport failures are a separate
delivery-status concern, not a DMesg conflict scope.

| Scope | State owner | Trigger and current behavior | Required gateway behavior | What it does not mean |
|---|---|---|---|---|
| Client handler / host-local publisher | One server-side `Dmn_DMesgHandler` and its daemon publisher | A handler's topic counter is stale or a message carries conflict; the writer handler becomes conflicted. Force/playback can update counters and clear handler conflict state. | Return local publish conflict explicitly; send conflict and later repair/playback events to the corresponding client handler; preserve event order and identify the handler/topic. | No remote node has necessarily seen the write; a conflict is not a global winner or transaction abort certificate. |
| Host DMesgNet / inter-node dissemination | The daemon's one `Dmn_DMesgNet` and its `Dmn_DMesg` publisher | An inbound message is compared with this host's local topic state. The network bridge can mark conflict and the current code may send cached force/playback data when it believes itself master. | Expose the observed local conflict/repair status without upgrading it to global authority. Do not strip, synthesize, or reinterpret control flags at the D-Bus boundary. | The current master heuristic does not prove exclusive authority; playback is a last-value repair, not a committed log or globally correct resolution. |
| Inter-node transport | Adapter and remote daemon | Queue admission, socket/broker failure, message loss, or disconnection. D-Bus output status and adapter writes describe local activity only. | Report local transport errors separately from DMesg conflicts. Distinguish queued/accepted, submitted, and remote-observed states only where a real acknowledgement exists. | A successful local `write()` or D-Bus reply is not remote receipt, publisher acceptance, or commit. |

### 5.1 Client-local write path

1. The client issues `Publish` with a call-correlation operation ID and
   application-only message through one opened remote handler.
2. The daemon validates it and invokes that handler's ordinary DMesg write
   path. The daemon's single publisher serializes competing local writes and
   checks its current per-topic counter.
3. The gateway returns the local result. A conflicted handler remains
   conflicted until its normal force/playback recovery path or an explicitly
   supported local reset operation succeeds.
4. A locally accepted message is eligible for normal `Dmn_DMesgNet` output.
   The response does not wait for an acknowledgement from another node.

The gateway must not retry the handler write internally after a timeout. The
first local-IPC version does not deduplicate retries: a timed-out caller treats
the result as unknown and queries handler status rather than automatically
issuing the same logical write again.

### 5.2 Remote conflict and playback path

1. A daemon's `Dmn_DMesgNet` receives an inter-node message and passes it
   through the existing local publisher and network conflict logic.
2. Its server-side client handlers observe eligible normal/conflict/repair
   notifications and stage them for their owning D-Bus connections.
3. The gateway sends those observations to clients as local handler events.
   The client library updates its proxy-visible counter/conflict state; client
   code may receive the event or query the handler state.
4. A `force`/`playback` value may clear local handler conflict state according
   to existing DMesg semantics. It does not prove that every remote node
   converged or that the selected cached value is globally authoritative.

### 5.3 Multi-writer policy

Per-host daemons collapse multiple local processes into one host publisher;
they do not serialize writes from different hosts into a cluster-wide
authority. Concurrent writers on different hosts can still present competing
topic counters and trigger the existing DMesgNet conflict path. Counter values
are scoped to publisher/handler behavior, can be rewritten by DMesg, and are
not globally unique revisions.

For a first deployment, applications should use one of these explicit topic
policies:

1. **Single-writer ownership:** configure one host/client principal as the
   writer for each mutable topic; other clients subscribe. This reduces
   expected conflicts but does not make automatic failover safe.
2. **Application merge:** allow multiple writers and define a deterministic,
   idempotent merge rule in the application payload. A DMesg force/playback
   event is not that rule.
3. **Consensus-backed authority:** if writes require a unique committed order,
   use a separately specified consensus protocol with authenticated voters,
   durable terms/log, quorum commit, recovery, and client-visible commit
   results. `Dmn_DMesgNet` is only a carrier where its transport contract is
   sufficient; its `Ready`/master state is not the authority.

Do not label the behavior "last writer wins" unless an explicit total-order,
identity, restart, and tie-breaking rule is designed and tested. Wall-clock
timestamps and the current seniority heuristic are not a safe substitute.

## 6. Node identity, restart, membership, and lifetime

- There is exactly one configured DMesgNet node ID per daemon and physical
  host. Local application IDs and D-Bus unique names are not node IDs.
- Node IDs must be unique within the configured cluster. The current protocol
  has no cluster ID or durable incarnation/epoch field; reusing an ID after
  restart can make old messages and `Destroyed` records ambiguous. A
  deployment that requires safe restart handling must add and validate an
  incarnation/version mechanism before claiming that property.
- The daemon's well-known D-Bus name has one active owner on the local bus.
  Losing the service invalidates live client connections/handlers; clients
  must surface disconnection and must not silently fall back to constructing a direct
  `Dmn_DMesgDbus` participant.
- D-Bus unique names are connection-scoped. On `NameOwnerChanged`/disconnect,
  stop accepting requests for that caller, close its DMesg handlers, wake
  local client reads, and account for discarded queued deliveries. This is
  local resource cleanup, not a distributed lease expiry or operation
  cancellation.
- Daemon shutdown stops new client calls, closes client handlers, stops and
  joins the inter-node input worker, then allows `Dmn_DMesgNet` to make its
  best-effort final `Destroyed` write while output remains alive. Keep
  endpoint ownership/destruction order consistent with
  `dmesgnet-dbus-facade-spec.md`.
- No failover between daemon processes may reuse an ID and claim continuity
  unless state transfer, incarnation fencing, and transport identity are
  separately specified. The D-Bus name queue alone does not restore DMesg
  cache, counters, membership, or authority state.

## 7. Alternatives and trade-offs

| Alternative | Description | Advantages | Main limitations | Decision |
|---|---|---|---|---|
| **A. Daemon RPC broker over local D-Bus (recommended)** | One daemon-owned `Dmn_DMesgNet` per host; client proxy calls local methods and receives directed events; server-side DMesg handlers live on the daemon's publisher. | One node identity per host; one counter/conflict domain per node; no raw message relay or second publisher; supports local auth/policy and per-client limits. | Requires a new service protocol/client proxy, connection lifecycle, queueing, and compatibility contract. Daemon is a local availability dependency. | Preferred starting point. |
| **B. Direct `Dmn_DMesgDbus` participants plus a separate bridge** | Each app remains a complete same-host D-Bus DMesgNet participant; an additional daemon observes/forwards traffic to a separate WAN DMesgNet. | Reuses current facade for local IPC; does not require a client proxy API. | Multiple election/member identities per host; system heartbeats may leak across the bridge; two DMesg publishers mean counter/force/playback remapping; loop prevention, origin identity, deduplication, and conflict propagation become a new protocol. Current code has no multi-transport bridge. | Do not implement as transparent forwarding. Consider only if per-process cluster membership is intentional and a versioned bridge protocol is designed. |
| **C. One network-facing `Dmn_DMesgNet` per application** | Every app process connects directly to its own inter-node transport; local D-Bus may still provide same-host peer traffic. | No shared daemon hop; each application is explicitly its own cluster node. | Does not provide one node daemon; multiplies connections, identities, membership entries, bus exposure, and credentials. | Valid only when per-process nodes are the intended product. |
| **D. One D-Bus daemon shared across hosts** | Processes on multiple machines connect to a common remotely reachable message bus. | Superficially reuses the broadcast transport. | Central bus, not federation; expands the trust and failure domain; does not provide per-node daemons or DMesg consensus; changes D-Bus authentication, confidentiality, and availability assumptions. | Not recommended. |
| **E. Use a message broker for inter-node transport** | Keep the recommended local D-Bus RPC service, but connect daemon DMesgNet nodes through Kafka or another broker-backed adapter. | Broker deployment may provide network connectivity, access control, buffering, and operational tooling. | Adapter-specific delivery/group/replay semantics still need qualification; a broker does not make DMesg conflict handling a consensus protocol. Existing Kafka integration has separately documented correctness and security gaps. | Suitable transport alternative after adapter review; it does not change gateway ownership/conflict rules. |

The recommended local D-Bus protocol is method-call based for client
operations and destination-specific signals for asynchronous delivery. A
signal-only raw-byte bridge would let any permitted bus peer inject arbitrary
DMesg control fields and would not provide a clean request/result correlation
or client connection boundary. The gateway protocol must not alter the existing
Option A/B broadcast byte-signal contract.

## 8. Inter-node transport and guarantees

Use one independently configured inter-node `Dmn_Io<std::string>` pair per
daemon-owned `Dmn_DMesgNet`, subject to the transport's actual contract:

- `Dmn_Socket` is IPv4 UDP, unauthenticated, and does not provide reliability,
  peer identity, or interruptible reads as a general standalone adapter.
- `Dmn_DMesgNet_Kafka` is an existing broker adapter, but the Kafka spec's
  configuration, error, credential, and optional-test gaps must be resolved
  before production use.
- A future TLS/TCP or peer-aware adapter may be preferable when authenticated
  node identity, bounded backpressure, delivery acknowledgement, or addressed
  routing is required. The adapter must expose those guarantees explicitly;
  they are not inferred from `Dmn_Io<std::string>`.

The gateway preserves the existing serialized `DMesgPb` network protocol
between daemons. It does not need to tunnel D-Bus service calls or serialize
client callback objects. No cross-host D-Bus unique name, well-known name,
object path, or caller credential is meaningful.

Unless a stronger transport and protocol are separately added, report only:

- local client request accepted/rejected by this daemon's DMesg publisher;
- local D-Bus delivery queued/sent/dropped for a specific client handler;
- inter-node adapter write accepted/submitted/failed according to that
  adapter's status; and
- observed remote DMesg message/conflict events.

Do not expose `Delivered`, `Committed`, or `AppliedEverywhere` as successful
states without an explicit acknowledgement protocol that proves the stated
condition. The current Dmn publisher has no remote-application acknowledgement.

## 9. Resource limits and security requirements

The service must bound, at minimum, maximum application message size,
connected clients per daemon, handlers per client, ingress requests and bytes
per client, staged/live deliveries per handler, and total staged bytes.
Limits must be checked before copying/queue allocation.
Overflow is explicit: reject a new publish request or stop a slow subscriber
with a queryable error; do not block the shared D-Bus dispatch thread or
silently drop data. Per-client limits must prevent one client from starving
other local clients or the DMesgNet input/heartbeat work.

The D-Bus server's dispatch path must not call potentially unbounded network
I/O or block while waiting for remote nodes. Handler processing callbacks
execute on each handler's serialized async context and must only validate
cheaply and enqueue to bounded client delivery queues. The delivery filter
executes synchronously on the DMesg publisher context and must remain cheap.
Dedicated workers or the client's local dispatch path perform D-Bus I/O
outside either callback.

Closing a handler first marks its event route as closing under the same
synchronization used for queue admission, then unregisters the DMesg handler.
Unregistration does not cancel handler-context callbacks already queued or in
flight; callbacks must safely observe the closed route and discard/account for
late events without touching freed client state. Stop the event sender and
synchronize in-flight sends before releasing the route and connection-owned
queue.

Security requirements:

- The daemon validates every client operation and application message, even
  when bus policy restricts callers.
- Bind each handler to the D-Bus sender and bus-authenticated OS credentials;
  reject calls for IDs owned by another sender.
- Apply topic/operation authorization before calling a DMesg handler.
- Do not accept client-authored sys, conflict, force, playback, master, or
  membership control data.
- Use directed client events and avoid broadcasting application payloads to
  unrelated bus clients.
- Use a least-privilege system-bus policy for deployments that require
  cross-user access; private-bus tests alone do not certify it.
- Define rate limits and audit/diagnostic behavior for rejected, malformed,
  unauthorized, oversized, and queue-full requests.

## 10. Implementation sequence and exit gates

Implementation is separate from the current `Dmn_DMesgDbus` facade. A
test-first rollout should use these gates:

1. **Freeze API and threat model.** Specify service identifiers, message
   validation, identity mapping, auth policy, operation correlation, delivery
   sequence, resource limits, error names, and client reconnection semantics.
2. **Build daemon-side handler core without D-Bus.** Use fake clients
   and a controllable local `Dmn_DMesgNet` publisher; prove handler ownership,
   the initial-playback handler-context barrier, local conflicts, ordered event
   notification, queue overflow, and close ordering deterministically.
3. **Implement D-Bus service and proxy.** Use a private bus. Test service
   ownership, credential binding, open/activate handshake, unicast delivery,
   that `OpenHandler` replies only after initial events are staged without
   blocking shared dispatch, concurrent requests, client disconnect cleanup,
   daemon loss, and bounded queues. Never use the developer's session or
   system bus in tests.
4. **Connect one daemon to a fake inter-node transport.** Run two independent
   daemon instances with deterministic forwarding, duplication, reordering,
   loss, partition, and restart controls. Verify one DMesgNet identity per
   daemon and that local clients do not appear in cluster membership.
5. **Exercise conflict propagation.** Cover two local client writers on one
   host, concurrent writers on different hosts, stale/equal counters,
   force/playback repair, repair loss, conflicting master observations, and
   partitions. Assert local handler state and explicitly assert that no test
   treats the result as consensus or a durable commit.
6. **Qualify a real inter-node adapter and deployment.** Test authentication,
   malformed/oversized frames, transport failure, node-ID spoofing, resource
   exhaustion, clean shutdown/final heartbeat, and recovery/restart behavior.
   A real broker/network test is separate from deterministic protocol tests.

Minimum acceptance criteria:

- A host with multiple local clients contributes exactly one configured node
  identity and one set of DMesgNet heartbeats to the inter-node cluster.
- Client publishes and remote messages pass through the daemon's same DMesg
  publisher and correctly update each server-side handler's topic/counter and
  conflict state.
- Initial playback is not lost or delivered under an unknown handler ID.
- A conflict in one client handler is not silently reported as another
  client's conflict; repair events are delivered to the correct live
  handlers.
- Client message fields cannot forge server identity or inject DMesgNet system
  and repair controls.
- Every configured queue remains within its bounds; overload
  is visible and does not block the shared publisher or D-Bus dispatch path.
- Method replies distinguish local acceptance from network send and remote
  application; no exactly-once, global-order, quorum, or consensus claim is
  made without a separate protocol.
- Daemon and client teardown leave no worker referencing destroyed handler,
  connection, or publisher state; daemon output remains alive for the final
  best-effort `Destroyed` heartbeat.

## 11. Research references and related specifications

- [D-Bus Message Bus Specification](https://dbus.freedesktop.org/doc/dbus-specification.html#message-bus-names):
  well-known and unique names, method routing, broadcast signals, and
  destination-specific messages.
- [D-Bus authentication protocol](https://dbus.freedesktop.org/doc/dbus-specification.html#auth-protocol):
  local connection authentication; it is distinct from identity fields in
  `DMesgPb`.
- Repository contract: [`Dmn_DMesg`](dmesg-spec.md) and
  [`Dmn_DMesgNet`](dmn-dmesgnet-spec.md), especially local per-topic conflict,
  playback, membership, and current master-heuristic limitations.
- Existing same-host transport: [D-Bus byte I/O and direct injection](dmesg-dbus-spec.md),
  [Option A](dmesgnet-dbus-injection-spec.md), and
  [Option B facade](dmesgnet-dbus-facade-spec.md).
- Existing inter-node option: [Kafka transport](kafka-spec.md).
- Stronger authority requirements and the warning against using DMesgNet
  master state as consensus: [DLock over DMesgNet](dmn-distributed-lock-dmesgnet-spec.md).
- Current daemon stub and build/test status: [Executables](executables-spec.md).

The linked D-Bus references were reviewed 2026-10-05. They describe local bus
routing and naming; they do not define this proposed gateway protocol.
