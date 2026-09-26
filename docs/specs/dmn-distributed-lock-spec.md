# Feature Specification: DMN Distributed Range Lock (`Dmn_DLock`)

Status: revised for publisher-serialized v1 and consensus-replicated evolution.

## 1. Purpose and v1 boundary

`Dmn_DLock` provides exclusive, handler-scoped locks over inclusive integer
ranges.  It is a table-replication protocol built on **one authoritative
`Dmn_DMesg` publisher**, not a client/authority command service:

- each `Dmn_DLock` client session has one corresponding `Dmn_DMesg` handler;
- handlers keep private mirrors of one full, versioned lock table;
- a handler commits by publishing its candidate full table through the shared
  publisher; publisher running-counter/version conflict detection is the v1
  serialization and commit point;
- the accepted publication is delivered to all other handlers, which replace
  their mirrors and reevaluate their local wait predicates.

V1 is safe only while every participating lock handler uses the same configured,
authoritative `Dmn_DMesg` publisher for a domain.  `Dmn_DMesgNet` may
eventually be a compatible base/transport, but v1 makes **no** claim of
multi-publisher, election, failover, partition, or consensus safety.  In
particular, `Dmn_DMesgNet` master election MUST NOT be treated as a lock
authority.

The distributed target is different from this v1 serialization mechanism:
no individual backend or publisher is authoritative.  A consensus group
replicates the lock-table state machine, and only a quorum-committed log entry
may grant, renew, release, cancel, or expire a lock.  Section 10 defines that
evolution boundary.  Substituting `Dmn_DMesgNet` as the template argument does
not by itself cross that boundary.

There is no standalone `Dmn_DLock_Manager`, backend abstraction, authority
service, command/reply topology, or manager-global mirror in this design.
Retaining any of those as a second commit authority is forbidden.

## 2. Repository facts and required narrow `Dmn_DMesg` extension

Current `Dmn_DMesg::openHandler()` always constructs
`Dmn_DMesgHandler`.  That type privately inherits `Dmn_Async`, so a derived
lock handler cannot currently schedule arbitrary work in its own handler
execution context.  Therefore the current API is insufficient; an
implementation MUST NOT pretend otherwise.

The smallest deliberate, additive extensibility change is required before
`Dmn_DLock` is implemented:

1. add a protected, non-template `openHandlerWithFactory(HandlerSpec,
   HandlerFactory)` path that performs the existing registration, playback,
   owner wiring, and lifetime setup.  `HandlerSpec` normalizes name, topic,
   filter, callback, and configuration; the existing public forwarding
   `openHandler()` remains unchanged and continues to construct exactly
   `Dmn_DMesgHandler`;
2. expose on `Dmn_DMesgHandler` a protected scheduling wrapper that posts a
   `void()` task to that handler's existing `Dmn_Async` context;
3. expose the minimum protected, nonblocking publish-completion hook needed
   for code already running in that context to learn publisher acceptance or
   conflict.  Current public `writeAndCheckConflict()` waits after scheduling,
   so using it from that context would be unsuitable.  The hook must perform
   no caller wait and post its completion back to the handler context; and
4. add a protected publisher validation hook invoked in the publisher async
   context after the running-counter check succeeds but before cache mutation
   or subscriber delivery.  The default hook accepts unchanged.  The DLock
   override receives the source handler identity, verifies that it is a live
   lock-derived session of this DLock instance, validates the lock-channel
   candidate against the currently committed table, and rejects malformed,
   stale-base, skipped-version, unauthorized, or unsafe transitions.

The lock-specific derived handler uses only those protected seams.  Public
`Dmn_DMesg::openHandler()`/`closeHandler()` signatures and default behavior
remain unchanged.  Focused existing-DMesg regression tests must prove default
handler construction, playback, filtering, write/conflict behavior, proxy
invalidation, and async callback behavior are unchanged.  A separate
custom-handler test proves that only an opted-in derived handler can post to
its own context.  This is an explicit, narrowly scoped base-library exception;
all other lock coordination remains lock-specific.

The protected APIs use explicit value/callback contracts, not a virtual
forwarding template.  Publish completion is delivered exactly once, after
publisher validation and before any later publication from that source can
complete.  Shutdown either delivers the already-accepted completion or a
closed result; it never silently drops a completion while retaining the
session.

## 3. Model and invariants

### 3.1 Terms

| Term | Meaning |
|---|---|
| Publisher | The one configured `Dmn_DMesg` object that accepts the table publications for a domain. |
| Lock table | Complete snapshot of all active and retained terminal entries for a domain, carrying `table_version`. |
| Session | One immutable `Dmn_DLock` handler-session association and its corresponding `Dmn_DMesg` handler. |
| Public proxy | Copyable client handle to a session; it is not the cleanup lifetime. |
| Commit | Publisher acceptance of a candidate table under its running-counter conflict semantics. |
| Mirror | A session-local validated copy of the last committed table it has consumed or committed. |
| Request | One immutable acquire intent, identified by `request_id` and associated forever with a `session_id`. |
| Fence | Monotonically increasing token attached to a grant; external protected resources enforce it. |

### 3.2 Required invariants

- **Single publisher (I-1):** exactly one configured authoritative publisher
  accepts commits for a v1 domain.
- **Range exclusivity (I-2):** no committed table has overlapping granted
  ranges.
- **Immutable request/session association (I-3):** `request_id`,
  `session_id`, owner, range, priority, and initial acquisition deadline never
  change.  The publisher assigns the insertion sequence from the committed
  table's `next_sequence` when the request is first accepted; that sequence is
  immutable afterward.  A request remains associated with its session after
  the public proxy closes; it is not defined as belonging only to an *open*
  handler.
- **Versioned commit (I-4):** a successful state-changing publication advances
  `table_version` exactly once.  Every candidate carries
  `base_table_version`, and publisher validation requires the base to equal the
  current committed version and the candidate version to equal
  `base_table_version + 1`.  The DMesg running counter is an additional
  publisher acceptance precondition, not the table CAS.  Rejected candidates
  do not enter the cache or reach subscribers.
- **Deterministic eligibility (I-5):** contenders rank by ascending priority and
  then lower immutable insertion sequence.  A waiting entry grants only when
  it overlaps neither a grant nor a better-ranked active contender.
- **Worker nonblocking (I-6):** a handler execution context and DMesg callback
  never wait on a condition variable, API completion, retry delay, or I/O.
  Only an API caller may wait.
- **No undisclosed grant (I-7):** a grant that races timeout, cancellation,
  public close, or shutdown is never returned/exposed and is released by the
  same commit/retry mechanism while transport is retained.  A finite lease is
  the fallback after transport closure.
- **Session isolation (I-8):** closing one session cannot invalidate, mutate,
  or stop sibling sessions.
- **Idempotent intent (I-9):** acquire/renew/release/cancel identities are stable;
  duplicate local API calls and conflict retries cannot create a second
  request or release another request.

## 4. Ranges, entries, and table semantics

Ranges are signed 64-bit, non-negative, and inclusive.  A valid range satisfies
`0 <= start && start <= end`; overlap is
`a.start <= b.end && b.start <= a.end`.  Thus `[1,2]` conflicts with `[2,3]`,
while `[1,2]` does not conflict with `[3,4]`.

`Dmn_DLock_Range` is an alias or direct value-compatible wrapper around
`Dmn_IntervalRange`; both use the same inclusive `int64_t` endpoints and
overlap operation. DLock adds the domain constraint `start >= 0` through its
own validation before calling the generic B-tree. Conversion MUST preserve
both endpoints exactly and MUST reject negative or reversed ranges before any
tree mutation.

Each table entry contains, at minimum:

- domain, immutable request/session/owner ids, range, priority, and sequence;
- `waiting`, `granted`, or terminal state plus terminal reason;
- acquisition deadline/remaining duration, lease duration/remaining duration,
  and optional fencing token;
- immutable table protocol version and schema version.

The table header contains domain, schema/protocol version,
`publisher_incarnation`, `base_table_version`, `table_version`,
`next_sequence`, and `next_fencing_token`, followed by the full set of
entries.  It is a snapshot, never a delta.  Entries and table encoding must be
canonical enough that equal versions cannot describe different structural
state.  The allocator counters survive release, expiry, and terminal
compaction.  First accepted insertion consumes exactly one sequence.  Each
waiting-to-granted transition consumes one fencing token; when one transition
grants several entries, tokens are assigned in deterministic request-rank
order.

No-wait acquire either commits an immediately eligible grant or commits no
entry and returns conflict.  Waiting acquire commits a waiting entry; every
successful table mutation reevaluates all waiting entries in rank order and
may atomically grant multiple non-overlapping entries.  Release, cancellation,
acquisition expiry, and lease expiry similarly reevaluate eligibility.

V1 runs in one process and every session plus the publisher uses one injected
monotonic clock.  The table serializes checked absolute acquisition and lease
expiry ticks in that shared process-local clock domain.  Playback and a newly
opened handler therefore preserve the original deadlines rather than
restarting relative durations.  These ticks are never valid across processes
and are one reason the v1 table protocol cannot be enabled on `Dmn_DMesgNet`.
A new grant receives a strictly larger fence, and the header's next-token
counter prevents reuse after pruning.  Users of protected resources MUST
enforce `(publisher_incarnation, fencing_token)`; a lease alone cannot stop a
paused stale client.

### 4.1 Internal lock-table representation

Implementations MUST use the independent `Dmn_IntervalBTree` module (see
`dmn-interval-btree-spec.md`) per handler to store and query entries by range.
This external dependency guarantees:

- the interval B-tree is a pure in-memory representation; it does not change the commit authority or wire protocol;
- overlap queries and canonical enumeration operate over the same logical set
  of entries;
- DLock applies lifecycle-state filtering and deterministic eligibility rules
  to B-tree results; `Dmn_TopologyResult` is an initial geometric and
  priority evaluation, not the grant decision; and
- serialization to the protobuf full-table payload uses
  `enumerateCanonical(lockDuplicateOrder)` to produce a stable sorted order.

Canonical ordering is normative: every committed table version MUST serialize
to a byte-identical payload on all handlers. DLock MUST never use the B-tree's
default insertion-ordinal ordering for wire serialization. It supplies an
explicit duplicate comparator with this exact ordering:

1. ascending `range.start`;
2. ascending `range.end`;
3. ascending `priority`;
4. ascending immutable `sequence`;
5. ascending `request_id` as a final tie-breaker.

Equal table versions that differ in entry ordering or structure are protocol
errors. The interval B-tree is an internal optimization only; it does not alter
the definition of a lock table, mirror, or commit.

When a committed full table is received, the handler reconstructs its tree with
`reconstructFromCanonical()` using the received canonical list and the same
duplicate comparator. Runtime callbacks are reconnected through registrations
matching a stable request identity carried in each opaque entry value; callback
functions are never serialized.

`addWithTopology()` may initialize a newly submitted request as waiting or
eligible, but DLock grant state is decided by the complete transition
function. A request is granted only when it overlaps no granted entry and no
better-ranked active waiting contender. Terminal retained entries do not block
eligibility. A topology callback signals affected requests for reevaluation; it
does not grant a lock directly. Any grant, release, expiration, or cancellation
is included in a complete candidate table and becomes visible only after
publisher acceptance.

DLock MUST retain all committed entries in the reconstructed B-tree so
canonical snapshots and runtime callback registrations cover the complete
table. Before topology or eligibility decisions, it MUST use
`forEachOverlapping()` and filter results by lifecycle state. Terminal entries
remain in the tree for query/idempotency but are excluded from grant
eligibility; DLock MUST NOT rely on generic topology over the unfiltered
retained table.

## 5. `Dmn_DLock` API shape and session ownership

`Dmn_DLock` is a template deriving from a DMesg-compatible base, so a later
transport-compatible implementation can be investigated without changing the
lock facade:

```cpp
struct Dmn_DLock_Config {
  std::string domain;
  std::chrono::milliseconds default_lease;
  std::chrono::milliseconds retry_min_backoff;
  std::chrono::milliseconds retry_max_backoff;
  std::chrono::milliseconds close_cleanup_timeout;
  std::chrono::milliseconds retained_terminal_ttl;
};

using Dmn_DLock_Range = Dmn_IntervalRange;

struct Dmn_DLock_RequestOptions {
  std::string owner_id;
  int priority{};
  std::chrono::milliseconds wait_timeout{};
  std::optional<std::chrono::milliseconds> lease_duration;
  std::shared_ptr<std::atomic_bool> cancel_token;
};

struct Dmn_DLock_Result;
class Dmn_DLock_Handler;

class Dmn_DLock_HandlerProxy {
public:
  explicit operator bool() const noexcept;
  auto operator->() const -> std::shared_ptr<Dmn_DLock_Handler>;
};

template <class DMesgBase>
class Dmn_DLock : public DMesgBase {
public:
  using HandlerType = Dmn_DLock_HandlerProxy;

  explicit Dmn_DLock(std::string_view name, Dmn_DLock_Config config);
  auto openHandler(std::string_view name) -> HandlerType;
  void closeHandler(HandlerType &handler) noexcept;
  auto getRequestStateForOwner(std::string_view request_id,
                               std::string_view owner_id) const
      -> Dmn_DLock_Result;
  void shutdown() noexcept;
};

class Dmn_DLock_Handler {
public:
  auto acquireLock(Dmn_DLock_Range range,
                   const Dmn_DLock_RequestOptions &options)
      -> Dmn_DLock_Result;
  auto acquireLockAsync(Dmn_DLock_Range range,
                        const Dmn_DLock_RequestOptions &options)
      -> Dmn_DLock_Result;
  auto renewLock(std::string_view request_id, std::string_view owner_id,
                 std::chrono::milliseconds lease_duration)
      -> Dmn_DLock_Result;
  auto releaseLock(std::string_view request_id, std::string_view owner_id)
      -> Dmn_DLock_Result;
  auto cancelRequest(std::string_view request_id, std::string_view owner_id)
      -> Dmn_DLock_Result;
};
```

Result codes cover granted, waiting, released, conflict, timeout, cancelled,
not-owner, not-found, invalid-state, invalid-argument, handler-closed,
publisher-error, lease-expired, and shutdown.  Invalid proxy dereference throws
std::logic_error; an operation that entered before close but loses the
public-gate race returns handler-closed without a request id.  The proxy
control block is thread-safe even though concurrent mutation of the same proxy
object is not.

openHandler() returns a copyable lock proxy.  Behind every copy is a shared
SessionControl block containing an immutable generated session_id, an
atomic public gate, and a weak/public-safe route to private session state.
The actual session owns its derived DMesg handler, local table mirror,
condition variable, request association index, retry state, and in-flight
cleanup accounting.

The control block is shared by every proxy copy.  Consequently a close
atomically changes the public gate before any cleanup work is queued: all
copies observe closed immediately, including copies whose operation has an
in-flight strong reference to private session state.  A strong internal
reference does not authorize a new public submission.

Session lifetimes are distinct:

```text
public-open --close--> closing-cleanup --cleanup committed--> fully-closed
                     \--transport/shutdown closure--> fully-closed (lease fallback)
```

- **public-open:** public acquire/cancel/renew/release submission is allowed.
- **closing-cleanup:** all public proxies are invalid; a private retained
  session and its DMesg handler execute only cancellation/release/reconciliation
  work.
- **fully-closed:** cleanup is terminally committed, or publisher transport has
  closed and the finite lease fallback is recorded; the underlying DMesg
  handler may then close.

Every accepted request is permanently indexed by `session_id`, including
retained terminal query metadata.  `getRequestStateForOwner(request_id,
owner_id)` lives on `Dmn_DLock`, not a public proxy, and remains available
after handler close and after shutdown for the configured retention period.
Owner mismatch remains `kNotFound` for privacy.

## 6. Acquire, publish, conflict, and wait behavior

### 6.1 Submission

handler.acquireLock() validates synchronously, allocates immutable request
and operation identities, records the request in session-local pending-intent
state, and posts a nonblocking job through its corresponding DMesg handler's
protected scheduling wrapper.  Its sequence is tentative until the first
accepted insertion assigns and commits next_sequence; conflict rebase may
change only that tentative value.  It does not publish inline and it never
blocks the handler context.

The job:

    checks the public/cleanup gate appropriate to its intent;

    starts from the session-local newest committed full table;

    adds or reapplies the immutable request entry, uses B-tree topology to
    identify affected entries, applies lifecycle filtering and deterministic
    eligibility/expiry rules, and drafts a complete next table;

    publishes that complete, versioned candidate through the single DMesg
    publisher; and

    treats publisher acceptance as commit, atomically replaces the committed
    mirror, and wakes local waiters after releasing internal locks.

Candidate construction MUST use a separate value/object from the committed
mirror. A rejected, conflicting, or abandoned candidate MUST NOT mutate the
committed mirror, its B-tree, or committed request state. The B-tree's
`reconstructFromCanonical()` operation is used when a committed snapshot is
consumed; candidate evaluation may use a temporary B-tree or a canonical
value transition before acceptance.

Only the calling thread of a blocking API waits.  It waits on a condition
predicate that includes request terminal/granted state, mirror/table version,
retention version, and shutdown state.  The predicate is rechecked under the
same mutex after a timed wait to prevent missed wakeups.  Async acquisition
returns accepted/waiting immediately; no-wait may wait only in its API caller
for the bounded result rule, never in the job.

### 6.2 Conflict recovery

If publication conflicts, another handler committed first.  The job must not
overwrite or force-publish its stale draft.  It consumes the newer committed
full table, validates and replaces its own mirror, reapplies the immutable
request intent, and schedules a retry in the same handler context.  Retry is
bounded exponential backoff with injected deterministic jitter in tests,
saturating arithmetic, and acquisition/lease/close checks before every retry.

The delay itself MUST NOT execute in or occupy the handler context.  The
current Dmn_Async::addExecTaskAfter() repeatedly requeues a not-yet-due item
and can prevent that handler from consuming the very table update needed for
reconciliation.  A lock-specific, session-owned cancellable timer waits
outside the handler context and posts only the ready retry back through the
protected handler scheduling wrapper.  The timer never reads or mutates the
lock table.  Session close cancels ordinary retry timers but retains timers
needed by admitted close cleanup.

After its own successful commit, a request may be returned as granted only if
it is eligible/top in that committed table and its local lease is still valid.
Otherwise it remains waiting.  Later synchronized table changes cause the
handler to reevaluate and publish a newly eligible candidate.  A session
receiving another handler's accepted table replaces only its own mirror and
wakes/rechecks its own predicates; there is no manager-global mirror.

Publisher conflict handling must use the existing DMesg counter recovery
contract before retrying.  It must not block in a delivery callback or claim
that a counter itself is a consensus protocol.

### 6.3 Mutation identities and terminal behavior

Acquire, renew, release, and cancel intents have immutable IDs.  Retrying
after a publish conflict reuses the same logical intent.  Renewal is valid
only for a live owned grant, preserves its fence, and cannot extend a locally
known deadline after release intent is sticky.  A release entered before a
late acquire grant is sticky: the late grant is reconciled to a release and
never made visible.  Concurrent equivalent releases join one cleanup intent;
incompatible mutations return a defined invalid-state result without changing
the table.

Terminal entries are retained for query and idempotency for
retained_terminal_ttl; granted entries are never TTL-pruned.  Safe pruning
retains a compact immutable tombstone for at least the publisher process
lifetime unless a bounded replay window is separately specified.  Maintenance
uses the same publish/conflict/retry path as every table mutation.

## 7. Close and shutdown

closeHandler(proxy) is synchronous for public invalidation and bounded
cleanup admission, idempotent, and noexcept; its public return type is
void.  It is semantically release-all plus cancel-waiting for that session:

    atomically close the shared public gate, so every proxy copy is immediately
    invalid;

    under the session state mutex, snapshot immutable session-associated
    requests, mark waiting requests cancellation-pending, and make release
    intent sticky for granted or potentially granted requests;

    retain a private closing-cleanup session and its DMesg handler;

    post cancellation/release mutations through that handler.  These cleanup
    operations explicitly bypass the public closed-handler submission gate,
    but no other new work does;

    for each mutation, draft and publish the resulting complete table, consume
    conflicts, and retry with the same bounded policy;

    close the underlying DMesg handler only when cleanup reaches a committed
    terminal outcome, or when publisher transport is closed and the recorded
    finite lease fallback applies.

After admitting all cleanup as one batch, closeHandler() waits collectively
for at most close_cleanup_timeout, never one timeout per request.  It then
returns even if private cleanup must continue.  It MUST NOT be called from a
DMesg handler callback or handler task; such use is a contract violation
because synchronous public invalidation would re-enter that handler's
lifecycle.

If close races a grant, close wins visibility: the grant is not returned or
exposed, and its release mutation is retried.  Explicit release followed by
close is idempotent: retained terminal state prevents another table mutation.
Closing a session never affects sibling sessions or their handlers.

Because closeHandler() returns void, it must emit durable/inspectable events:
close_started, cleanup_cancel_submitted, cleanup_release_submitted,
cleanup_retry, cleanup_committed, and cleanup_deferred_to_lease.  Each
includes session/request ids, table version, retry attempt, and reason.
Retained query records expose the same terminal/cleanup projection.

Shutdown has precedence over new public work.  At shutdown linearization:

    the lock transitions to closing, closes every public session gate, and
    rejects operations not already accepted;

    cleanup already admitted by a handler close is preserved, and shutdown
    additionally admits release/cancel cleanup for every remaining session;

    all waiters are notified, and the implementation permits one bounded,
    collective cleanup window while the publisher remains usable;

    it stops accepting new retries/maintenance, closes underlying DMesg
    handlers/publisher transport, marks remaining cleanup as lease fallback,
    and reaches fully-closed.

Thus a close beginning after shutdown linearization does not reopen transport
or submit work; it observes shutdown/lease fallback.  A close that began
first contributes its already-admitted cleanup to shutdown's single collective
window.  Once transport has closed, no document or implementation may promise
a release commit; finite leases and fence enforcement are the explicit limit.

## 8. Wire compatibility and publication format

The lock publication is a DMesg payload containing a versioned full lock table,
not command/reply/authority-service messages.  Any protobuf addition is
additive: existing field numbers and enum values remain unchanged; all new
enums reserve zero as unspecified; checked durations are integer
milliseconds.  Existing sys and ordinary message bodies must round-trip
unchanged.

The topic/configuration is a single, explicit domain table channel bound to
the authoritative publisher.  It is not a collection of per-client command,
reply, or election topics, and no topic convention alone creates authority.
Only lock-derived handlers created by that Dmn_DLock instance may write this
reserved channel.  Publisher validation checks the canonical transition even
for such trusted writers; an ordinary DMesg handler cannot bypass it.
Raw user data is not concatenated unsafely into a topic.  Playback is a valid
bootstrap source only after schema/domain/version validation.  Older versions
are ignored; equal versions with structural differences are protocol errors;
newer valid snapshots replace the session-local mirror.

## 9. Deterministic test matrix

No DLock implementation or target exists today.  These tests are introduced
alongside the implementation stages; this specification does not require
uncompilable placeholder test files.  All timing tests use manual clocks,
deterministic IDs/jitter, explicit handler-context drains, promises, and
barriers—not wall-clock sleeps.

| Named test requirement | Required proof |
|---|---|
| `DmesgDefaultOpenHandlerBehaviorUnchanged` | Default factory remains `Dmn_DMesgHandler`; public API, playback/filter, proxy close, and write/conflict behavior are unchanged. |
| `DmesgCustomHandlerPostsInOwnContext` | The new protected factory/scheduling seam creates only an opted-in derived handler and runs its job in that handler context. |
| `DmesgCustomHandlerPublishCompletionIsNonblocking` | The protected completion hook reports accepted/conflict publication without a handler-context wait. |
| `DmesgPublisherValidationRejectsBeforeCacheAndDelivery` | A rejected custom validation never changes the cache or reaches subscribers; ordinary handlers retain default acceptance. |
| `DmesgDefaultProxyCloseUnchanged` | Existing DMesg proxy-copy invalidation and close behavior remain unchanged. |
| `DlockInclusiveSharedEndpointConflicts` | Inclusive overlap and adjacent non-overlap are correct. |
| `DlockRangeAdapterPreservesEndpoints` | Conversion between the DLock and interval range types preserves both endpoints and rejects negative or reversed ranges before mutation. |
| `DlockSinglePublisherCommitExcludesOverlap` | Accepted full tables never grant overlapping ranges. |
| `DlockPublisherRejectsInvalidTableTransition` | Stale-base, skipped-version, overlapping-grant, allocator-regression, and unauthorized lock-channel publications never enter cache or delivery. |
| `DlockDeterministicPriorityThenSequence` | Priority then immutable sequence determines eligible contenders. |
| `DlockPublishConflictConsumesAndRetries` | A losing handler consumes the newer table, reapplies the same request, and retries without force overwrite. |
| `DlockSuccessfulCommitGrantsOnlyWhenEligible` | A successful request commits waiting when not top and later grants only after synchronized change. |
| `DlockTopologyCallbackSchedulesReevaluation` | A topology change signals reevaluation without exposing a grant before a publisher-accepted table transition. |
| `DlockTerminalEntriesDoNotBlockEligibility` | Retained terminal entries remain queryable but do not block a new eligible request. |
| `DlockCanonicalSnapshotRebuildPreservesEnumeration` | Reconstructing a handler tree from canonical entries reproduces canonical ordering and reconnects runtime callbacks without load-time dispatch. |
| `DlockOverlapInspectionDoesNotCopyEntries` | Eligibility inspection can visit B-tree entries by const reference without copying payloads. |
| `DlockCandidateMutationDoesNotAlterCommittedMirror` | A rejected/conflicting candidate leaves the committed mirror unchanged until publisher acceptance. |
| `DlockCallerWaitHasNoMissedWakeup` | Notification-before-sleep and final predicate checks cannot hang. |
| `DlockWorkerNeverBlocksForCaller` | Another request progresses while an API caller waits. |
| `DlockAcquireAsyncReturnsWithoutWaiting` | Asynchronous acquire returns without blocking the caller or handler context. |
| `DlockInvalidArgumentsDoNotMutateTable` | Invalid ranges, empty owners, invalid durations, and malformed identities return the documented result without publication. |
| `DlockResultCodesAndOwnerAuthorization` | Granted, waiting, conflict, timeout, cancelled, not-owner, not-found, invalid-state, handler-closed, publisher-error, lease-expired, and shutdown results are distinguishable and owner checks are enforced. |
| `DlockProxyInvalidDereferenceThrows` | Dereferencing an invalid handler proxy throws `std::logic_error`. |
| `DlockOpenCreatesOneDerivedDmesgHandler` | Each lock session owns exactly one corresponding derived DMesg handler. |
| `DlockSessionAssociationIsImmutableAfterPublicClose` | Request/session association remains queryable and immutable after public close. |
| `DlockProxyCopiesCloseImmediately` | Every proxy copy rejects public calls immediately despite retained/in-flight session references. |
| `DlockCloseCancelsWaitersAndReleasesGrants` | Close publishes cancellation/release table changes and leaves sibling sessions usable. |
| `DlockCloseRacingGrantNeverExposesGrant` | Barrier-controlled grant/close race releases the grant and never returns it. |
| `DlockExplicitReleaseThenCloseIsIdempotent` | No second release-table mutation is committed. |
| `DlockCancelWaitingRequest` | Cancelling a waiting request commits a terminal cancellation and reevaluates other contenders. |
| `DlockReleaseGrantedRequestIsIdempotent` | Repeated release of one owned grant produces one logical release transition. |
| `DlockLateGrantAfterTimeoutIsReleasedNotExposed` | A grant racing timeout is never exposed and is reconciled through release. |
| `DlockLeaseExpiryReleasesAndReevaluates` | Lease expiry releases the grant and reevaluates eligible waiters through publication. |
| `DlockTerminalQueryRetainedThenPruned` | Terminal query metadata remains available through retention and is safely pruned afterward. |
| `DlockOwnerMismatchQueryIsNotFound` | An owner mismatch does not reveal request state. |
| `DlockConcurrentEquivalentReleaseJoinsOneIntent` | Equivalent concurrent releases coalesce without duplicate table mutations. |
| `DlockCleanupBypassesClosedPublicGate` | Private cleanup runs after proxy invalidation; ordinary public submission cannot. |
| `DlockCloseObservabilityAndRetainedQuery` | Void close emits lifecycle/cleanup outcomes and owner query remains available after close. |
| `DlockCloseShutdownPrecedence` | Close-before-shutdown uses the collective cleanup window; close-after-shutdown records lease fallback and submits nothing. |
| `DlockShutdownRejectsNewOperationsAndWakesWaiters` | Shutdown rejects newly submitted work and wakes all blocked callers with shutdown state. |
| `DlockNoHandlerJobOrCallbackOutlivesDlock` | Teardown drains or invalidates all handler jobs and callbacks before destruction. |
| `DlockTransportClosureDefersCleanupToFiniteLease` | Transport closure records lease fallback and never claims an uncommitted release. |
| `DlockLeaseAndFenceRejectStaleUse` | Lease expiry and monotonic fencing protect a fence-enforcing resource model. |
| `DlockPlaybackPreservesOriginalExpiry` | After original sessions disappear and the manual clock passes expiry, a newly opened handler cannot revive the played-back grant. |
| `DlockAllocatorsSurvivePruningAndBatchGrant` | Sequence/fence counters never regress across release, expiry, pruning, or a transition granting multiple entries. |
| `DlockRenewPreservesFenceAndRespectsReleaseIntent` | Renewal is serialized as a table mutation, preserves the fence, and cannot win over close/release. |
| `DlockVersionAndProtobufCompatibility` | Full-table version validation and existing DMesg protobuf payload compatibility hold. |

The implementation plan further decomposes this matrix with explicit tests
for adjacent and invalid ranges, codec round trips, independent mirror
synchronization, no-wait conflict, version advancement, fencing allocation,
conflict backoff and delivery starvation, final timeout predicates, retained
queries, sibling isolation, shutdown ordering, and invalid-state mutations.

## 10. Future consensus-replicated mode

The eventual multi-node mode is a replicated state machine, not a
multi-publisher extension of the v1 counter retry loop:

- a concrete consensus protocol, recommended as Raft, defines member identity,
  terms, voting, leader election, log matching, quorum commit, membership
  changes, and durable term/vote/log state;
- `Dmn_DMesgNet` is only a transport for consensus messages; its existing
  membership or master-election result is not a vote, term, leader lease, or
  commit certificate;
- handlers submit immutable lock intents to the current consensus leader, but
  neither the handler, leader, transport backend, nor receiving publisher may
  declare success independently;
- the lock-table transition is deterministic and is applied in committed-log
  order on every replica; a handler returns a grant only after the entry's log
  index is quorum committed and applied;
- the public consensus fence is the ordered pair
  `(consensus_term, committed_log_index)`; it is retained across snapshot/log
  compaction and compared lexicographically without a lossy scalar mapping;
- conflict retry rebases an uncommitted proposal, but cannot overwrite or
  supersede committed state;
- handler close submits cancel/release intents through the same consensus log
  and retains its private cleanup session until commit or transport loss;
- lease expiry requires a separately specified consensus-safe time mechanism,
  such as quorum-confirmed leader time with bounded-clock assumptions or
  replicated logical expiry ticks.  A process-local timer alone MUST NOT
  release a distributed lock;
- partitions without quorum make no lock-table progress.  They never grant
  from a local mirror, even if that mirror appears uncontended.

A separate consensus protocol specification and fault-injection test plan are
required before enabling `Dmn_DLock<Dmn_DMesgNet>`.  At minimum they must prove
leader-change safety, minority-partition non-progress, log reconciliation,
duplicate proposal idempotency, committed close-as-release, stale-leader
fencing, crash/restart persistence, and membership-change safety.

## 11. External Interval B-Tree Dependency

Dmn_DLock relies strictly on the independent Dmn_IntervalBTree<T> module to maintain the local lock-table mirror.
Dmn_DLock does not redefine interval storage or logic internally.

- `addWithTopology()` and `queryTopology()` identify geometric overlap and
  priority relationships for affected requests; DLock then filters terminal
  entries and applies its grant rule.
- DLock uses `forEachOverlapping()` when it only needs to inspect entries;
  copy-returning overlap results are used only when ownership of a snapshot
  is required.
- Priority evaluation inside the B-tree ranks active contenders by ascending
  priority and lower immutable sequence, while DLock remains authoritative for
  lifecycle-state filtering and grant transitions.
- Canonical serialization delegates to
  `enumerateCanonical(lockDuplicateOrder)`, never to the default
  insertion-ordinal ordering.

Refer to `dmn-interval-btree-spec.md` for the precise structural API and
topological matrix.

### 11.1 Determinism and isolation

Each `Dmn_DLock` session owns exactly one interval tree instance. The tree is
never shared across sessions and never used as a global manager or authority.
All mutation and query operations run exclusively on that session’s handler
execution context and MUST NOT block.

After a committed snapshot is received, the session rebuilds its tree from the
canonical entry list in that list's order. The rebuild is callback-suppressed;
registered callbacks are reattached by stable request ID, and DLock computes
net request-state notifications only after the complete mirror is consistent.

Two handlers with equal logical lock-table state MUST produce byte-identical
protobuf payloads, regardless of differences in their interval-tree internal
structure. Equal table versions that serialize differently are protocol errors.

### 11.2 Relationship to the lock-table snapshot

The interval tree is an optimization for:

- overlap detection,
- eligibility evaluation,
- deterministic ranking,
- and local mirror maintenance.

It does not change:

- the full-table snapshot wire format,
- the commit point (publisher acceptance),
- the deterministic transition rules,
- or any invariant in Section 3.2.

All committed state continues to be defined solely by the canonical full-table
payload delivered by the publisher. The interval tree MUST always reflect the
session-local mirror of that committed table.

## 12. Acceptance criteria

The feature is complete only when the test matrix passes, normal DMesg
regressions pass, `git diff --check` is clean, and documentation says exactly
what v1 guarantees: handler-local mirrors converge through one authoritative
`Dmn_DMesg` publisher.  Multi-publisher operation, DMesgNet election, and
automatic failover are not enabled by v1.  A future network specialization is
complete only after the Section 10 consensus requirements and their separate
tests pass; no individual backend is authoritative in that mode.
