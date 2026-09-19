# Implementation Plan: `Dmn_DLock` Publisher-Serialized v1

## 1. Delivery rule

Implement the distributed-lock specification as small, independently buildable
test-driven increments.  This plan replaces the manager/backend/standalone
authority approach: the only v1 serialization point is publisher acceptance
of a full lock table by one authoritative `Dmn_DMesg` publisher.

For every increment:

1. write one focused failing test;
2. add the smallest real API and implementation needed for it;
3. build the smallest affected target and run its completed tests;
4. run `git diff --check`;
5. do not add a method with placeholder success or an uncompilable future test.

No DLock implementation or test target currently exists.  Create tests and
their CMake registration only in the layer that supplies compilable production
code.  Tests use a manual clock, deterministic IDs/backoff jitter, explicit
handler-context drains, promises, and barriers.  Correctness tests never use
wall-clock sleeps.

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
       |
caller-only condition wait / retained owner query
```

The local full-table mirror MAY be represented internally as an interval tree
per session to accelerate overlap and eligibility queries. This internal
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

The first code layer deliberately changes `Dmn_DMesg` only because current
`openHandler()` hard-codes `Dmn_DMesgHandler`, whose private `Dmn_Async`
inheritance prevents a derived lock handler from posting work in its own
execution context.

Make precisely these additive changes:

1. protected non-template `openHandlerWithFactory(HandlerSpec,
   HandlerFactory)` path that normalizes constructor inputs and preserves the
   existing registration/playback/ownership path; the public forwarding
   `openHandler()` remains unchanged; and
2. protected `Dmn_DMesgHandler` scheduling wrapper for a `void()` job in its
   existing async context; and
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
cmake --build build --target dmn-test-dmesg dmn-test-dlock
ctest --test-dir build -R '^(dmn-test-dmesg|dmn-test-dlock)$' --output-on-failure
```

Use the repository's actual existing DMesg test target name if it differs;
never invent a duplicate target.  Final validation runs the lock target,
existing DMesg tests, and the `dmn` label.

## 5. Layer 0: baseline

1. Configure/build `dmn` and run existing `dmn`-label tests.
2. Record pre-existing failures without changing unrelated code.
3. Record the original diff names, because these specification files may
   already be modified.

**Exit:** baseline is known; no implementation/test file has been created.

## 6. Layer 1: DMesg extensibility seam

### Scaffold

Add the protected factory and protected scheduling wrapper with repository
documentation.  Keep the existing public `openHandler` call shape and default
factory path exact.  The factory must preserve registration, playback,
ownership, and close semantics for ordinary handlers.

### Tests, in strict order

1. `DmesgDefaultOpenHandlerBehaviorUnchanged` — public construction produces
   the existing behavior, including default handler type/lifetime.
2. `DmesgDefaultPlaybackFilterAndConflictUnchanged` — latest-message playback,
   filtering, normal writes, running-counter conflict, and conflict recovery
   behave as before.
3. `DmesgCustomHandlerPostsInOwnContext` — a test-only derived handler opened
   through the protected factory posts a job and the job runs after the
   handler's regular async delivery in the same handler context.
4. `DmesgCustomHandlerPublishCompletionIsNonblocking` — a custom handler
   receives accepted/conflict completion on its context without waiting there.
5. `DmesgDefaultProxyCloseUnchanged` — proxy copies and close retain their
   former invalidation/lifetime behavior.
6. `DmesgPublisherValidationRejectsBeforeCacheAndDelivery` — an opted-in
   validator rejects once, reports completion deterministically, and does not
   alter default-channel behavior.

The first two are regression tests for current behavior, not changed
expectations.  Test access must not expose async queues publicly.

**Exit:** lock code can derive a handler and post nonblocking per-handler
jobs; existing public DMesg behavior is proven unchanged.

## 7. Layer 2: values, full-table codec, and lock scaffold

Create the public lock header and lock-private implementation header/source
with only types and no operational placeholders:

- inclusive `Dmn_DLock_Range` validation/overlap;
- table entry/state/terminal reason, immutable request/session identities,
  table schema/version, lease/fence values, and result/event values;
- a v1 template constraint requiring exactly `Dmn_DMesg`, so
  `Dmn_DMesgNet` cannot instantiate the unsafe v1 algorithm;
- `template<class DMesgBase = Dmn_DMesg> class Dmn_DLock`;
- injected clock, deterministic ID/jitter interfaces, and private test access.
- define `Dmn_IntervalTree<T>` interface and tests for canonical enumeration
  and overlap queries; the lock-table codec uses canonical enumeration to
  serialize entries.

Add an additive lock-table protobuf/value codec only when the existing DMesg
payload shape requires it.  Preserve all old protobuf field numbers/enums and
test ordinary/sys payload round trips before adding lock fields.  The payload
is a full table; do not implement command/reply/authority messages.

Introduce `dmn-test-dlock` at this layer and add:

- `DlockInclusiveSharedEndpointConflicts`;
- `DlockAdjacentRangesDoNotConflict`;
- `DlockRejectsNegativeAndReversedRanges`;
- `DlockFullTableCodecRoundTrips`;
- `DlockExistingDmesgPayloadCompatibility`.

**Exit:** values, compatibility boundary, and full snapshot encoding compile;
no lock is acquired yet.

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
2. updates only its session-local mirror;
3. drafts/publishes a full candidate table through the one configured DMesg
   publisher; and
4. on publisher acceptance records the committed mirror/version and notifies
   only after unlocking.

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
- `DlockIntervalTreeCanonicalEnumerationMatchesWireOrder` — the interval-tree
  mirror enumerates entries in the same canonical order used by the protobuf
  full-table payload.

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
8. `DlockTimeoutFinalPredicateLetsCommittedGrantWin`.

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
