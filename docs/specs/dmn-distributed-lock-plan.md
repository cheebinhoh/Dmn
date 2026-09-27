# Implementation Plan: `Dmn_DLock` Publisher-Serialized v1

## Current repository status

This document is the implementation roadmap, not a statement that the design
below is already delivered. The repository currently contains a partial
`include/dmn-dlock.hpp` prototype, `src/proto/dmn-dlock.proto`, protobuf
oneof wiring, and the `dmn-test-dlock-*` targets. The prototype provides
snapshot value/codec helpers and a local mutex/condition-variable acquisition
path; it does **not** implement the publisher-serialized protocol described
below. In particular, snapshot publication is not wired to DMesg, sessions do
not own independent mirrors, and close/retry/lease/cleanup semantics are not
implemented. Do not describe v1 as complete or safe for distributed use until
the remaining layers and acceptance tests are implemented.

The DMesg seam is also partial: `openHandlerWithFactory()` and
`scheduleInHandlerContext()` exist but are currently public, not protected as
this design requires. Nonblocking publish completion and pre-cache publisher
validation hooks do not exist.

## 1. Delivery rule

Implement the distributed-lock specification as small, independently buildable
test-driven increments.  This plan replaces the manager/backend/standalone
authority approach: the only v1 serialization point is publisher acceptance
of a full lock table by one authoritative `Dmn_DMesg` publisher.

The implementation is intentionally Phase 1 only: a `Dmn_DLock<Dmn_DMesg>` that
serializes a canonical full lock-table snapshot over the DMesg transport.  This
plan does not permit a v1 `Dmn_DMesgNet` specialization or any use of
multi-publisher master election as a lock authority.  A future consensus-backed
transport is a separate design stream that must pass its own quorum, leader,
log, and fencing tests before being enabled.

For every increment:

1. write one focused failing test;
2. add the smallest real API and implementation needed for it;
3. build the smallest affected target and run its completed tests;
4. run `git diff --check`;
5. do not add a method with placeholder success or an uncompilable future test.

The prototype and initial test targets already exist; extend them only in the
layer that supplies compilable production behavior. New protocol correctness
tests should use a manual clock, deterministic IDs/backoff jitter, explicit
handler-context drains, promises, and barriers. Existing prototype tests
include wall-clock sleeps and are not deterministic protocol proofs; replace
those as the corresponding behavior is implemented. Correctness tests for the
completed protocol must not depend on wall-clock sleeps.

Current prototype test coverage includes protobuf table/message-body wire
round-trips; DLock range conversion and overlap semantics; snapshot validation,
candidate classification and sorting; proxy-copy behavior; handler task
posting and snapshot validation; invalid acquire ranges; no-wait conflict;
async acquisition; release/not-found; and snapshot-copy isolation. These tests
do not establish the publisher-serialized protocol or its distributed
lifecycle.

## 2. Target architecture

```text
application proxy
       |
shared SessionControl (immutable session id, atomic public gate)
       |
private Dmn_DLock session -- owns --> derived Dmn_DMesg handler
       |                                  |
       |                         handler async execution context
       |                                  |
local full-table mirror <----> one Dmn_DMesg publisher <----> sibling handlers
       |                                  |
       |                     single reserved lock-domain topic
       |                     canonical full-table DMesg snapshot payload
       |
caller-only condition wait / retained owner query
```

The lock-domain topic is a single reserved table channel.  Only lock-derived
handlers created by the same `Dmn_DLock` instance may publish to it, but the
publisher still validates the snapshot against the current committed version and
state machine before accepting it.  The table channel is not a per-request
command/reply stream, and it is not a source of authority by itself.

The local full-table mirror MUST be represented internally as a
`Dmn_IntervalBTree` per session to accelerate overlap and eligibility queries.
This internal
representation does not change the wire format: publication still carries a
canonical, sorted list of entries in a full snapshot.

`Dmn_DLock<DMesgBase = Dmn_DMesg>` derives from a DMesg-compatible base.
V1 implementation and tests use `Dmn_DLock<Dmn_DMesg>` only.  Do not add a
manager-global mirror, a backend, an authority service, command/reply topics,
or a separate authority protocol.  A `Dmn_DMesgNet` specialization may be
researched later, but it inherits no v1 multi-publisher/election/failover or
consensus safety claim.  The eventual distributed mode uses quorum consensus;
no individual backend or publisher is authoritative there.

Each session has its own mirror, predicate/version state, derived handler, and
immutable `session_id`.  The shared DMesg publisher is the sole v1 commit
authority.  Handler jobs create a candidate full table and publish it; accepted
publication commits it.  Conflict means another handler committed first:
consume the current full table, reapply immutable intent, and retry
asynchronously with bounded backoff.

## 3. Approved base-library seam

The initial DMesg seam is partly implemented: the handler factory and
handler-context scheduling wrapper are already present, but both are public
today and must be narrowed to protected access. The remaining work also
includes safe asynchronous publish completion and publisher-side validation
before cache mutation/delivery.

The extension goal is narrow and explicit: allow a lock-derived handler to post
jobs to its own DMesg callback context, observe publisher acceptance/conflict in
that context without waiting, and validate the lock-table payload before the
publisher mutates its cache or delivers to subscribers.  This is not a general
multi-transport API and not a way to bypass the single-authority design.

Complete and test the following additive seams:

1. **Present, visibility differs:** `openHandlerWithFactory(HandlerSpec,
   HandlerFactory)` normalizes constructor inputs and preserves registration,
   playback, and ownership. It is currently public but must be protected; the
   public forwarding `openHandler()` remains unchanged. Verify behavior with
   regression tests.
2. **Present, visibility differs:** `Dmn_DMesgHandler` has a public
   `scheduleInHandlerContext()` wrapper for a `void()` job in its existing
   async context. Narrow it to protected and test that it executes in the
   handler context.
3. protected nonblocking publish-completion hook for a job already in that
   context.  The public `writeAndCheckConflict()` waits after scheduling and
   therefore cannot be used from the handler context; the hook reports
   accepted/conflict asynchronously on that context; and
4. protected publisher validation hook, default-accepting for ordinary DMesg,
   called after counter validation and before cache/delivery.  The DLock
   override rejects an invalid lock-table CAS or state transition.

Do not expose queues, add synchronous execution, or alter public handler
signatures.  All other DMesg behavior remains unchanged.  The seams are not
optional implementation polish: without them the required per-handler job and
nonblocking commit-observation model cannot be implemented safely.

Do not expose `Dmn_Async::addExecTaskAfter()` as the retry mechanism.  Its
current not-yet-due requeue behavior can occupy a handler context and starve
incoming table notifications.  Delayed retries use a lock-specific,
session-owned cancellable timer that posts a job into the handler context only
when ready; the timer itself never accesses lock state.

## 4. Build and test commands

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dmn
ctest --test-dir build -L dmn --output-on-failure
```

When the relevant targets exist:

```bash
cmake --build build --target dmn-test-dlock-dmesg-seam dmn-test-dlock-1 dmn-test-dlock-2 dmn-test-dlock-3 dmn-test-dlock-4 dmn-test-dlock-5
cmake --build build --target dmn-test-dmesg-1 dmn-test-dmesg-2 dmn-test-dmesg-3 dmn-test-dmesg-5 dmn-test-dmesg-6 dmn-test-dmesg-7 dmn-test-dmesg-8 dmn-test-dmesg-9 dmn-test-dmesg-10 dmn-test-dmesg-11
ctest --test-dir build -R '^(dmn-test-dlock.*|dmn-test-dmesg-[0-9]+)$' --output-on-failure
```

The repository registers individual numbered DMesg and DLock test targets;
there is no aggregate `dmn-test-dmesg` or `dmn-test-dlock` target. Final
validation runs the DLock targets, relevant existing DMesg tests, and the
`dmn` label.

## 5. Layer 0: baseline

1. Configure/build `dmn` and run existing `dmn`-label tests.
2. Record pre-existing failures without changing unrelated code.
3. Record the original diff names, because these specification files may
   already be modified.

**Exit:** baseline is known; no new implementation or test file is created in
this layer.

## 6. Layer 1: DMesg extensibility seam

### Scaffold

Narrow the existing factory and scheduling wrapper to protected access and
document them. Keep the existing public `openHandler` call shape and default
factory path exact. The factory must preserve registration, playback,
ownership, and close semantics for ordinary handlers.

### Tests, in strict order

1. `DlockDmesgSeamDefaultOpenHandlerBehaviorUnchanged` — public construction produces
   the existing behavior, including default handler type/lifetime.
2. `DlockDmesgSeamDefaultPlaybackFilterAndConflictUnchanged` — latest-message playback,
   filtering, normal writes, running-counter conflict, and conflict recovery
   behave as before.
3. `DlockDmesgSeamCustomHandlerPostsInOwnContext` — a test-only derived handler opened
   through the protected factory posts a job and the job runs after the
   handler's regular async delivery in the same handler context.
4. `DlockDmesgSeamCustomHandlerPublishCompletionIsNonblocking` — a custom handler
   receives accepted/conflict completion on its context without waiting there.
5. `DlockDmesgSeamDefaultProxyCloseUnchanged` — proxy copies and close retain their
   former invalidation/lifetime behavior.
6. `DlockDmesgSeamPublisherValidationRejectsBeforeCacheAndDelivery` — an opted-in
   validator rejects once, reports completion deterministically, and does not
   alter default-channel behavior.

The first two are regression tests for current behavior, not changed
expectations.  Test access must not expose async queues publicly.

**Exit:** lock code can derive a handler and post nonblocking per-handler
jobs; existing public DMesg behavior is proven unchanged.

## 7. Layer 2: values, full-table codec, and lock scaffold

The public header, protobuf schema/oneof, basic snapshot codec, and initial
tests already exist. Review and complete these values and codec requirements
without duplicating the existing scaffold or treating its local acquisition
path as the distributed implementation:

- prefix C++ class and struct data members with `m_`; preserve protocol field
  identifiers and wire names unchanged;
- `Dmn_DLock_Range` as an alias or value-compatible wrapper around
  `Dmn_IntervalRange`, with DLock-specific non-negative validation;
- table entry/state/terminal reason, immutable request/session identities,
  table schema/version, lease/fence values, and result/event values;
- a v1 template constraint requiring exactly `Dmn_DMesg`, so
  `Dmn_DMesgNet` cannot instantiate the unsafe v1 algorithm;
- `template<class DMesgBase = Dmn_DMesg> class Dmn_DLock`;
- injected clock, deterministic ID/jitter interfaces, and private test access.
- use the existing `Dmn_IntervalBTree<T>` interface and tests for canonical
  enumeration, canonical reconstruction, callback registration, and both
  copying and visitor-based overlap queries; the lock-table codec uses
  `enumerateCanonical(lockDuplicateOrder)` to serialize entries. Use the
  default `(start, end)` range order; the duplicate evaluator orders only
  equal-range entries by `(priority, sequence, request_id)`. DLock entry values
  must be copy-constructible for snapshot enumeration and reconstruction;
  visitor-based overlap inspection avoids copies.

The existing `src/proto/dmn-dlock.proto` schema is already included in the
`DMesgBodyPb` oneof. Preserve all old protobuf field numbers/enums and add
compatibility tests for ordinary/sys payloads; the existing DLock tests
currently check message-body compatibility by value-copy, not a serialized
wire round trip. The payload remains a full table; do not implement
command/reply/authority messages.

The lock schema lives in the `DMesgBodyPb` oneof as an additive application
payload and MUST be validated as a full snapshot, not as a diff or command.  A
separate `dmn-dlock.proto` is acceptable only if it is inserted into the existing
payload oneof and all legacy field numbers remain untouched.

Extend the existing `dmn-test-dlock-1` target and add:

- `DlockInclusiveSharedEndpointConflicts`;
- `DlockAdjacentRangesDoNotConflict`;
- `DlockRejectsNegativeAndReversedRanges`;
- `DlockRangeAdapterPreservesEndpoints`;
- `DlockFullTableCodecRoundTrips`;
- `DlockExistingDmesgPayloadCompatibility`.

Related coverage already present in the numbered test files includes
`DlockRange.IntervalConversionPreservesEndpointsAndValidity`,
`DlockCanonicalProto.RoundTripTablePayload`,
`DlockLocalMirror.SnapshotRejectsInvalidDomainAndEntryData`,
`DlockLocalMirror.SnapshotRejectsEntriesFromAnotherDomain`, and
`DlockLocalMirror.CandidateClassificationCoversInvalidWaitingAndGranted`,
`DlockLocalMirror.CandidateFromAnotherDomainIsRejected`, and
`DlockLocalMirror.SnapshotCodecPreservesAllTableAndEntryFields`.

**Exit:** value/codec compatibility is covered. The existing local acquire
prototype is not considered a completed distributed lock; the later layers
must replace it with publisher-accepted full-table commits.

## 8. Layer 3: session/proxy lifetimes

Implement `openHandler()` for `Dmn_DLock` by opening a derived DMesg handler.
Create a separate, immutable `SessionControl` shared by all public proxy
copies.  It contains the immutable session ID and atomic public-open gate.
The private session owns the derived handler and persists independently of the
proxy.

Implement only construction and close state transitions:

```text
public-open -> closing-cleanup -> fully-closed
```

At this layer no cleanup mutation is admitted until the corresponding acquire
logic exists.  The transition nevertheless closes the public gate first; the
underlying handler stays retained by the private session.

Add:

- `DlockOpenCreatesOneDerivedDmesgHandler`;
- `DlockSessionLifetime.ClosingOneProxyCopyLeavesOtherCopiesLive`;
- `DlockSessionLifetime.HandlerPostRunsAndSnapshotValidationIsExposed`;
- `DlockSessionAssociationIsImmutableAfterPublicClose`;
- `DlockProxyCopiesCloseImmediately`;
- `DlockClosingOneSessionDoesNotInvalidateSibling`;
- `DlockOwnerQueryRetainedAfterHandlerClose`.

The final test starts with retained local terminal/query data.  It establishes
that query is on `Dmn_DLock`, not on a live handler proxy.

**Exit:** public and cleanup lifetime are distinct, and a strong in-flight
private session reference cannot bypass the public closed gate.

## 9. Layer 4: local mirror, deterministic table transition, and publication

Implement a lock-private transition function: input is one validated full
mirror plus an immutable intent; output is a complete candidate table.  It
enforces inclusive ranges, priority-then-sequence rank, no-wait conflict,
waiting eligibility, cancel/release, expiry, idempotency, and monotonically
increasing fences.  Every candidate carries the publisher incarnation,
base/new table versions, and persistent next-sequence/next-fence counters.  It
never reads global manager state.

Then implement a derived handler job which:

1. posts via the Layer 1 wrapper;
2. reads the session-local committed mirror without mutating it;
3. drafts/publishes a full candidate table through the one configured DMesg
   publisher; and
4. on publisher acceptance reconstructs/replaces the committed mirror and
   records its version, then notifies only after unlocking.

Candidate evaluation uses a separate candidate value/tree. A rejected or
conflicting candidate MUST leave the committed mirror and committed B-tree
unchanged. Snapshot delivery uses `reconstructFromCanonical()` only after
validation and acceptance.

The transition function uses `addWithTopology()` or `queryTopology()` to
identify affected ranges, but DLock applies lifecycle-state filtering and its
priority-then-sequence grant rule separately. A topology callback only
requests reevaluation; it never exposes a grant directly. Terminal entries
remain queryable but do not block eligibility.

Retain all committed entries in the B-tree so canonical reconstruction and
runtime callback registration cover the complete table. Before evaluating
topology or eligibility, visit overlaps with `forEachOverlapping()` and filter
by lifecycle state. Do not pass the unfiltered retained table to generic
topology as a grant decision.

All tree operations are confined to the session's handler execution context;
the interval tree is not thread-safe. State callbacks run synchronously after
the mutation completes, and callback exceptions do not roll back that
mutation. Keep them nonblocking, nonthrowing at the integration boundary, and
non-reentrant: schedule or mark reevaluation rather than mutating the tree
from inside the callback. Snapshot reconstruction suppresses tree callbacks
while loading, reconnects registrations from stable request IDs, and DLock
computes request notifications after the complete mirror has been replaced.
The received list is already canonical; reconstruction preserves its supplied
order, while later serialization must explicitly pass
`enumerateCanonical(lockDuplicateOrder)`.

Add:

- `DlockSinglePublisherCommitExcludesOverlap`;
- `DlockDeterministicPriorityThenSequence`;
- `DlockNoWaitConflictCreatesNoEntry`;
- `DlockSuccessfulCommitGrantsOnlyWhenEligible`;
- `DlockPublicationSynchronizesIndependentSessionMirrors`;
- `DlockTableVersionAdvancesOncePerAcceptedMutation`;
- `DlockFenceIncreasesForNewGrant`;
- `DlockPublisherRejectsInvalidTableTransition`;
- `DlockAllocatorsSurvivePruningAndBatchGrant`.
- `DlockIntervalBTreeCanonicalEnumerationMatchesWireOrder` — the interval-tree
  mirror enumerates entries in the same canonical order used by the protobuf
  full-table payload;
- `DlockCanonicalSnapshotRebuildPreservesEnumeration`;
- `DlockTerminalEntriesDoNotBlockEligibility`;
- `DlockTopologyCallbackSchedulesReevaluation`;
- `DlockCandidateMutationDoesNotAlterCommittedMirror`.
- `DlockOverlapInspectionDoesNotCopyEntries`.

Use a test publisher fixture with explicit delivery drains.  There is one
publisher, not a simulated second authority.

**Exit:** a no-wait and a waiting entry can commit through publisher acceptance
and synchronize all session mirrors.

## 10. Layer 5: conflict, retry, and caller-only waiting

Add conflict handling to the handler job, one test at a time:

1. `DlockPublishConflictConsumesAndRetries`;
2. `DlockConflictRetryPreservesImmutableRequestAndSession`;
3. `DlockRetryUsesBoundedDeterministicBackoff`;
4. `DlockRetryDelayDoesNotStarveTableDelivery`;
5. `DlockNewerTableReevaluatesWaitingRequest`;
6. `DlockCallerWaitHasNoMissedWakeup`;
7. `DlockWorkerNeverBlocksForCaller`;
8. `DlockTimeoutFinalPredicateLetsCommittedGrantWin`;
9. `DlockAcquireAsyncReturnsWithoutWaiting`;
10. `DlockInvalidArgumentsDoNotMutateTable`;
11. `DlockResultCodesAndOwnerAuthorization`;
12. `DlockProxyInvalidDereferenceThrows`;
13. `DlockMultiThreadBlockedAcquireWaitsUntilRelease` — a second `Dmn_Proc`
    thread attempting to acquire an overlapping range stays blocked while the
    grant is held and only proceeds after the owner releases it.  This test
    must use `Dmn_Proc` (not only static data) and an explicit release barrier
    to prove the blocked path does not advance early. The existing
    `DlockRealAcquire.SecondThreadBlocksUntilRelease` test uses fixed sleeps
    and does not yet provide this proof; replace it with the barrier-controlled
    test rather than counting it as complete coverage.
14. `DlockRealAcquire.InvalidRangesDoNotMutateAndMissingReleaseIsReported`;
15. `DlockRealAcquire.AsyncAcquireCompletesAndSnapshotIsAnIndependentCopy`.

On conflict the job consumes/validates the new complete table, replaces only
its local mirror, reapplies the same request entry, and schedules retry in the
same handler context.  It never force-writes a stale table.  Delay must be
cancellable and bounded with saturating exponential arithmetic.  A
session-owned timer performs only the delay and posts the ready retry into the
handler context; all table access and mutation remain on that handler context.

Implement blocking `acquireLock()` only now.  It posts the work then lets the
API caller wait on a condition predicate containing request terminal/granted
state, table version, retention version, and shutdown.  No handler job,
DMesg callback, or publisher path waits.

**Exit:** publication conflict converges and callers can wait without starving
the handler context or missing a notification.

## 11. Layer 6: cancellation, release, leases, and retained query

Introduce renewal, cancellation, and release as immutable table intents, all
processed by the same per-handler publish/conflict/retry path.  Renewal is
valid only for a live owned grant, preserves its fence, and loses to sticky
release intent.  Add maintenance as scheduled handler work for
acquisition/lease expiry and retention; it must also publish a whole table and
retry conflicts.

Tests:

- `DlockCancelWaitingRequest`;
- `DlockReleaseGrantedRequestIsIdempotent`;
- `DlockRenewPreservesFenceAndRespectsReleaseIntent`;
- `DlockLateGrantAfterTimeoutIsReleasedNotExposed`;
- `DlockLeaseExpiryReleasesAndReevaluates`;
- `DlockPlaybackPreservesOriginalExpiry`;
- `DlockLeaseAndFenceRejectStaleUse`;
- `DlockTerminalQueryRetainedThenPruned`;
- `DlockOwnerMismatchQueryIsNotFound`;
- `DlockConcurrentEquivalentReleaseJoinsOneIntent`.
- `DlockInvalidStateMutationsAreRejectedWithoutPublication`.

Persist compact immutable terminal/tombstone data so query and duplicate
release are meaningful after normal terminal completion.  The session ID
association must survive all of these transitions.

V1 stores absolute expiry ticks from the one injected process-local monotonic
clock shared by the DLock publisher and every session.  Do not encode only a
remaining duration: playback must not restart an expired lease.  This
representation is deliberately unavailable to multi-process DMesgNet mode.

**Exit:** normal acquire/cancel/release/expiry lifecycle is safe and
queryable, including no-undisclosed-grant cleanup.

## 12. Layer 7: close cleanup

Implement real `closeHandler(proxy) noexcept` only after Layer 6 has the
shared mutation mechanism.  Close must:

1. atomically close `SessionControl` before work is queued;
2. retain the private session and DMesg handler in closing-cleanup;
3. snapshot session-associated requests;
4. submit cancellation for waiting work and sticky release for granted or
   potentially granted work via an internal cleanup route;
5. explicitly bypass the **public** closed gate only for this cleanup route;
6. keep retrying publication conflicts under bounded policy; and
7. close the underlying DMesg handler only after cleanup commit or transport
   closure/finite-lease fallback.

Add deterministic barrier tests:

- `DlockCloseCancelsWaitersAndReleasesGrants`;
- `DlockCloseRacingGrantNeverExposesGrant`;
- `DlockExplicitReleaseThenCloseIsIdempotent`;
- `DlockCleanupBypassesClosedPublicGate`;
- `DlockClosePreservesRetainedQuery`;
- `DlockClosingOneSessionLeavesSiblingOperationsUsable`;
- `DlockCloseObservabilityAndRetainedQuery`.

For void close, add immutable events for close start, cleanup submission,
retry, commit, and deferred-to-lease result.  Verify event creation is under
state protection and observer delivery is after unlock; observer failure
cannot alter correctness.

**Exit:** public close is immediately visible to all proxy copies but does not
prematurely destroy the session required to serialize release/cancel commits.

## 13. Layer 8: shutdown and transport closure

Implement shutdown with explicit precedence:

1. linearize shutdown; close all public session gates and reject new public
   calls;
2. preserve already-admitted close cleanup and admit cleanup for still-open
   sessions;
3. wake caller waiters and allow one bounded collective cleanup window;
4. stop new maintenance/retries, close DMesg handlers/transport, and record
   uncommitted cleanup as finite-lease fallback;
5. reach fully-closed only after internal callback/task draining.

Add:

- `DlockCloseShutdownPrecedence`;
- `DlockShutdownWakesBlockedAcquire`;
- `DlockShutdownRejectsNewOperationsAndWakesWaiters`;
- `DlockShutdownCleanupUsesOneCollectiveWindow`;
- `DlockCloseAfterShutdownSubmitsNothing`;
- `DlockTransportClosureDefersCleanupToFiniteLease`;
- `DlockNoHandlerJobOrCallbackOutlivesDlock`;
- `DlockRetainedQueryAvailableAfterShutdown`.

The tests must exercise both orderings with barriers: close admitted before
shutdown uses the collective window; shutdown first wins against a later close.
Do not assert a committed release after the publisher was closed—assert the
documented lease/fence fallback and observability event instead.

**Exit:** teardown has no use-after-free, no sibling cross-effects, and no
untrue guarantee after transport closure.

## 14. Layer 9: protocol compatibility and regression

Add a final table publication compatibility test set:

- unknown/newer schema and malformed table rejection;
- older table ignored, equal-version structural divergence rejected, valid
  newer table atomically replacing each local mirror;
- existing `sys` and normal DMesg payloads still round-trip;
- playback bootstraps only validated table state;
- DMesg running-counter conflict leads to table resynchronization, not a
  forced stale overwrite.

Run:

```bash
cmake --build build
ctest --test-dir build -R '^dmn-test-(dmesg|dlock)' --output-on-failure
ctest --test-dir build -L dmn --output-on-failure
git diff --check
```

Inspect the complete diff to ensure it does not introduce a manager backend,
authority service, global mirror, command/reply topic scheme, or unsupported
DMesgNet safety assertion.

## 15. Future consensus-replicated mode

Do not implement `Dmn_DLock<Dmn_DMesgNet>` by replacing the v1 template
argument and reusing local publisher acceptance as commit.  First write and
approve a separate consensus protocol specification.  The recommended design
is a Raft-replicated deterministic lock-table state machine:

1. define persistent node identity, term/vote/log storage, leader election,
   AppendEntries/RequestVote messages, quorum commit, snapshots, and joint
   consensus membership changes;
2. transport those messages over `Dmn_DMesgNet` without using its current
   master election as consensus evidence;
3. submit immutable handler intents to the leader and return success only
   after the corresponding log index is quorum committed and applied;
4. expose fencing order as the lexicographically ordered
   `(term, committed_log_index)` pair and preserve it through compaction;
5. route handler-close cancel/release through the same committed log and retain
   cleanup state across retries and leader changes;
6. specify consensus-safe lease expiry before enabling leases--local process
   clocks cannot independently mutate replicated lock state.

Required deterministic fault tests include:

- minority partition cannot grant or mutate;
- one committed grant survives leader replacement without an overlapping
  grant;
- divergent uncommitted suffixes are reconciled without exposing grants;
- duplicate acquire/release/handler-close proposals are idempotent;
- handler close racing leader change commits release once or remains visibly
  pending;
- stale leaders and stale fences are rejected;
- a fence remains ordered across term change, log compaction, and restart;
- crash/restart restores term, vote, log, table, and deduplication state;
- membership change preserves quorum intersection.

This future layer has its own definition of done and is not part of the v1
build target.  Until it is complete, the documentation and API MUST reject or
leave unavailable the `Dmn_DMesgNet` specialization.

## 16. Definition of done

- Every DLock handler owns/uses a corresponding DMesg handler and all table
  jobs run in that handler's async context.
- Publisher acceptance of a full, versioned table is the only v1 commit point.
- Conflict consumes, reapplies, and asynchronously retries immutable intent.
- Only callers wait; predicates are missed-wakeup safe.
- Immutable session association, shared immediate proxy invalidation, private
  close cleanup, retained queries, and explicit close observability are proven.
- Close-vs-grant and close-vs-shutdown race tests are barrier-controlled.
- Existing DMesg behavior is proven unchanged except for the documented
  protected extension seam.
- The documentation and tests state the retained limitation: one authoritative
  Dmn_DMesg publisher only; Dmn_DMesgNet/election/multi-publisher safety is
  future consensus work, and no individual backend is authoritative in that
  future mode.
