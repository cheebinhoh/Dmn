# Future Specification: `Dmn_DLock` over `Dmn_DMesgNet`

Status: deferred future design; not part of the v1 `Dmn_DMesg` lock implementation.

## 1. Scope and non-goal boundary

This document defines the future consensus-backed evolution of `Dmn_DLock` for
`Dmn_DMesgNet`. It is intentionally separate from
`dmn-distributed-lock-spec.md`, which is complete and authoritative for the
current Phase 1 implementation.

This future design is not a v1 extension of the publisher-serialized DMesg
lock. `Dmn_DMesgNet` does not become a lock authority merely because its
transport is present. The lock authority is an explicit consensus protocol, not a
transport-side election result.

## 2. Required design model

The eventual multi-node mode is a replicated state machine, not a
multi-publisher extension of the v1 counter retry loop.

The design must specify a concrete consensus protocol, recommended as Raft or an
equivalent deterministic quorum protocol, including:

- member identity and membership change semantics;
- terms, voting, leader election, and leader lease semantics;
- log matching and append ordering;
- quorum commit and durability guarantees;
- snapshot and compaction safety;
- stale leader rejection and fencing.

`Dmn_DMesgNet` may be used only as the transport for consensus messages and
replicated lock intents. Its master-election state, membership metadata, or
transport-level delivery state is not itself a lock grant, release, lease,
expiry, or commit certificate.

## 3. Lock-authority invariants

The future `Dmn_DLock<Dmn_DMesgNet>` implementation must enforce all of the
following:

- a handler submits an immutable lock intent to the current consensus leader;
- neither the handler, leader, transport backend, nor receiving replica may
  declare success independently;
- the lock-table transition is applied in committed-log order on every replica;
- a lock grant is returned only after the log entry is quorum committed and
  applied;
- the public consensus fence is the ordered pair
  `(consensus_term, committed_log_index)`; it must be retained across snapshot
  and log compaction and compared lexicographically, not via a lossy scalar
  mapping;
- conflict retry rebases an uncommitted proposal but cannot overwrite committed
  state;
- handler close submits cancel/release intents through the same consensus log;
- lease expiry requires a consensus-safe time mechanism, such as quorum-confirmed
  leader time with bounded clock assumptions or replicated logical expiry ticks;
- partitions without quorum make no lock-table progress and never grant from a
  local mirror, even if the mirror appears uncontended.

## 4. Required future safety and recovery tests

Before enabling `Dmn_DLock<Dmn_DMesgNet>`, a separate consensus specification and
fault-injection test plan must prove the following:

- leader change safety;
- minority-partition non-progress;
- log reconciliation and replay safety;
- duplicate proposal idempotency;
- committed close-as-release semantics;
- stale-leader fencing and rejection;
- crash/restart persistence;
- membership change safety;
- quorum-safe lease expiry and release ordering;
- retry, retry-backoff, and replay correctness under delayed delivery.

## 5. Explicit phase boundary

The current `Dmn_DMesg` lock remains the authoritative implementation for this
repository phase. The `Dmn_DMesgNet` version is deferred and must not be enabled
until the consensus safety requirements above are proven in tests and the future
spec is accepted.
