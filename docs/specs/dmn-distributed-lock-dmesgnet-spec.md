# Distributed Range Lock over `Dmn_DMesgNet`

Status: **proposed design; not implemented**. This is a separate cross-network
design for a future DLock mode. It is not an extension of the local
`Dmn_DLock<Dmn_DMesg>` prototype, and the current `Dmn_DMesgNet` master election
is not a lock authority or a consensus protocol.

Design readiness: the code-derived transport boundary and lock semantics are
specified, and the v1 profile is fixed-membership Multi-Paxos with explicit
state transitions, failure assumptions, API semantics, and implementation
gates. No additional product decision is required to begin implementation.
This is **specification-ready, not implementation- or deployment-ready**:
`DLockNet` does not exist yet, and a separate consensus safety review remains
mandatory before release.

This document is intentionally specific about two different things:

1. **Observed DMesgNet behavior** is specified in
   [`dmn-dmesgnet-spec.md`](dmn-dmesgnet-spec.md), based on the implementation,
   adapters, protobufs, and tests. Only tests `-1` through `-6` are registered
   in `test/CMakeLists.txt`; `-7` through `-9` are commented out.
2. **Proposed DLock behavior** is normative for a future implementation and
   must be proven by the test plan in
   `dmn-distributed-lock-dmesgnet-plan.md`.

The source of truth for current behavior remains code and registered tests;
this proposal does not claim that the existing network layer provides the
reliability, consensus, persistence, or fencing required by a distributed
lock.

## 1. Relationship to the other DLock design

`dmn-distributed-lock-spec.md` describes the intended single-process,
single-`Dmn_DMesg`-publisher protocol. Its publisher acceptance point,
handler-local mirrors, and retry behavior do not carry over to network mode.
The current `Dmn_DLock` implementation itself is still a local mutex/condition
variable prototype; neither design is currently implemented as specified.

Network mode has distinct requirements:

- Nodes communicate through configured `Dmn_Io<std::string>` endpoints carrying
  serialized `DMesgPb` messages. The topology and delivery guarantees depend on
  the I/O adapter; `Dmn_DMesgNet` does not create a reliable all-to-all network.
- Each process has a local `Dmn_DMesg` publisher and local per-topic counters.
  Local publisher acceptance is not a cluster-wide commit point.
- The current master identity is an advisory, cooperatively reconciled
  `Dmn_DMesgNet` state. It is not a quorum certificate, durable term, exclusive
  lease, or proof that a node is the only leader.
- Application-message playback is a last-known-value mechanism, not a durable
  command log. It cannot establish which lock-table mutations committed before
  a crash or partition.

Therefore, a network DLock MUST have a separate cluster agreement protocol
whose commit and fencing rules are independent of local running counters,
heartbeats, node-list observations, `m_ready`, and the `masteridentifier`
field. `Dmn_DMesgNet` may provide message I/O after a peer-addressed adapter
exists. Its elected master MAY be displayed as a diagnostic hint, but MUST NOT
be trusted to grant, renew, expire, or release a lock.

The current code contains **no consensus algorithm**. It contains a small
cooperative master-selection heuristic: nodes announce observations, wait a
few local timer ticks, self-elect if they have not heard from a ready master,
and prefer an older initialized timestamp when they observe competing master
claims. There are no ballots, quorum acknowledgements, terms, durable
promises, replicated operation log, or commit certificates. A partition can
therefore produce multiple self-declared masters. The useful design lesson is
the small event-driven lifecycle and heartbeat/message plumbing, not a
consensus safety property.

## 2. What the current network code does

### 2.1 Construction, transport, and message flow

- A `Dmn_DMesgNet` node has a caller-provided name and optional input and
  output `Dmn_Io<std::string>` objects. With an input object it starts a reader
  process; with input and output it starts the periodic heartbeat timer.
- Locally published messages are serialized by a subscriber handler and sent
  to the output I/O when the node is ready. Non-system messages emitted before
  readiness are queued by topic and flushed later. System messages are sent
  without waiting for readiness.
- Incoming serialized messages are parsed and fed into the node's local
  `Dmn_DMesg` publisher. A message whose
  `sourcewritehandleridentifier` equals this node's name is dropped to avoid
  echo. Other inbound application messages are not blindly re-forwarded by
  this subscriber path.
- If either input or output is absent, the code sets the system self-state to
  `Ready` and names itself master, but this path does not run the heartbeat
  timer. The transport-ready atomic flag is managed separately. No
  distributed-lock implementation may infer network readiness or agreement
  from that state.
- The destructor stops local input/timer work and, if output is present, makes
  a best-effort write of a final `Destroyed` system message. This is not a
  durable leave record or an acknowledged cluster commit.

The output-only and input-only fallback sets the protobuf self-state to
`Ready`, but does not set the separate `m_ready` flag; in output-only mode,
normal messages therefore remain queued. `dmn-dmesgnet-spec.md` records this
as a current implementation defect, not an intended transport contract.

`Dmn_DMesgNet` does not define delivery acknowledgements, retransmission,
message authentication, replay protection, durable queues, or a uniform
broadcast contract. Any DLock protocol frame requiring those properties must
provide them itself or explicitly depend on a future transport contract.

### 2.2 Membership and cooperative master selection

For the current implementation, each node starts in `MasterPending`. When
both transport directions exist, the timer runs at a one-second interval:

- after three pending timer cycles without adopting a remote ready master, a
  node self-declares `Ready` and names itself master;
- a pending node that receives a remote `Ready` system message adopts that
  remote master and becomes ready;
- while ready and following a remote master, five timer cycles without a
  matching ready-master heartbeat return the local node to `MasterPending`;
- when two ready nodes report different masters, the implementation compares
  their initialized timestamps and may follow the older node's master;
- a received `Destroyed` system record removes that node from the local node
  list; the current code does not expire a node-list entry merely because
  heartbeats stop arriving; and
- the node list contains observed remote nodes, not the local node.

These are cooperative convergence heuristics. Nodes can self-elect during
message delay or partition, and different nodes can temporarily have
inconsistent membership or master views. Timestamps are not terms or
monotonic consensus indices. There is no voting quorum, durable election
state, leader fencing, membership-change protocol, or partition safety proof.
The constants and observed outcomes are current implementation details, not
parameters for a DLock correctness decision.

### 2.3 Topic counters, conflicts, and playback

`Dmn_DMesg` keeps a local running counter and last-message cache per topic.
Network input is applied through that local publisher. An out-of-date or
conflicting application message can mark the receiving write handler as
conflicted. `Dmn_DMesgNet` responds using conflict and force/playback messages;
the receiving side can force-apply a playback value and update its local cache.

The network layer also keeps a last-message value per topic. When its observed
remote-node count increases while it believes itself master, it re-sends those
cached values with the playback flag. This is bounded last-value replay, not
history replication. A joiner may receive the latest cached value, but cannot
recover every missed transition. Local running counters can detect some
out-of-sync writes; they do not provide a global total order, compare-and-swap
across nodes, or a distributed commit decision.

### 2.4 Current test evidence and its limits

The checked-in test sources provide these examples of current behavior. Only
tests `-1` through `-6` currently run through the default CMake test list;
tests `-7` through `-9` are disabled there and provide no routine CI evidence:

| Test | Exercised behavior | Limit |
|---|---|---|
| `dmn-test-dmesgnet-1` | Single-node `MasterPending` -> self-`Ready` -> `Destroyed` lifecycle. | Sleeps through the timer and checks emitted system messages only. |
| `dmn-test-dmesgnet-2` | Two pipe-connected nodes discover one another; first-started node is expected to remain master; explicit destroy removes it and the survivor becomes master. | Sleep-driven two-node case; no partition or silent-node expiry proof. |
| `dmn-test-dmesgnet-3` | Local handler writes and outbound application-message flow alongside system lifecycle. | One node; wall-clock delays; no cross-node ordering guarantee. |
| `dmn-test-dmesgnet-4` | Two-node pipe exchange, application delivery, explicit departure, and survivor promotion. | Similar timing-sensitive two-node path; no quorum or durable state. |
| `dmn-test-dmesgnet-5` | An inbound stale/equal topic counter causes a local handler conflict. | Single node with injected counter; does not prove network-wide serialization. |
| `dmn-test-dmesgnet-6` | Reversed startup order changes the expected senior/master; duplicate/conflicting message traffic exercises conflict and force-playback handling. | Scripted pipe forwarding and sleeps; playback is a current value, not committed-history recovery. |
| `dmn-test-dmesgnet-7` (disabled) | A one-node test expects its local handler not to receive its own write. | It does not test network echo suppression: output targets port 5000 while input binds port 5001, and local self-delivery is already suppressed by `Dmn_DMesg`; it is not registered in CMake. |
| `dmn-test-dmesgnet-8` (disabled) | Attempts to test playback to a later socket-connected node. | The first node has both input and output sockets, but the sleep-driven test does not establish deterministic delivery or replay; it is not registered in CMake. |
| `dmn-test-dmesgnet-9` (disabled) | Attempts one-way pipe-backed application delivery. | The sender is output-only and does not set `m_ready`; the assertions do not establish delivery under current code; it is not registered in CMake. |

These are primarily integration examples with sleeps and wall-clock timer
behavior. They do not test a DLock protocol, quorum, network partitions,
duplicate/reordered consensus traffic, durable recovery, membership changes,
or safe lease expiry. They do not establish that master election is safe for
granting locks. Test descriptions in this table identify exercised behavior,
not stronger guarantees implied by their names or comments.

The tests' observed master choices depend on startup order and timing; they
are examples of current behavior, not normative ordering guarantees.
Distributed-lock tests MUST use deterministic clocks and controllable
transport/storage fixtures for protocol correctness, with a smaller number of
separate real-transport integration tests.

## 3. Proposed cross-network lock model

### 3.1 Authority, membership, and safety boundary

The proposed design is **DLockNet-Lite**, a fixed-membership, three-voter
Multi-Paxos state machine dedicated to range-lock commands. It excludes
dynamic voter reconfiguration, automatic lock leases, follower reads,
leadership transfer, and replication of unrelated application data. The
algorithm is fixed for v1 rather than left as an implementation-time choice:
Multi-Paxos gives a deliberately narrow consensus core while retaining quorum
intersection, durable recovery, and ordered decisions. Raft remains a valid
alternative only through a separate reviewed protocol/profile change; it is
not a second, implicit implementation choice. A custom
“heartbeat + timestamp + majority vote” heuristic is not a substitute:
electing a coordinator alone does not recover prior accepted lock operations
or prove which state is authoritative.

The consensus group is the authority. It is distinct from `Dmn_DMesgNet`
membership and master election, even if the initial deployment deliberately
configures the same node set.

Normative rules:

1. A process may report a grant only after the corresponding state-machine
   command is chosen by a quorum and applied locally.
2. A node without quorum makes no lock-table progress. It MUST NOT grant from
   an apparently free local mirror, even if `Dmn_DMesgNet` says it is master.
3. A stale proposer cannot overwrite a chosen value. Every chosen command
   has one stable consensus slot.
4. The consensus group has an immutable cluster ID, three configured stable
   voter IDs, a two-voter quorum, and durable ballot promises, accepted values,
   and chosen/applied positions. The transport node name is not voter identity.
5. Every successful grant is its own chosen `Grant` command in a unique slot.
   Its fence is `(cluster_id, cluster_generation, slot)`, ordered
   lexicographically. `cluster_generation` is provisioned monotonically and
   may change only through an offline, reviewed recovery/migration; a voter
   must refuse startup if local durable generation conflicts with
   configuration. The slot is never reset within a generation. Protected
   resources persist the greatest fence they have accepted and reject older
   tokens. One slot MUST NOT grant more than one request.
6. If a deployment cannot provide durable consensus state and a quorum, the
   implementation MUST fail closed for lock grant/renewal. Availability is
   subordinate to mutual exclusion.

The lock-domain table is replicated state. `Dmn_IntervalBTree` may be used as
an in-memory derived index on each replica, but the canonical committed state
and its recovery image are the consensus log/snapshot. The B-tree is not itself
the authority or a wire format.

### 3.2 Fixed-membership Multi-Paxos profile

This section fixes the protocol vocabulary and behavior so implementation
does not substitute DMesgNet's master heuristic or an underspecified custom
majority scheme.

In this profile, a value is **chosen** once accepted by a quorum; chosen is
the protocol's commit status. A voter **applies** only the contiguous chosen
slot prefix to its state machine. An applied prefix is therefore never
inferred merely from a proposer announcement or a local lock-table mirror.

#### Failure and membership model

- Exactly three voters are provisioned with unique stable IDs, immutable
  `cluster_id`, `cluster_generation`, and protocol version. The quorum is two.
  DMesgNet observations never change membership. Dynamic membership and
  one-voter "distributed" modes are out of scope.
- The safety model is crash/recovery, not Byzantine fault tolerance. Voters
  authenticate one another and follow the protocol; a compromised or
  equivocating voter is outside the guarantee.
- Voter state is stored in a checksummed append-only WAL. Promise changes and
  accepted values are appended and `fsync`ed before the corresponding promise
  or acceptance acknowledgement is sent. An incomplete final record can be
  truncated only because no acknowledgement may precede successful fsync; a
  complete record with a checksum failure is storage corruption and fails
  closed. Storage errors disable voting and serving lock mutations.
- A node whose durable cluster ID, generation, membership, or protocol version
  differs from configuration does not participate. Reusing a cluster ID with
  lost/reset data requires an offline generation increase before restart.

#### Ballots and leadership

- A ballot is the ordered pair `(round, voter_id)`. `round` is a persistent
  unsigned counter allocated above every locally observed round; `voter_id`
  breaks ties. Counter exhaustion is fatal and requires offline migration.
- A candidate sends `Prepare(ballot, cluster_id, generation)` to all voters.
  An acceptor that sees a greater ballot persists it as its promise before
  returning a `Promise` page containing its applied prefix, snapshot ID,
  accepted-record page, and continuation token. Pages are authenticated,
  bounded by the 64 KiB frame limit, and complete before the candidate counts
  that voter toward recovery. Repeated Prepare for the same ballot returns the
  current durable state/pages; it rejects lower ballots.
- A candidate becomes an active proposer only after promises from a quorum,
  and must complete recovery for the reported accepted slots before serving
  new lock operations or linearizable reads.
- A higher promise fences the old proposer at the acceptors that made it. An
  old proposer without a current quorum cannot obtain acceptance acknowledgments
  from a majority. No local timer expiration transfers authority by itself.

#### Ordered operations and recovery

- Each voter stores accepted values by monotonically increasing 64-bit slot:
  `(ballot, slot, command_id, command_bytes, digest)`. A single slot carries
  exactly one command. Slots are global to the cluster, not per lock domain.
- For each slot returned by a prepare quorum, the new proposer selects the
  value with the greatest accepted ballot and re-proposes it at that same slot.
  If two records for the same slot report the same greatest ballot but
  different digests, the candidate fails closed and requires operator
  intervention; it must not choose a value by arbitrary tie-break.
  A reported applied prefix is the acceptor's own durable contiguous chosen
  state; it does not establish the status of any slot above that prefix.
  Chosen state and any suffix above it are recovered from accepted records
  and snapshots.
  If a slot is reported with no accepted value, it remains eligible for a new
  value. Missing holes below the highest recovered slot are filled by `NoOp`
  commands before later slots are applied. The proposer never skips a reported
  accepted value and never invents a replacement for one.
- A proposer sends `Accept(ballot, slot, command)` only after recovery has
  established its authority. An acceptor accepts only if the ballot is not
  below its durable promise and the slot has no conflicting value accepted at
  that ballot; it persists the accepted record before `Accepted` response.
  Repeated identical accepts are idempotent; conflicting same-ballot accepts
  are protocol errors.
- A value is **chosen** when accept acknowledgements for the same ballot,
  slot, and digest are received from a configured quorum. The proposer
  broadcasts `Chosen(slot, ballot, digest, certificate)` where the certificate
  contains two distinct voter-authenticated durable `Accepted` receipts over
  those exact fields. Each receipt is independently verifiable by cluster
  members (for example, a member signature or an equivalent per-voter
  authentication proof). Learners verify the configured membership, receipt
  authenticity, matching ballot/slot/digest, and command bytes before
  recording the slot as chosen; the proposer announcement alone is not proof.
  Learners apply only a contiguous chosen prefix. Snapshot and state-transfer
  data use bounded, integrity-checked pages/chunks under the same frame limit.
  Lost `Chosen` notifications are
  repaired by querying voters and re-proposing the value under a later ballot
  if necessary. A client may receive success only after its command is chosen
  and applied in slot order by the responding node.
- If the proposer learns that a higher ballot has been promised, it stops
  issuing new accepts, returns unknown/unavailable for unresolved calls, and
  starts recovery before serving again. A response timeout never means the
  operation was not chosen.
- A linearizable read first completes prepare/recovery and chooses a `Barrier`
  in a fresh slot. The read is evaluated after applying all slots through that
  barrier. There are no follower reads in v1.
- Persist chosen/applied prefix metadata and lock state. A deterministic
  snapshot includes the exact included slot, state-machine image, all retained
  request records, unacknowledged terminal results, client
  sequence/acknowledgement   floors, cluster ID, and generation. Per-voter accepted records above the
  included slot remain in that voter's WAL and are not part of the shared
  snapshot. The next allocated slot is greater than both the durable
  checkpoint floor and every recovered accepted slot, not taken from a
  snapshot-local allocator. In v1, a voter may delete accepted records
  at or below the included slot only after all three configured voters
  acknowledge durable installation of the same snapshot; the durable
  checkpoint floor prevents any later acceptance at or below it. If any voter
  is unavailable, compaction waits; the implementation must alert on WAL
  growth and stop accepting new mutations before exhausting reserved storage.
  The checkpoint floor is durable and acceptors reject any new value for an
  included slot. Snapshot creation is permitted only through a contiguous
  chosen/applied prefix.

These rules define a small, fixed-membership Multi-Paxos profile, not general
purpose Raft or a claim that DMesgNet already implements consensus. A
maintained library may be used only if its behavior and durable-storage
contract are shown equivalent to this profile; otherwise implement this
profile behind a small deterministic core and subject it to independent
consensus review before release.

### 3.3 Transport and `Dmn_DMesgNet` integration

The authority protocol must address or otherwise authenticate messages from
configured peer IDs. The current `Dmn_DMesgNet` API exposes a generic input and
output `Dmn_Io<std::string>`:
it has no peer-addressed `send(peer_id, bytes)`, delivery acknowledgement,
stable authenticated peer identity, or transport-level backpressure contract.
Its normal DMesg application path also applies topic-counter conflict,
force/playback, local cache, and echo-suppression behavior. Those mechanisms
are not authority-protocol transport guarantees.

Before integrating the authority protocol, provide a `DLockNetTransport` interface with
`send(peer_id, frame)` and inbound `(peer_id, frame)` delivery, a stable
configured peer-ID map, bounded frame sizes, and explicit send/error behavior.
The adapter may use a broadcast/fan-out `Dmn_DMesgNet` deployment with a
recipient field and receiver-side filtering, or a dedicated point-to-point
I/O adapter. It MUST bypass last-topic playback and must not treat successful
local DMesg publication as remote delivery or quorum acknowledgement.
Consensus responses come only from authenticated voters that persist the
relevant state. Malformed DMesg protobuf parsing must be rejected before
consensus input; the current reader ignores the boolean parse result. The
code-derived DMesgNet behavior and adapter limitations are specified in
`dmn-dmesgnet-spec.md`; the current generic bridge is not an authority
transport without additional identity, validation, routing, and acknowledgement
contracts.

If the deployment cannot provide reliable peer identity and routing (or a
specified broadcast channel with identity/authentication and frame isolation),
it does not satisfy this profile. In that case use a dedicated peer-aware
transport rather than forcing authority frames through the current generic
DMesgNet pub/sub path.

### 3.4 Commands, results, and deterministic application

The protocol MUST define versioned, bounded messages for at least:

- acquire intent: cluster/domain, stable authenticated client stream ID,
  increasing client sequence, inclusive range, and wait/no-wait policy. Its
  immutable request ID is `(client_stream_id, acquire_sequence)`. A caller-local
  wait timeout is not a replicated lock deadline and does not cancel an
  already chosen request;
- release/cancel intent: client sequence, request ID, and authenticated
  owner identity;
- `RegisterClientStream`, `AckThrough`, and other stable control intents to
  manage a stream and acknowledge observed terminal results without consuming
  a stream sequence; `AckThrough` prunes terminal command results while
  retaining the client's acknowledged sequence floor;
- Multi-Paxos `Prepare`, `Promise`, `Accept`, `Accepted`, `Chosen`, `NoOp`,
  `Barrier`, recovery/state-transfer, and snapshot-install traffic; membership-
  change traffic is excluded from the initial fixed-membership profile; and
- result/query response, including chosen slot and fencing token.

Each client stream is bound to an authenticated principal and begins at
sequence 1. A `RegisterClientStream(client_stream_id, principal)` control
command is chosen before the first sequenced command; repeat registration of
the same ID/principal is idempotent, while binding the ID to a different
principal is rejected. Registration failures at the 4096-stream cap return
`ResourceExhausted`; no command sequence exists yet. The client persists its
next sequence before sending and
serializes sequence assignment per stream; callers sharing one stream must use
the same serialization mechanism and may have at most one not-yet-resolved
sequence per stream. After timeout/unknown outcome, the client retries or
queries that same sequence and MUST NOT issue the next sequence until the
prior command's chosen result is established. The replicated sequence high-water mark is
authoritative after client recovery. A retry MUST use the identical stream,
sequence, and payload. An acquire's request ID is derived from its stream and
sequence, so retries cannot create a second request. Repeating a retained
sequence with the same command digest returns its stored result; a different
digest is a protocol error. A gap returns `SequenceGap` and is not buffered.

Client-side validation failures and proposer-local admission failures detected
before sending any `Accept` return `InvalidArgument` or `ResourceExhausted`
without consuming a sequence. A deterministic request-table quota outcome is
a chosen `ResourceExhausted` command result and consumes the sequence.
Unacknowledged command results have a separate 100,000-record cap. Before
sending any `Accept`, the active proposer reserves one result slot; when none
is available it rejects new sequenced commands locally without consuming
their sequence. `AckThrough` remains admissible and frees result slots. A reservation is
released only after the command is known chosen or known not chosen; if the
outcome is unknown, the caller receives `UnknownOutcome` and the slot stays
reserved until recovery resolves it. On proposer recovery, every recovered
unresolved client command reserves a result slot before the proposer becomes
active; if capacity cannot be accounted for, it remains unavailable and fails
closed.
After any `Accept` has been sent, storage/transport failure cannot be reported
as a definite rejection: return `UnknownOutcome`/`Unavailable` unless recovery
proves the command was not chosen. This can leave a held lock unreleased until
capacity is restored, but cannot falsely acknowledge a release. Once proposed,
state-dependent outcomes (including `Busy` for a no-wait acquire, `NotOwner`,
and already-terminal request outcomes) are chosen and consume the sequence. A
chosen blocking acquire returns command result `Enqueued`; its
separate request state may remain `Waiting` and later transition through
`Granted` to `Released` or `Cancelled`. Same-sequence retries return the
original command result; clients use request status to observe later state.
`AckThrough(n)` is an idempotent, non-decreasing control command in a chosen
slot, with stable control ID `(client_stream_id, "ack-through", n)`; retries
reuse that ID and do not consume a client sequence. The first application is
accepted only when all client commands through `n` have terminal results and
the client confirms it observed them. It removes those result details but
retains the high-water mark. Active/waiting request
records remain until terminal even when their acquire command result is
acknowledged; terminal request records may be removed once the relevant
command results are acknowledged. `AckThrough(0)` is a no-op. A retry at or
below the acknowledged floor returns `OutcomeForgotten` and can never execute
again.

The exact chosen state-machine outcomes are:

| Command | Chosen result and transition |
|---|---|
| Blocking acquire accepted | `Enqueued`; creates a `Waiting` request; later grant is a separate chosen `Grant` slot. |
| Eligible no-wait acquire accepted | `Enqueued`; creates a `Waiting` request; later grant is a separate chosen `Grant` slot. |
| No-wait acquire blocked by a grant or earlier overlapping waiter | `Busy`; creates no request. If eligible, it follows the ordinary acquire then explicit-grant path. |
| Release of an owned granted request | `Released`; removes the grant. |
| Release of an unknown request | `NotFound`; no state change. |
| Release by a different authenticated principal | `NotOwner`; no state change. |
| Release of a waiting request | `NotGranted`; use `Cancel`. |
| Release of an already terminal request | `AlreadyTerminal`; no state change. |
| Cancel of an owned waiting request | `Cancelled`; removes it from the wait queue. |
| Cancel of an unknown request | `NotFound`; no state change. |
| Cancel by a different authenticated principal | `NotOwner`; no state change. |
| Cancel of a granted request | `AlreadyGranted`; use `Release`. |
| Cancel of an already terminal request | `AlreadyTerminal`; no state change. |

The active proposer emits `Grant(request_id)` only for a currently waiting,
eligible request. If cancellation is chosen first, a later/recovered Grant for
that request is a deterministic `GrantObsolete` no-op and does not change lock
state. A Grant referring to an absent or non-waiting request is also a
`GrantObsolete` no-op; this remains deterministic after terminal request
records are pruned. If Grant is chosen first, cancellation returns
`AlreadyGranted`.

Request status values are `Waiting`, `Granted`, `Released`, and `Cancelled`.
Unknown request IDs return `NotFound`; a previously acknowledged and pruned
terminal request returns `OutcomeForgotten`. Status/query operations use the
linearizable `Barrier` read path.

All lock-table changes are deterministic state-machine commands. Replicas
derive grants in chosen-slot order; they do not independently choose a grant
from message-arrival order. An `Acquire` command adds a waiting request to the
chosen queue. A no-wait request that conflicts with a grant or an earlier
overlapping waiter returns the chosen `Busy` result and is not queued. For a
request that is grantable, the active proposer proposes one explicit `Grant`
command in one consensus slot; all replicas
validate eligibility when applying it. The client receives success only after
that `Grant` slot is chosen and applied. A later request cannot bypass an
earlier overlapping waiter, while disjoint requests
may proceed. Every sequenced client command carries a stable authenticated
`client_stream_id` and strictly increasing per-stream sequence as specified
above. A chosen
`AckThrough(client_stream_id, sequence)` may prune terminal result details
only through a contiguous sequence the client confirms it has observed;
active/waiting request records cannot be pruned. The acknowledged high-water
mark is retained indefinitely.

For a domain, granted ranges are inclusive and mutually exclusive: two grants
conflict when they share any integer endpoint. Adjacent ranges `[a,b]` and
`[b+1,c]` do not conflict, with comparisons implemented without overflowing
signed integer bounds. Invalid negative or reversed DLock ranges are rejected
before proposal. Waiting requests do not block or grant based on uncommitted
state. To keep the first state machine deterministic and small, use conflict-
aware FIFO: order waiters by chosen enqueue slot; a waiter may not be
bypassed by a later waiter whose requested range overlaps it, but disjoint
later waiters may proceed. On each committed release/cancel, scan in enqueue
order and commit explicit grant commands for every now-eligible waiter. This
avoids making grant decisions from local scheduler timing. Priority is not
part of initial network-mode admission; it can be added only as a separately
specified ordering rule.

The consensus apply function MUST define exact results for acquire, no-wait,
release, cancel, duplicate sequence, timeout, shutdown, not-owner, and unknown
request. A caller timeout means "the caller stopped waiting," not "the
command did not commit"; the client must query/retry with the same sequence.

### 3.5 Lifecycle, restart, and ownership

- Client IDs and command sequences are stable across retries and client
  reconnects. A client process restart cannot silently start a new sequence
  stream or mint a new owner for an existing grant.
- Explicit release is a consensus command and takes effect only at commit.
  Handler destruction is not itself proof of release.
- The initial profile has no automatic expiry: a committed grant remains held
  until a committed release/cancel. A crashed client can therefore leave a
  grant held indefinitely. This is a deliberate fail-safe tradeoff, not
  garbage collection; a lease-based reclaim policy is excluded until separately
  designed and reviewed.
- Client/session failure detection is not sufficient to revoke a grant.
  Automatic cleanup requires a committed cancellation/release or a separately
  specified safe lease-expiry mechanism.
- Persistent log and snapshot recovery MUST restore committed grants,
  operation deduplication, fencing position, and membership before the node
  participates in voting or reports a lock result.
- Snapshot installation is atomic with respect to service: a replica must not
  serve lock operations from a partially installed state.
- Authorization binds release/cancel to the authenticated client principal
  stored with the chosen request. Transport payload source identifiers are not
  authentication.
- Client streams have a finite quota (v1 default: 4096). Stream identities
  and acknowledged sequence floors are durable and not silently reclaimed.
  New streams fail with `ResourceExhausted` at quota; stream removal requires
  an offline cluster-generation migration.
- The v1 public surface is a distinct `Dmn_DLockNet`; do not specialize the
  local `Dmn_DLock` template. It exposes acquire, no-wait acquire, release,
  cancel-waiting, status/query, and acknowledge-through. A chosen acquire
  submission returns `Enqueued` while request status is `Waiting`; the
  blocking API waits only in the calling thread for a later chosen `Grant`.
  If its local wait timeout expires, it returns `Waiting` with the request ID
  after a linearizable status check; if it cannot establish status because
  quorum is unavailable, it returns `UnknownOutcome`. Neither outcome cancels
  the request.
- No renew operation is exposed in v1. Cancellation applies only to a waiting
  request. `close()` stops local submissions and may submit explicit
  cancellation/release commands, but local destruction is never represented
  as a chosen release.

#### Fencing and protected-resource contract

The lock service guarantees a strictly ordered token for each chosen grant;
it cannot forcibly stop a paused, partitioned, or buggy former holder from
performing external side effects. A protected resource must atomically compare
each operation's fence with its persisted maximum, require the configured
cluster ID, and reject tokens lower than that maximum. This prevents a stale
holder from writing after the resource has observed a newer grant token. A
token alone does not notify the resource that a
new grant committed, so it does not provide immediate revocation before that
new token reaches the resource. Applications requiring that stronger property
must validate operations against the authoritative lock service or choose a
separately proven lease protocol.

Accordingly, DLockNet-Lite provides consensus-backed ownership and fencing
metadata, not a universal guarantee that arbitrary external work stops at
release. The API and user documentation must state this boundary.

### 3.6 Leases and timeouts

The initial network-lock phase MUST omit automatic lease expiry. A process
local monotonic timer or `Dmn_DMesgNet` heartbeat timeout MUST NOT expire a
committed lock. Any future lease feature requires a new specification with
bounded-clock assumptions or replicated logical time, committed lease state,
renewal/release ordering, restart handling, and fencing against stale holders.

Request wait timeouts may be client-local: expiration stops a caller's wait but
does not undo an already committed acquire. If server-side timeout/cancel is
required, it is a consensus command with a defined ordering relative to grant
and release.

### 3.7 Transport, replay, and protocol isolation

- Authority frames MUST be distinguishable from ordinary DMesg application
  messages, have explicit protocol and schema versions, cluster ID and
  generation, sender/recipient identity, ballot and slot as required by the
  Multi-Paxos profile, message ID, and integrity/authentication policy.
- Retransmission is allowed; handlers MUST tolerate duplicate frames. Reordered
  or delayed frames are checked against the protocol's epoch/round, committed
  position, and message ID.
- `Dmn_DMesgNet` last-topic playback MUST NOT be used to replay authority
  commands, establish committed state, or reconstruct the lock history.
  Recovery uses the selected protocol's own durable records/snapshots.
- The deployment adapter MUST document whether output is point-to-point,
  broadcast, brokered, or fan-out; how writes fail; whether reads preserve
  message boundaries; and what shutdown does to in-flight frames. The consensus
  protocol must remain safe if messages are lost, duplicated, delayed, or
  reordered within that contract.
- A `Dmn_DMesgNet` re-election is only a routing/discovery event. The lock
  authority independently establishes its epoch and quorum. A mismatch
  between the two master identities is not an error if the authority protocol
  remains safe.

All reads that influence API decisions MUST be linearizable. The initial
profile uses a quorum-confirmed read barrier through the current authority
before reading state (or an equivalent protocol/library operation that proves
the authority is current); it does not serve lock status from an arbitrary
follower mirror.

## 4. Non-goals and explicit prohibitions

The initial cross-network design does not promise availability during a
minority partition, exactly-once transport, a globally ordered stream of all
ordinary DMesg topics, or compatibility with a single-publisher local commit
protocol. It does not treat `nodelist`, timestamps, playback, conflict flags,
`m_ready`, local topic counters, or the elected DMesgNet master as consensus
evidence.

Do not enable `Dmn_DLock<Dmn_DMesgNet>` by changing only the template argument
or routing existing `Dmn_DLock` calls over network I/O. Do not advertise the
feature as distributed-safe before the acceptance gates in the plan pass.

## 5. Acceptance invariants

The implementation is not releasable until tests demonstrate:

- **Mutual exclusion:** no two committed granted entries overlap in a domain.
- **No minority grants:** no grant result is returned without a chosen
  quorum entry, including during partition or stale-proposer operation.
- **Linearizable chosen operations:** each successful acquire/release has
  one commit position and a legal sequential history.
- **Fencing:** a later chosen grant has a strictly newer fence; stale
  proposers and former owners cannot act as current owners.
- **Idempotency:** duplicated/retried commands do not create multiple grants
  or consume multiple sequence/fence values.
- **Recovery:** restart and snapshot installation preserve committed locks,
  fences, deduplication, and membership.
- **Deterministic application:** all replicas apply the same committed command
  stream to the same canonical table and interval index.
- **Fail-closed uncertainty:** timeout, transport loss, or lost response never
  turns an unknown operation result into an assumed failed/uncommitted request.
- **Lease safety:** if leases are introduced, expiration cannot overlap a live
  stale holder; otherwise automatic expiration remains disabled.

## 6. Required test-driven development plan

`dmn-distributed-lock-dmesgnet-plan.md` defines the incremental implementation
sequence and test inventory. It is a prerequisite for implementation and
acceptance; all protocol safety claims above require deterministic tests, not
timing-based inference from the existing DMesgNet integration suite.

## 7. Frozen v1 decisions and implementation assumptions

The following choices are normative v1 defaults, not unresolved questions:

| Concern | V1 decision |
|---|---|
| Consensus | Three-voter fixed-membership Multi-Paxos as specified in §3.2. Any algorithm substitution requires a reviewed spec change. |
| Fault model | Crash/recovery, non-Byzantine voters, authenticated member identity, two-of-three quorum. |
| Transport | New peer-aware `DLockNetTransport`. A DMesgNet-backed adapter is optional only if it supplies authenticated sender identity and addressed/fan-out frames while bypassing DMesg topic counters and playback. Current generic I/O alone is insufficient. |
| Persistence | Checksummed per-voter WAL; fsync promises and accepted values before acknowledgements; atomic durable snapshots. |
| Membership | Three statically provisioned voter IDs; no dynamic change. Offline migration bumps cluster generation. |
| Client identity | Authenticated stable client stream ID with strictly increasing command sequence. |
| Deduplication | Keep results until terminal and client `AckThrough`; retain acknowledged high-water marks indefinitely. |
| Lock lifetime | No automatic expiry; explicit committed release/cancel only. No renew operation in v1. |
| Fencing | `(cluster_id, cluster_generation, chosen_slot)`; protected resource atomically persists and compares the maximum accepted token. |
| Wait ordering | Conflict-aware FIFO by chosen enqueue slot; disjoint ranges may proceed. No priority in network mode. |
| Public API | Distinct `Dmn_DLockNet`; do not enable `Dmn_DLock<Dmn_DMesgNet>`. |
| Limits | Maximum 64 KiB encoded frame, exactly three voters, at most 4096 client streams, 100,000 retained/active requests, and 100,000 unacknowledged command results per cluster, plus finite configured WAL and reserved snapshot/recovery quotas; reject with explicit resource errors before capacity is exhausted. |

Concrete serialization, crypto library, WAL encoding, and transport adapter
remain implementation choices only where they preserve these contracts and
are covered by compatibility/conformance tests. A deployment without
authenticated voter identity or durable storage is unsupported; this does not
weaken the safety profile.

## 8. Research references

- Leslie Lamport, [Paxos Made Simple](https://lamport.azurewebsites.net/pubs/paxos-simple.pdf):
  safety basis for ballots, promises, accepted values, and quorum intersection.
- Tushar Chandra, Robert Griesemer, and Joshua Redstone, [Paxos Made Live: An
  Engineering Perspective](https://research.google/pubs/paxos-made-live-an-engineering-perspective/):
  operational lessons for persistent state, recovery, and deploying Paxos.
- Diego Ongaro and John Ousterhout, [In Search of an Understandable Consensus
  Algorithm (Raft), USENIX ATC 2014](https://www.usenix.org/conference/atc14/technical-sessions/presentation/ongaro):
  comparative reading for election, replicated-log, and membership design;
  not the selected v1 protocol.
- [Raft project overview](https://raft.github.io/): majority-based progress and
  replicated-state-machine framing.
- [etcd-io/raft](https://github.com/etcd-io/raft): deterministic Raft core
  design with caller-provided transport and storage; useful architecture and
  testing reference, but its Go implementation is not a direct C++ dependency.
- [NuRaft usage guide](https://github.com/eBay/NuRaft/blob/master/docs/how_to_use.md):
  C++ Raft option providing server/Asio layers while the application supplies
  log store, state machine, and state manager. Its integration and networking
  fit with DMesgNet still requires a prototype and dependency review.
- [etcd API guarantees](https://etcd.io/docs/v3.5/learning/api_guarantees/):
  useful reference for the client-visible distinction between linearizable
  operations and weaker/eventually delivered watches.
