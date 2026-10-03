# Test-Driven Implementation Plan: Network DLock over `Dmn_DMesgNet`

Status: proposed implementation roadmap; no network DLock protocol is
implemented. This plan belongs to the independent cross-network design in
`dmn-distributed-lock-dmesgnet-spec.md`. It does not implement code as part of
the present documentation task.

## 1. Goals and hard boundaries

Build **DLockNet-Lite**, the fixed-membership three-voter Multi-Paxos profile
specified in `dmn-distributed-lock-dmesgnet-spec.md`, with a deterministic
replicated range-lock state machine. Keep the profile narrow: no dynamic voter
reconfiguration, automatic leases, follower reads, or leader transfer in the
first release. Do not reuse the single-process `Dmn_DLock<Dmn_DMesg>` publisher
retry algorithm and do not use `Dmn_DMesgNet`'s current cooperative master
election as consensus. The source of commit and fencing is durable quorum
evidence and an ordered chosen-slot history. Do not substitute a
different consensus algorithm without revising and reviewing the protocol
specification.

The current repository provides useful lower layers, but not a network lock:

- the code-derived architecture and improvement review are documented in
  `dmn-dmesgnet-spec.md`;
- `Dmn_DMesgNet` serializes messages through caller-supplied
  `Dmn_Io<std::string>` endpoints and locally feeds accepted inbound application
  messages to `Dmn_DMesg`;
- it has timer-driven cooperative master/membership heuristics, local
  per-topic counter conflict handling, and best-effort last-value playback;
- its current generic I/O path has no peer-addressed send API or documented
  quorum/acknowledgement contract, and normal DMesg conflict/playback behavior
  must not be mistaken for authority-protocol transport;
- the registered tests use pipes and sockets and mostly wall-clock sleeps; and
- `Dmn_DLock` remains a local in-memory prototype, while its single-publisher
  target is separately specified.

No completed layer may claim quorum safety until consensus state is durable,
fault injection proves the safety properties, and all exit criteria below
pass. If persistence, membership, or protocol assumptions are unresolved,
stop at a disabled prototype rather than ship an unsafe lock.

## 2. Development and testing rules

For every layer:

1. Add a focused test that expresses the invariant and fails for the missing
   behavior before implementation.
2. Implement only enough production behavior to satisfy that test while
   preserving the previous layers.
3. Run the focused target, then all completed network-lock layers.
4. Run formatting/static checks and `git diff --check`.
5. Keep protocol correctness independent of sleeps and real wall-clock timers.
6. Record whether the test is a deterministic unit, deterministic fault
   simulation, persistence/restart test, or real-transport integration test.
7. Never add a placeholder-success implementation or a test for a protocol
   layer whose production API is not yet defined.

Use injected deterministic dependencies:

- manual monotonic/logical clock, with no implicit real-time expiry;
- deterministic member, session, owner, operation, and message IDs;
- seeded or disabled randomized backoff;
- controllable message transport that can hold, drop, duplicate, reorder, and
  partition frames;
- in-memory and restartable durable-log/storage fixtures;
- explicit scheduler/executor drains and promises/barriers for concurrency;
- deterministic ballot, prepare/recovery, accept, and chosen advancement.

Wall-clock and live-socket tests are smoke/integration tests only. They must
have bounded timeouts and must not serve as evidence for election, lease, or
mutual-exclusion safety.

## 3. Test architecture and shared fixtures

Create a reusable `Dmn_DLockNet_TestCluster` fixture with three independent
configured voters.
Each voter has separate promise/accepted state, durable storage, local lock
state, and a peer-aware transport endpoint. The fixture can:

- connect nodes using explicit unicast/fan-out routes;
- advance timers and run queued work without sleeping;
- partition and heal arbitrary links;
- drop, delay, duplicate, and reorder selected frames;
- stop/restart nodes from persisted state;
- inspect ballot promises, accepted/chosen slots, and deterministic
  state-machine snapshots;
- count successful grant responses and verify every returned commit/fence; and
- inject transport/storage failures at well-defined operation boundaries.

Provide a pure state-machine model/oracle. For every trace, compare all
replicas with the oracle and assert table invariants after each chosen
operation. Keep transport events separate from chosen-slot and state-machine
application events.

## 4. Incremental layers and exit criteria

### Layer 0 - Decision record and protocol contract

The protocol choices are frozen in the spec: fixed 3 voters, quorum 2,
crash/recovery (non-Byzantine) model, Multi-Paxos, persistent promises and
accepted values, no leases, strict client stream sequences, and an explicit
peer-aware transport. Implementations may choose serializer, crypto provider,
WAL encoding, and adapter subject to conformance.

**Tests:** review the Multi-Paxos safety argument against the implementation
state transitions; prove the repository can build a voter process with durable
WAL and route an authenticated peer frame without DMesg playback/conflict
mutation. No public lock API is enabled here.

**Exit:** a written protocol/state-transition table and failure model are
approved; exact `fsync` acknowledgement points, cluster bootstrap identity,
snapshot compaction gate, limits, and client retry/ack behavior match the spec.

### Layer 1 - Durable voter state and frame envelope

Implement the configured cluster identity, immutable 3-voter membership,
ballot encoding, checksummed WAL, durable promise/accepted-value writes, atomic
snapshot writer, and peer-authenticated transport envelope. Define frame
version, cluster ID/generation, sender/recipient voter IDs, ballot, slot,
message ID, and bounded payload. Reject malformed, oversized, unauthenticated,
wrong-cluster/generation, wrong-recipient, and unknown-required-version traffic
before the protocol state machine. DMesgNet's wall-clock master-selection
state is never input to a ballot.

**Tests:** valid/invalid frame round trips, including the 64 KiB total-frame
limit; identity/cluster/version rejection; page ordering/completion and
snapshot/state-transfer chunk integrity;
WAL crash after every append/fsync boundary; torn/corrupt tail classification;
restart reproduces promises and accepted records; a reply is never sent before
its durable record; duplicate WAL records recover idempotently.

**Exit:** all protocol-relevant durable fields survive restart and every
acknowledgement is ordered after its specified durable write.

### Layer 2 - Controllable transport and event loop

Add peer-addressed `DLockNetTransport::send(peer_id, frame)` and inbound
delivery carrying authenticated peer identity, independent of
`Dmn_DMesgNet`'s elected-master state. Implement a deterministic fake
supporting loss, duplication, delay, reordering, partitions, reconnect,
backpressure, and write failure. The production adapter may use DMesgNet only
if the configured I/O supplies fan-out/routing and identity; isolate authority
frames from ordinary DMesg topic counters, force/playback, and last-topic
replay. Otherwise use a dedicated peer transport; do not emulate one with
unrouted generic `Dmn_Io` calls.

**Tests:** each injected fault can be scheduled and observed; spoofed sender
identity and cross-cluster traffic are rejected; local echo is filtered;
duplicate and reordered frames reach ballot/slot validation rather than being
silently treated as chosen; normal topic-counter conflict/playback never
creates an `Accepted` response.

**Exit:** protocol tests can control every frame deterministically, and adapter
contract documents actual delivery guarantees.

### Layer 3 - Multi-Paxos prepare, recovery, accept, and learn

Implement the protocol in §3.2 as written: ballot allocation; Prepare and
durable Promise; quorum activation; per-slot highest-ballot recovery; NoOp
hole filling; Accept and durable Accepted; quorum-chosen detection; Chosen
dissemination; contiguous apply; and preemption/recovery. Do not add leases,
speculative lock grants, or an election shortcut based on DMesgNet Ready.

**Tests:** simultaneous ballots and deterministic tie-break; one durable
promise per acceptor; lower-ballot rejection; crash/restart before and after
Promise/Accepted fsync; complete paginated promises required for proposer
activation; chosen value recovered from every 2-of-3 prepare quorum; equal
highest-ballot records with different digests fail closed; unchosen minority
value safely superseded; conflicting accepted values never both chosen;
forged/incomplete Chosen quorum certificates rejected; chosen notifications
lost/reordered/duplicated; holes filled before apply; delayed old proposer is
rejected after two promises at a higher ballot; proposer restart recovers all
quorum-accepted commands; a one-node partition cannot choose. Exhaustively
enumerate short three-voter schedules.

**Exit:** model checking and deterministic fault schedules preserve one
chosen value per slot, preserve every previously chosen value, and never
advance applied state across an unresolved slot.

### Layer 4 - Fixed membership and bootstrap validation

Provision an immutable voter set for the cluster lifetime of this release:
exactly three voters and a two-voter quorum, tolerating one
unavailable voter. Validate duplicate IDs, cluster mismatch, and incomplete
or conflicting bootstrap configuration at startup. DMesgNet node-list
observations do not add/remove voters. Dynamic membership is explicitly out
of scope.

**Tests:** all nodes agree on the configured voter set; bootstrap mismatch and
duplicate member identity fail closed; unknown IDs cannot vote; a minority
cannot activate a proposer or choose a value; restart restores the same static
configuration. Test any future reconfiguration only in a separate change.

**Exit:** exactly three statically configured voters yield the documented
two-voter quorum and reject implicit membership changes.

### Layer 5 - Deterministic lock state machine and interval index

Implement pure deterministic commands for acquire, no-wait acquire, release,
cancel, request lookup, and terminal-result retention. Model inclusive range
conflicts with `Dmn_IntervalBTree` as a derived per-domain index; validate
negative/reversed DLock ranges and boundary behavior. Order waiting requests by
chosen enqueue slot and operation ID. Prevent bypass by a later waiter
whose range overlaps an earlier waiter, but permit disjoint requests to
proceed. Grant eligible waiters by committed grant commands, never by
replica-local scheduling. Rebuild the index from committed canonical state
after replay or snapshot install.

**Tests:** adjacent/shared endpoint and `int64_t` boundary cases; invalid
ranges do not mutate state; overlapping grants are impossible; disjoint grants
can coexist; waiter fairness is independent of replica scheduling; no-wait
conflict with either a grant or earlier overlapping waiter is deterministic;
request ID derived from stream/sequence is stable; duplicate client sequence
returns the same result;
same sequence/different bytes is rejected; sequence gap does not advance the
client floor; stream registration is idempotent for the same principal and
rejects rebinding; one in-flight sequence per stream; `AckThrough` rejects
unresolved gaps and makes old retries return `OutcomeForgotten`; exact
release/cancel/NotOwner/NotFound/AlreadyTerminal/NotGranted/AlreadyGranted
results; stale Grant no-op after cancellation/pruning; owner checks; terminal
outcomes and quotas; interval-index rebuild equals the canonical linear
oracle; identical chosen slots always produce the same eligible request and
grant command.

**Exit:** the pure state machine deterministically maps one chosen command
stream to one canonical table and invariant-preserving B-tree on every node.
The 100,000-request, 100,000-unacknowledged-result, and 4096-client-stream
quotas are enforced as specified: request-table rejection is a chosen,
sequence-consuming result; stream registration rejection precedes sequenced
commands; full result storage prevents new sequenced proposals until
`AckThrough` frees capacity. Test concurrent admission reservations, crashes
with reserved/unresolved results, and recovery accounting before proposer
activation. Proposer-local storage admission may return
`ResourceExhausted` without sequence consumption only before any `Accept` is
sent. Once proposal begins, uncertain failure is `UnknownOutcome`, never a
false successful release.

### Layer 6 - Quorum-backed DLock API

Expose a distinct `Dmn_DLockNet` API. Never specialize the existing
`Dmn_DLock<Dmn_DMesgNet>` in v1. Every sequenced client command uses its
authenticated client stream and next sequence number. An acquire submission
returns `Enqueued` once its command is chosen, while request status is
`Waiting`; blocking acquisition returns `Granted` only after a separate
`Grant` slot is chosen and applied. If its local wait expires, return `Waiting`
after a linearizable status check; if status cannot be established, return
`UnknownOutcome`. Neither local timeout nor unknown outcome cancels the
request. Conflict/no-wait, release, cancel, owner, status, and
acknowledge-through resolve from chosen state. A timed-out sequenced command is
resolved by retrying/querying the same sequence.

**Tests:** acquire/release sequential histories are linearizable; no node
returns success before a grant slot is chosen and applied; two overlapping
requests racing through different voters yield at most one grant; cancellation
before/after chosen enqueue is distinguished; release response is chosen;
lost response plus same-sequence retry is idempotent; local wait timeout
returns `Waiting` without cancellation; quorum loss during status resolution
returns `UnknownOutcome`; `AckThrough` permits
terminal command-result pruning while preserving an active request, and cannot
reactivate an old sequence; no-quorum
caller receives unavailable/unknown rather than a false grant; local mirror
never authorizes by itself.

**Exit:** API returns are traceable to committed entries and client retries
cannot duplicate or lose an operation outcome.

### Layer 7 - Fencing, restart, snapshot, and compaction

Represent each successful grant in its own chosen slot and derive its fence
from `(cluster_id, cluster_generation, grant_slot)`. Require downstream
protected resources to atomically store the greatest accepted fence and reject
lower tokens. A voter may compact a chosen prefix only after all three voters
persist and acknowledge the same snapshot/checkpoint floor. Snapshot install
is atomic; it contains canonical lock state, client sequence floors and
unpruned results, all retained request records, cluster identity, generation,
and its exact included slot.
Per-voter accepted records above that slot remain in that voter's WAL; the
next slot is recovered above the greatest accepted slot. If one voter is
unavailable, compaction pauses and WAL capacity alarms/limits apply.

**Tests:** strictly increasing grant-slot fences across release/reacquire,
ballot change, restart, snapshot, and compaction; resource validator rejects
an old fence after observing a newer one; snapshot plus suffix replay equals
full replay; replicas with different accepted suffixes install the same
canonical prefix snapshot without deleting suffix records; all-voter
checkpoint gate prevents premature deletion; corrupt
snapshot fails closed; interrupted install exposes neither partial state nor
service; sequence floors and unpruned outcomes survive snapshots; old slots
below the checkpoint floor cannot be accepted; no slot grants multiple
requests.

**Exit:** recovery and fencing preserve safety through every tested crash and
compaction boundary.

### Layer 8 - Failure handling and observability

Specify error mapping for no quorum, transport unavailable, storage failure,
`ResourceExhausted`, protocol/version mismatch, not-owner, invalid range,
conflict, timeout/unknown outcome, shutdown, and stale ballot/generation.
Log/metric hooks must not claim a grant
before commit or suppress storage/protocol errors.

**Tests:** each failure yields the documented result and no invalid state
transition; alerts and diagnostic events include cluster/domain/operation and
commit position without leaking credentials; shutdown drains or explicitly
abandons outstanding requests while preserving unknown-outcome semantics.

**Exit:** failure modes are visible, stable, and cannot become success-shaped
fallbacks.

### Layer 9 - Lease decision gate (deferred; disabled)

Do not implement lease expiration in the initial network-lock release.
Explicit committed release is required, so a crashed client can leave a lock
held indefinitely. If product requirements later demand reclamation, first
approve a separate clock assumption and protocol design. It must define
renewal, expiry ordering, leader change, clock uncertainty, suspend/resume,
restart, release races, fencing, and the maximum stale-holder interval.

**Tests before enablement:** manual-clock expiry at exact boundaries;
renew-vs-release ordering; quorum loss before/after deadline; leader change
near expiry; skew/uncertainty bounds; process pause/restart; stale-holder
resource rejection; partitions where the old holder cannot continue after
fencing/expiry.

**Exit:** an independent safety review accepts the assumptions and all lease
tests pass. Otherwise automatic expiry remains absent and documented.

### Layer 10 - Real transport and deployment acceptance

Run multi-process tests with the actual configured I/O adapter(s), including
process kill/restart and the deployment's network routing. Keep a separate
transport qualification matrix for point-to-point, broker, or broadcast
configurations.

**Tests:** bidirectional delivery; actual message boundaries and size limits;
loss/reconnect; duplicate/reordered delivery where possible; authentication
failure; shutdown with in-flight frames; node restart; network partition; and
consensus safety assertions over the resulting committed history. Existing
`dmn-test-dmesgnet-*` sleep-driven tests remain DMesgNet examples, not substitutes
for these tests.

**Exit:** all supported deployment adapters meet their documented assumptions
and the full deterministic safety suite plus real-transport suite passes.

## 5. Acceptance test matrix

Every row is required unless explicitly marked optional. Tests should be
organized by protocol layer and assertion, not by implementation file.

| Area | Required scenarios | Required assertion |
|---|---|---|
| Wire protocol | Round-trip, malformed, oversized, unknown version, wrong cluster | Reject invalid frames before state mutation |
| Ballots/recovery | Concurrent ballots, stale promise, delayed/paginated Prepare/Promise, chosen-value recovery, equal-ballot corruption, DMesgNet master disagreement | A chosen slot is immutable; only a fully recovered two-voter quorum activates a proposer |
| Quorum | Two-voter and one-voter partitions, asymmetric reachability, quorum loss | No new chosen operation without two acceptances |
| Accepted slots | Duplicate/reordered/lost Accept/Accepted/Chosen; invalid/missing quorum certificate; divergent accepted values; holes | Chosen slots never conflict; learners verify quorum evidence; recovered chosen prefix is preserved and holes precede apply |
| Membership | Fixed voter set, duplicate/unknown ID, bootstrap mismatch, restart | Only configured voters count toward quorum; no implicit membership |
| Lock semantics | Same/overlapping/disjoint/adjacent/boundary ranges; wait/no-wait | No overlapping committed grants; deterministic fairness |
| API races | Concurrent acquire/release/cancel, response loss, caller timeout | Linearizable chosen history; same-sequence retry returns same result |
| Client sequence | Stream registration/quota, gap, duplicate, changed payload, concurrent stream callers, unknown outcome, `AckThrough`, reconnect | No duplicate execution; one unresolved sequence per stream; old acknowledged sequence never executes again |
| Fencing | Reacquire, ballot change, old owner, restart, compaction | `(cluster, generation, slot)` strictly increases; resource rejects old fence after observing a newer one |
| Resource enforcement | Old-token use before and after newer token; concurrent updates | Resource atomically stores/checks fences; no claim of pre-observation revocation |
| Recovery | Crash before/after fsync/ack/chosen/apply/snapshot install | Recover all chosen state and never serve a partial snapshot |
| Interval index | Build/rebuild, mutation, boundary, randomized comparison to linear model | Derived B-tree equals canonical state-machine oracle |
| Transport | Loss, duplicate, reorder, partition, reconnect, echo, adapter errors | Transport events alone never imply a grant |
| Lease (not in v1) | N/A | No automatic expiry or renewal API exists |
| Security boundary | Spoofed identity, invalid auth, cross-cluster frames, replay | Unauthorized/stale frames cannot mutate committed state |

For property/model tests, generate reproducible schedules and retain the seed
and minimal failing trace. At minimum, continuously assert after every
committed command:

1. no two granted ranges overlap within a domain;
2. every grant has a unique owner/operation and committed fence;
3. chosen slot and applied-prefix indexes never regress;
4. all replicas that applied the same committed prefix have identical
   canonical state and deduplication result; and
5. no client success response references an uncommitted entry.

## 6. Release gates and non-goals

Do not release as a distributed lock until Layers 0-8 and 10 pass, all
acceptance invariants in the spec are covered, persistence and fixed membership
are qualified on supported hardware/OS/filesystems, and the Multi-Paxos
implementation has passed an independent consensus safety review. Automatic
lease expiry is not a v1 layer or a supported API; adding it requires a new
specification and safety review.

The current `Dmn_DMesgNet` heartbeat, node list, elected master, topic counters,
conflict repair, and last-value replay remain useful transport/application
behavior. They are not a replacement for the consensus log, do not transfer
the single-publisher DLock commit semantics to a network, and do not by
themselves satisfy any layer in this plan.
