# `Dmn_DMesgNet`: Existing Design, Guarantees, and Improvement Study

Status: code-derived specification and design review. The **current behavior**
sections describe `include/dmn-dmesgnet.hpp`, `src/dmn-dmesgnet.cpp`, the
underlying `Dmn_DMesg` publisher, protobuf schemas, I/O adapters, and checked-in
tests. The improvement sections are recommendations, not claims about current
code. This document does not prescribe a complete Raft implementation.

## 1. Purpose and relationship to `Dmn_DMesg`

`Dmn_DMesgNet` publicly derives from `Dmn_DMesg`. It retains the local
DMesg handler API and publisher/subscriber behavior, and adds a bridge between
that local in-process message bus and caller-supplied
`Dmn_Io<std::string>` endpoints:

```text
local DMesg handler
      |
      v
Dmn_DMesg publisher/subscribers <----> Dmn_DMesgNet bridge
                                              |          ^
                                  protobuf string        | protobuf string
                                              v          |
                                       output I/O     input I/O
```

The protobuf `DMesgPb` message is the serialized envelope. The network layer
does not replace the base publisher, nor does it persist or replicate the
publisher's complete history. It transports individual messages and adds
system-heartbeat handling, local membership observations, master-selection
heuristics, conflict/playback handling, and a last-value cache.

The local and network roles must not be conflated:

- `Dmn_DMesg` serializes local publication through its publisher context,
  maintains per-topic running counters and last-known messages, detects
  counter conflicts, and can replay cached values to local handlers.
- `Dmn_DMesgNet` serializes protobuf messages to and from I/O adapters and
  reconciles selected network system messages into local state.
- The I/O adapter controls actual routing and delivery. `Dmn_DMesgNet` does not
  itself create a network, guarantee that every peer receives a frame, or
  identify/authenticate the network sender.

In particular, the master heuristic is not a consensus or commit protocol.
There are no quorum acknowledgements, durable terms/votes, replicated log,
commit index, or safety rule preventing two partitioned nodes from each
believing they are master. A `Ready` state or `masterIdentifier` is an
observation made by that process, not proof of exclusive authority.

## 2. Current construction and message flow

### 2.1 Constructor modes

The constructor accepts optional input and output
`Dmn_Io<std::string>` instances. It initializes a local `sys` message with the
caller-supplied name as identifier, wall-clock initialization/update
timestamps, `MasterPending`, and an empty master identifier. It always opens
the outbound local subscriber. If an input endpoint exists, it also opens an
internal write handler and sys handler and starts a blocking input-reader
process. A one-second heartbeat timer is started only when **both** input and
output endpoints exist.

With both endpoints present, the constructor publishes its initial system
message through the internal sys handler; the outbound subscriber emits system
messages even before the readiness flag is set. With either endpoint absent,
the timer/election state machine is skipped and the local `m_sys` value is set
directly to `Ready` with self as master. This fallback does not perform peer
discovery.

The fallback does not set the atomic `m_ready` flag used by application-message
outbound buffering. Consequently, an output-only instance can have local
`Ready` state while its non-system outbound messages continue to queue rather
than send. This is a code-level inconsistency, not an intended contract.

### 2.2 Local-to-network application messages

The network subscriber subscribes to all local topics, including system
messages, by using the no-topic-filter and include-system configuration. Its
filter excludes messages whose `sourceWriteHandlerIdentifier` equals this
node's name, which is the bridge's loop-prevention marker for inbound
messages.

For each locally published message, the subscriber's async callback:

1. writes this node's name into `sourceWriteHandlerIdentifier`;
2. sends a serialized message immediately if `m_ready` is set, or if the
   message is a system message;
3. otherwise queues non-system messages by topic until readiness;
4. records each transmitted non-system message as the last value for its topic.

When the local node believes itself to be master and observes the local
`nodeList` size increase, the heartbeat task resends each cached topic value
with the `playback` flag set. This is **one last-known value per topic**, not
an event log or state snapshot protocol. The resend condition is based on
neighbor-count growth, not a per-peer acknowledgement; it cannot establish
that a particular joining node received or applied the value.

### 2.3 Network-to-local messages

The input process blocks on `read()`, parses each string as `DMesgPb`, and
discards a message whose `sourceWriteHandlerIdentifier` equals the local node
name. It then handles system and application messages differently:

- `sys` messages are asynchronously passed to system reconciliation and are
  not first delivered as ordinary inbound application messages.
- A message marked `conflict` causes a self-believed master to send the
  cached topic value marked `force` and `playback`.
- A message marked both `force` and `playback` is passed to the internal
  `Dmn_DMesg` writer and cached locally if accepted.
- An ordinary application message has its source-write-handler field replaced
  with the receiving node's name. The bridge compares its counter with its
  local last-topic cache, may mark it conflicted, and submits it through the
  internal writer. Accepted messages update the network layer's last-value
  cache. A rejected/conflicting message results in conflict and cached
  force/playback frames being emitted.

The receive path is a bridge into the local DMesg publisher, not a remote
commit acknowledgement. Local per-topic counters and conflict state are
re-evaluated on each node; they are not a global replicated counter or a
globally ordered history. Inbound data is not automatically forwarded by the
outbound subscriber because its source-write-handler marker is rewritten to
the local node name and then filtered.

`Dmn_DMesg` playback and force flags have special local semantics: playback
bypasses the base publisher's normal running-counter conflict path, while force
can set/repair a counter and clear a handler conflict. These are repair
mechanisms for cached topic state, not a consensus decision, quorum proof, or
durable recovery mechanism.

## 3. Current system state and master-selection behavior

The system protobuf carries one node's self record and a repeated node list.
Each self record includes identifier, state, master identifier, and
initialized/updated timestamps. The code uses `gettimeofday()`; these are
wall-clock timestamps, not monotonic clocks, terms, or durable epochs.

### 3.1 Local state transitions

With both I/O endpoints configured, the transitions are:

- **Startup:** remains `MasterPending` with an empty master identifier; the
  initial sys message is published.
- **Pending, receives remote `Ready`:** adopts that record's
  `masterIdentifier`, becomes `Ready`, resets counters, publishes local sys
  state, sets `m_ready`, and flushes queued outbound messages.
- **Pending, no suitable remote `Ready`:** after three one-second timer
  callbacks, self-declares `Ready` and self as master, then sets `m_ready`.
- **Ready, follows remote master:** each timer tick increments the sync
  counter; a `Ready` sys record from the followed master resets it. At five
  ticks without one, returns to `MasterPending`, clears its master identifier,
  and clears `m_ready`.
- **Ready, current master reports non-`Ready`:** returns to
  `MasterPending`, clears the master identifier and `m_ready`.
- **Ready, observes conflicting `Ready` master claim:** adopts the sender's
  claimed master if that sender's initialization timestamp is earlier than
  the local node's.
- **Destruction:** stops input/timer processing and handlers, then directly
  attempts a final `Destroyed` sys write if output exists.

The thresholds are callback counts, not protocol deadlines: delays, scheduler
latency, and timer behavior affect elapsed time. The code tracks a timestamp
for the last remote-master observation but uses the five-cycle counter—not
elapsed timestamp arithmetic—to trigger the local transition.

### 3.2 Membership observations and limits

On receipt of a non-destroyed sys record, the receiver inserts or updates that
record in its own `nodeList`; a `Destroyed` record removes a matching entry.
It does not merge the remote message's advertised `nodeList`. The list is
therefore a set of directly observed non-destroyed peers, represented by
caller-provided identifiers. The current code does not remove a peer merely
because its heartbeat stops arriving. The five-cycle master-sync rule only
tracks the currently followed master and does not perform general node-list
failure detection.

Master reconciliation is cooperative and local:

- a pending node adopts a received `Ready` node's claimed master;
- a node follows a claim from a sender with an earlier initialization
  timestamp when it sees a conflicting master claim; and
- nodes can self-declare after waiting, or return to pending after missing
  the followed master's synchronization messages.

There is no election round, voter set, ballot, majority, candidate log
freshness check, or quorum intersection. Different message visibility,
startup timing, wall-clock ties/skew, partitions, and duplicate identifiers
can leave nodes with inconsistent membership/master views or simultaneous
self-declared masters. This is a convergence heuristic under cooperative
message exchange, not consensus.

## 4. Transport and deployment boundary

`Dmn_Io<std::string>` specifies read/write/shutdown operations but does not
specify delivery, ordering, retry, sender identity, authentication,
peer-addressed routing, backpressure, or concurrent-call guarantees.
`Dmn_DMesgNet` does not add these guarantees.

The checked-in adapters have different behavior and must be qualified
individually:

- `Dmn_Pipe` is used to build in-process test forwarding links; it is not a
  network guarantee.
- `Dmn_Socket` uses IPv4 UDP datagrams. It preserves zero-length datagrams,
  validates IPv4/port configuration, and reports receive errors or truncated
  datagrams with `std::system_error`. `Dmn_DMesgNet` discards empty or malformed
  protobuf payloads; it logs and discards oversized datagrams and stops its
  input worker after other socket read failures. The socket adapter is not
  thread-safe, and its inherited `shutdown()` does not interrupt a blocked
  read. `Dmn_DMesgNet` cancels and joins its owned input worker before releasing
  the adapter; other callers must stop and join their I/O threads before
  destroying a socket.
- `Dmn_DMesgNet_Kafka` uses the fixed `Dmn_dmesgnet` topic, a consumer group
  named from the node, and a static producer key. Kafka acknowledgements are
  not translated into per-peer DMesgNet membership or application-apply
  acknowledgements.

The DMesgNet input code rejects empty payloads and checks the result of
`ParseFromString()`, logging and discarding invalid messages. The message
envelope still has no authenticated transport-sender field; its source
identifiers are payload values and can be spoofed by a sender able to publish
to the configured channel.

An adapter that fans messages to every subscriber can provide useful
best-effort dissemination. It still does not supply authenticated peer identity
or prove which voters received, persisted, or applied a message. A DLock
protocol requiring acknowledgements must establish those properties through
an explicit, isolated transport/protocol seam; it must not infer them from a
successful local write or from a DMesgNet `Ready` state.

## 5. Current guarantees and non-guarantees

### The implementation provides

- Local DMesg handler APIs through inheritance.
- Serialization of locally published `DMesgPb` messages to the configured
  output endpoint, subject to readiness buffering for ordinary messages.
- Parsing/dispatch of input strings through the local publisher path, subject
  to the routing/flags/conflict branches described above.
- Best-effort heartbeat exchange and per-process membership/master observations
  when both endpoints are configured and messages are exchanged.
- Per-topic local counter conflict detection and last-value repair/playback.
- A direct final `Destroyed` message attempt when an output endpoint exists.

### The implementation does not guarantee

- Reliable or authenticated delivery, peer-addressed unicast, end-to-end
  acknowledgement, retransmission, or bounded message size.
- A complete, ordered, durable, or replayable network history.
- Identical global topic counters, exactly-once delivery, or global ordering
  across independent DMesgNet instances.
- A single master, quorum election, consensus, linearizable reads/writes,
  fencing, or partition-safe mutual exclusion.
- General peer liveness detection or automatic removal of silent peers.
- That last-value playback reaches or synchronizes every joining peer.
- That a local `Dmn_DMesg` conflict signal identifies which remote peer or
  authoritative state is correct.

## 6. Checked-in test evidence

Tests `dmn-test-dmesgnet-1` through `-6` are in the default CMake test list.
Tests `-7` through `-9` are present but commented out in
`test/CMakeLists.txt`; they are not routine CI evidence.

- **`-1`:** single-node startup state emission, self-`Ready`, and final
  `Destroyed`. **Limit:** sleeps through timer; no peer behavior.
- **`-2`:** two pipe-forwarded nodes discover one another; tests the
  first-started-master expectation, explicit departure, and survivor promotion.
  **Limit:** timing-driven two-node path; no partition or quorum test.
- **`-3`:** local handler writes appear on output; a message observed by the
  test triggers another write; checks lifecycle output. **Limit:** single node,
  sleeps, no remote application delivery.
- **`-4`:** two nodes exchange system and application messages; explicit
  departure is followed by survivor promotion. **Limit:** sleep-driven, with
  no loss/reorder/partition or durable recovery.
- **`-5`:** injected inbound counter conflict places a local write handler in
  conflict. **Limit:** single-node counter test; no network-wide conflict proof.
- **`-6`:** reverse startup order, duplicate/conflicting traffic, force/playback,
  application delivery, and explicit departure. **Limit:** scripted pipe
  forwarding and sleeps; playback is not history recovery.

The remaining checked-in sources are disabled and must not be counted as
passing evidence:

- `-7` expects a local handler not to receive its own write. It does not test
  network echo suppression: output targets port 5000 while input binds port
  5001, and local self-delivery is already suppressed by `Dmn_DMesg`.
- `-8` attempts playback to a later socket-connected node. The first node has
  both input and output sockets, but the test is disabled and timing-driven;
  it does not establish deterministic delivery or replay.
- `-9` attempts one-way pipe-backed delivery. The sender is output-only and
  does not set `m_ready`; its assertions do not establish delivery under the
  current code.

These tests are integration examples, not deterministic protocol tests. They
use sleeps, wall-clock timer behavior, and shared captured state; they do not
prove convergence under arbitrary schedules or the guarantees listed as
nonexistent above.

## 7. Full design review: improvement gaps

Priority labels describe engineering order, not a claim that the code is
security-reviewed.

**P0 — Input and repair-path safety**

- **Gap:** Input rejects empty and malformed protobuf payloads but does not
  validate the semantic shape of parseable messages before dispatch. **Improve:**
  validate required envelope/body fields and add deterministic malformed,
  semantically invalid, and oversized-frame coverage.
- **Gap:** Conflict handling assumes a cached last-topic value exists and can
  dereference an empty optional. An input-only node has no output handler,
  while some conflict paths write to it. **Improve:** make missing cache/output
  explicit error outcomes; do not assert, dereference, or silently claim repair.
- **Gap:** Identity and system state come from the payload; there is no
  authenticated transport sender or check that sys `self.identifier` matches
  the actual sender. **Improve:** validate stable authenticated member identity,
  cluster, and message kind before mutating membership/state.

**P1 — Membership, readiness, and convergence**

- **Gap:** Master conflict resolution compares a sender's initialization
  timestamp and adopts its claimed master; there is no ballot, term, or quorum.
  **Improve:** specify this only as best-effort discovery. Stronger authority
  requires a separate, reviewed protocol with epochs and quorum evidence.
- **Gap:** A peer is removed only on explicit `Destroyed`; silent failure leaves
  stale entries. **Improve:** add monotonic heartbeat expiry for membership
  display/routing, and state clearly that suspicion is not authority revocation.
- **Gap:** Wall-clock timestamps can move backward or tie and are used as
  seniority metadata. **Improve:** use stable IDs for deterministic tie-breaks,
  monotonic elapsed time for local timeouts, and wall time only as metadata.
- **Gap:** Replay triggers on neighbor-count growth and sends only a last value
  per topic, with no per-peer cursor, consistent snapshot, or acknowledgement.
  **Improve:** either label replay as best-effort cache sync or add versioned
  snapshots/cursors and explicit receiver acknowledgements.
- **Gap:** The no-both-endpoints fallback sets protobuf state to `Ready` but
  not `m_ready`, leaving output-only application messages queued. **Improve:**
  define readiness consistently and test all four endpoint combinations.

**P2 — Resource bounds and evidence**

- **Gap:** Pre-ready outbound messages are retained in unbounded per-topic
  vectors. **Improve:** specify bounds, coalescing/rejection behavior, and
  observable overflow.
- **Gap:** Playback checks neighbor-count increases, so replacing one peer with
  another at the same count does not trigger it. **Improve:** track membership
  generation/per-peer joins and make replay idempotent per peer.
- **Gap:** Some remote-state assumptions are enforced only by `assert`.
  **Improve:** validate state enums and required fields with explicit error
  reporting.
- **Gap:** Tests `-7` through `-9` are disabled and active tests are
  sleep-driven. **Improve:** correct and register useful adapter smoke tests,
  plus deterministic fake-clock/fake-I/O transition and fault tests.

### 7.1 Improvement boundary: better convergence is not consensus

The following changes would make the existing lightweight behavior more
deterministic and robust without turning it into Raft:

1. Define a stable node identity and validated message envelope.
2. Replace timestamp seniority ties with deterministic identity ordering.
3. Add monotonic heartbeat-expiry handling and an explicit member-incarnation
   or generation value so an old `Destroyed` message cannot evict a restarted
   process.
4. Make master claims monotonic within an explicit local election generation,
   reject stale generations, and define how simultaneous claims are resolved.
5. Separate membership liveness from master eligibility and topic playback.
6. Make cached-state replay versioned and bounded, and report whether replay was
   sent/received/applied without calling it a committed log.
7. Expose transport, parse, queue-overflow, and shutdown failures through an
   observable error interface, and add focused malformed/truncated-input tests.
8. Replace wall-clock sleeps in state-machine tests with injected clock/scheduler
   and controllable message delivery.

These measures can improve convergence under a cooperative connected network.
They cannot make independently partitioned nodes safely agree that exactly
one of them may commit lock grants. A deterministic tie-breaker or timeout is
not a quorum, and an epoch field without durable voter promises is not fencing.

## 8. Consequences for DLock over DMesgNet

The current local lock implementation is based on `Dmn_DMesg` publisher
semantics. Reusing it with `Dmn_DMesgNet` does not make the lock distributed:
the network layer can deliver competing table writes to different processes,
and each process has its own local counters, conflict decisions, and cache.
Neither `Ready`, `masterIdentifier`, a topic counter, nor force/playback may
authorize a lock grant.

The DLock proposal is maintained in
[`dmn-distributed-lock-dmesgnet-spec.md`](dmn-distributed-lock-dmesgnet-spec.md)
and its test-driven plan. That design must treat this document as the source
for DMesgNet-specific behavior and constraints. The compatible design boundary
is:

- `Dmn_DMesgNet` may remain the carrier for best-effort discovery or
  application dissemination.
- A consensus-backed DLock must have a distinct authority protocol that
  defines fixed voters, durable epochs/promises, recovery, quorum commit,
  linearizable decisions, and resource-side fencing.
- This authority protocol may be implemented with a focused consensus core
  rather than a general-purpose Raft deployment. It may not omit the safety
  properties those mechanisms provide. If automatic failover is not required,
  a single manually operated authority can avoid consensus but must fail closed
  and must not advertise automatic safe takeover.
- Existing generic DMesgNet I/O and application-message delivery do not expose
  enough peer identity/routing/acknowledgement guarantees for the authority
  protocol by themselves. Add an isolated peer-aware adapter or provide an
  externally qualified broadcast channel with authenticated member identity.
- A DLock fence can reject stale work only once the protected resource has
  observed and persisted a newer fence; no token can revoke arbitrary
  side-effects at a distance.

Thus the recommended path is not “make all of DMesgNet Raft.” Improve
DMesgNet's own message validation, membership lifecycle, convergence, replay,
and tests as a transport/pub-sub layer. Keep the DLock safety boundary narrow:
choose either a consensus protocol with a written proof and fault tests or a
single-authority mode with no unsafe automatic failover.

## 9. Test and documentation acceptance

Before claiming the current-code description is an implemented guarantee:

- tests must match the behavior and limitations in Sections 2–6;
- constructor-mode, malformed-input, missing-cache, absent-output, silent-peer,
  duplicate-ID, timestamp-tie, playback, and queue-boundary cases must be
  covered;
- deterministic tests must establish transition behavior; real sockets and
  Kafka remain separate adapter integration tests; and
- any future authority or DLock guarantee must be specified and tested
  separately from the DMesgNet master-selection heuristic.

The source-of-truth locations are:

- `include/dmn-dmesgnet.hpp`, `src/dmn-dmesgnet.cpp`;
- `include/dmn-dmesg.hpp`, `src/dmn-dmesg.cpp`;
- `src/proto/dmn-dmesg.proto`, `src/proto/dmn-dmesg-body.proto`,
  `src/proto/dmn-dmesg-type.proto`;
- `include/dmn-io.hpp`, the selected concrete I/O adapter, and
  `src/kafka/dmn-dmesgnet-kafka.cpp`; and
- `test/dmn-test-dmesgnet-*.cpp`, `test/CMakeLists.txt`.
