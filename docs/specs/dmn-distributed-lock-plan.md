# Implementation Plan: DMN Distributed Range Lock

## 1. Goal and Working Method

Implement `docs/specs/dmn-distributed-lock-spec.md` in small, buildable slices.
Every increment ends with a focused compile/test checkpoint. The sequence first
builds and proves the authority core, then the client manager, and only then the
DMesg transport.

Do not implement transport and concurrency at the same time. Do not add
wall-clock sleeps to tests. Use fake clocks, deterministic jitter, promises,
barriers, and backend fault hooks.

Within each increment:

1. add the named failing tests;
2. add only the production surface needed by those tests;
3. build the smallest affected target;
4. run the focused test target;
5. run all lock targets completed so far;
6. inspect the diff for accidental coupling to `Dmn_DMesgNet` or the
   process-wide runtime.

No increment may leave a declared public API with placeholder success
behavior. A temporarily unsupported path returns an explicit error and is
covered by a test.

## 2. Baseline Commands

Use the existing build directory when correctly configured. Otherwise:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dmn
ctest --test-dir build -L dmn --output-on-failure
```

After the first lock test target exists:

```bash
cmake --build build --target dmn-test-dlock-authority dmn-test-dlock
ctest --test-dir build -R '^dmn-test-dlock(-authority)?$' --output-on-failure
```

For one GoogleTest:

```bash
./build/test/dmn-test-dlock-authority \
  --gtest_filter='DmnDLockAuthority.ExactTestName'
```

Confirm the executable location with:

```bash
ctest --test-dir build -N -V -R '^dmn-test-dlock-authority$'
```

## 3. Design Decisions to Preserve

1. `Dmn_DLock_Manager` is shared-owned but not a singleton.
2. One explicitly configured authority exists per domain; v1 has no automatic
   authority failover.
3. The authority, not the client mirror or DMesg running counter, commits lock
   order.
4. Ranges are inclusive and conflict by overlap.
5. Multiple non-overlapping ranges may be granted.
6. All accepted waits and grants are bounded by acquisition expiry and lease
   expiry respectively.
7. Every grant returns
   `(authority_generation, authority_nonce, fencing_token)`.
8. Manager workers and transport callbacks never run the blocking API wait
   loop.
9. Protocol commands are idempotent by stable `operation_id`.
10. Tests control time and randomness.
11. A caller timeout or lost no-wait response cannot abandon an undisclosed
    authority grant; reconciliation submits a cleanup release.
12. V1 uses `Dmn_DMesg`; future `Dmn_DMesgNet` support is implemented behind
    the same backend contract and never changes lock semantics.
13. DMesg command topics are client-specific because current running counters
    assume one coordinated writer per topic.
14. Acquisition deadlines begin at local request acceptance; bootstrap and
    retries never extend them.
15. Authority policy independently caps acquisition and lease durations, and
    authority maintenance belongs to the backend/service that owns the
    authority.

## 4. Planned Dependency Direction

Keep dependencies one-way:

```text
public manager -> backend interface <- in-memory backend
                               \---- DMesg backend -> Dmn_DMesg

DMesg backend -> protocol conversion -> generated protobuf
in-memory backend -> authority core
DMesg authority service -> authority core
authority core -> value/protocol types + injected clock/id source
```

The authority core does not include DMesg headers. The manager does not include
authority or protobuf headers. `Dmn_DMesgNet` is absent from v1 lock source
files. This boundary is the future-extension checkpoint: a network backend can
reuse the protocol and manager without altering authority rules.

## 5. Increment 0: Record the Baseline

### Change

- configure the existing project;
- build `dmn`;
- run the current `dmn`-label tests;
- record pre-existing failures separately and do not fix unrelated behavior.

### Checkpoint

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dmn
ctest --test-dir build -L dmn --output-on-failure
```

No lock files are added in this increment.

## 6. Increment 1: Public Value Types and Validation

### Test first

Create `test/dmn-test-dlock.cpp` with tests for:

- inclusive overlap;
- adjacent non-overlap;
- invalid ranges.

Register `dmn-test-dlock` in `test/CMakeLists.txt`.

### Implement

Create `include/dmn-dlock.hpp` with:

- `Dmn_DLock_Range`;
- constexpr `Dmn_DLock_Range::isValid()` and `overlaps()`;
- public state/reason/result enums;
- entry, config, and request option structures;

Do not add threads or a manager implementation yet. Add the header to the
`dmn` target's interface sources and `include/dmn.hpp`.

Configuration and request API validation tests intentionally wait until
Increment 7, where `Dmn_DLock_Manager::create()` and request methods exist.
This keeps the first test target linkable without exposing validation helpers
solely for tests.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock
ctest --test-dir build -R '^dmn-test-dlock$' --output-on-failure
```

Expected capability: range value semantics and overlap/validity rules compile
and pass.

## 7. Increment 2: Authority Data Model and Deterministic Ordering

### Test first

Create `test/dmn-test-dlock-authority.cpp`. Add tests for:

- priority then sequence rank;
- non-overlapping requests grant concurrently;
- overlapping requests grant one at a time;
- an earlier overlapping waiter prevents bypass;
- granted snapshots contain no overlapping ranges.

Register the new target.

### Implement

Create:

- `include/dmn-dlock-protocol.hpp`;
- `include/dmn-dlock-authority.hpp`;
- `src/dmn-dlock-authority.cpp`.

Implement a single-threaded authority core initially:

- transport-neutral command, reply, and snapshot value types;
- request and operation records;
- `request_id`-keyed active and retained-terminal indexes;
- ordered waiting traversal by `(-priority, sequence)`;
- granted-range traversal used only for overlap checks;
- overlap helper reuse;
- grant reevaluation;
- snapshot generation.

Inject a `Dmn_DLock_Clock` interface or callable. Do not add locking yet; tests
drive the authority synchronously.

Wire the source into `src/CMakeLists.txt`.

Create header-only `test/dmn-test-dlock-fakes.hpp` for the manual clock,
deterministic IDs, and later barriers/fake backends. The repository's
`ADD_TEST_EXECUTABLE` macro compiles exactly one `.cpp` per test target, so
shared test support must be header-only unless the CMake macro is deliberately
extended.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock-authority
ctest --test-dir build -R '^dmn-test-dlock-authority$' --output-on-failure
```

Expected capability: a deterministic in-process authority can order and grant
range requests.

## 8. Increment 3: Versioning and Idempotent Commands

### Test first

Add one test at a time for:

- first accepted create assigns sequence 1 and table version 1;
- every new logical mutation increments table version once;
- duplicate `operation_id` returns the cached result without mutation;
- stale expected version returns a conflict plus current full snapshot;
- a version-conflict retry reuses its operation id, changes only
  `expected_table_version`, and commits;
- duplicate create preserves sequence;
- replay with changed immutable fields is rejected.

### Implement

Create `include/dmn-dlock-backend.hpp` containing the abstract backend
interface over the protocol types introduced in Increment 2. Add authority
command dispatch:

- validate;
- deduplicate;
- compare expected version;
- mutate and reevaluate;
- increment version;
- cache response;
- return full snapshot.

Use one stable operation id per logical mutation. Cache only committed or
permanently rejected outcomes; do not finalize an operation id on
`kVersionConflict`. Store immutable operation payload identity separately from
retry metadata so a retry may update only `expected_table_version`. Keep ID
generation behind an injectable function.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock-authority
ctest --test-dir build -R '^dmn-test-dlock-authority$' --output-on-failure
```

Expected capability: duplicate/reordered client attempts cannot duplicate
authority mutations.

## 9. Increment 4: No-Wait, Cancel, and Release Authority Operations

### Test first

Cover:

- immediately grantable no-wait create;
- no-wait conflict creates no record, sequence, fence, or version;
- duplicate delivery of a final no-wait conflict remains conflict after the
  range becomes free;
- cancel waiting request;
- cancel granted request is invalid;
- release granted request;
- repeat release is idempotent;
- owner mismatch;
- release/cancel reevaluates waiting requests.

### Implement

Add create mode and cancel/release commands. Keep grant reevaluation in one
authority helper called after all mutations that can unblock contenders.

Do not add the manager API yet.

### Checkpoint

Build and run only `dmn-test-dlock-authority`, then run both lock targets.

Expected capability: the full non-lease authority lifecycle is testable without
networking or threads.

## 10. Increment 5: Leases, Renewal, Acquisition Expiry, and Fencing

### Test first

Using a manual clock, cover:

- first grant receives a fence;
- later grant receives a larger fence;
- renewal extends from authority time and preserves the fence;
- waiting/expired/foreign renewal is rejected;
- lease expiry releases the range and grants the next request;
- waiting acquisition expiry removes the contender and reevaluates grants;
- authority restart creates a different epoch;
- higher authority generation supersedes the old epoch;
- lower/retired generation messages are ignored;
- same generation with a different nonce is rejected;
- authority configuration and over-limit acquisition/lease durations are
  rejected without mutation;
- one maintenance pass that expires multiple records increments the table
  version once.

### Implement

Add:

- validated `Dmn_DLock_Authority_Config`;
- deployment-supplied, monotonically increasing authority generation plus a
  random per-process nonce;
- fencing counter;
- lease deadline;
- renew command;
- authority `runMaintenance()` entry point;
- authority-side acquisition deadline fixed on first accepted create;
- lease-expiry terminalization and reevaluation.

Never use client wall-clock timestamps for an authority decision.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock-authority
./build/test/dmn-test-dlock-authority \
  --gtest_filter='DmnDLockAuthority.*Lease*:DmnDLockAuthority.*Fence*'
ctest --test-dir build -R '^dmn-test-dlock-authority$' --output-on-failure
```

Expected capability: stale holders can be superseded and identified by fence.
The test resource compares `(generation, token)` and pins the nonce registered
for that generation.

## 11. Increment 6: In-Memory Backend

### Test first

Add backend tests, initially in `dmn-test-dlock-authority.cpp`, for:

- start and snapshot callback;
- asynchronous submit callback;
- local accepted/retryable/permanent/closed submission classification;
- duplicate reply injection;
- conflict then snapshot;
- shutdown rejects new submission and delivers no later callback;
- concurrent submit calls are serialized at authority entry.

### Implement

Create `src/dmn-dlock-backend-memory.cpp`.

The backend owns/uses one authority and adapts command results to callback
delivery. Provide deterministic test mode where callbacks are explicitly
drained by the test. Implement the final typed `Dmn_DLock_Submit_Result`
contract now so later manager code does not infer transport errors from
exceptions or missing callbacks.

Protect authority entry with a mutex or confine it to the backend executor;
command and maintenance transactions must never overlap.
Schedule authority maintenance on that same executor, with an explicit manual
clock wake/drain hook for deterministic tests.

Do not make the authority itself depend on this backend.

### Checkpoint

Build and run the authority target.

Expected capability: a backend boundary exists and can simulate delivery
behavior without DMesg.

## 12. Increment 7: Manager Skeleton and Local Lifecycle

### Test first

Extend `dmn-test-dlock.cpp`:

- factory rejects empty identifiers and invalid duration/backoff/jitter;
- request APIs reject invalid range, owner, timeout, expiry, and lease;
- defaults or explicit request durations above configured maxima;
- factory validates config/backend;
- multiple manager instances are independent;
- async valid request returns `kWaiting` and a request id;
- invalid/local-submission-failed async request has no request id;
- query projects waiting state;
- shutdown is idempotent.

### Implement

Create `src/dmn-dlock.cpp` and declare the manager factory/API.

Add:

- shared ownership factory;
- lifecycle mutex and request map;
- shutdown gate;
- stable request/operation ID generation;
- backend startup callback using `weak_ptr`;
- local acceptance rollback if queue submission fails.

For this increment, use a minimal immediate executor abstraction with no retry
or delay support.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock
ctest --test-dir build -R '^dmn-test-dlock$' --output-on-failure
```

Expected capability: accepted async lifecycle can be created and queried.

## 13. Increment 8: Snapshot Application and Async Grant

### Test first

Cover:

- newer snapshot atomically replaces the mirror;
- older snapshot is ignored;
- structurally identical duplicate version is harmless and may only shorten a
  local deadline;
- structurally divergent same-version snapshot is a protocol error;
- first snapshot completes bootstrap and releases queued async submission;
- lower/retired generation is ignored after restart;
- same-generation foreign nonce is a protocol error;
- wrong domain/authority is ignored and reported;
- invalid overlapping-grant snapshot terminalizes with publisher error;
- authority epoch change terminalizes old requests;
- a snapshot-discovered grant remains locally waiting until a correlated
  create reply establishes lease validity;
- that snapshot observation replays the original create operation id for fresh
  lease-remaining metadata;
- owner-safe query returns `kNotFound`.

### Implement

Add one snapshot application function:

1. validate outside mutation where possible;
2. acquire lifecycle mutex;
3. compare identity/epoch/version;
4. replace mirror atomically;
5. project local lifecycle outcomes without exposing a newly snapshot-only
   grant;
6. release mutex;
7. notify waiters and emit events.

Do not update individual mirror entries piecemeal.

### Checkpoint

Build and run both lock targets.

Expected capability: async acquisition exposes `kGranted` only after a
correlated reply establishes a conservative local lease deadline.

## 14. Increment 9: Blocking and No-Wait Public APIs

### Test first

Cover:

- synchronous no-wait grant;
- synchronous no-wait conflict creates no retained request;
- blocking request grants;
- notification immediately before wait does not hang;
- final predicate observes a simultaneous grant;
- timeout returns request id and queues cancel;
- a no-wait response timeout returns publisher error with a request id;
- a late no-wait or blocking grant is not exposed and queues cleanup release;
- cleanup cancel returning `kInvalidState` creates a cleanup release with a new
  operation id;
- cleanup cancel returning `kNotFound` retains the tombstone and replays the
  original create operation id until its final outcome is known;
- another async request progresses while an API thread waits.

### Implement

Implement the Section 11 predicate/version wait loop. The transport reply and
snapshot callback only update state and notify. They never run the wait loop.

For no-wait mode, use a request-local promise/future for the single authority
response bounded by `authority_response_timeout`. A no-wait ordering conflict
must not enter the ordinary retained lifecycle map. A response timeout keeps a
cleanup tombstone when delivery was possible so a later reply or snapshot can
release an undisclosed grant.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock
./build/test/dmn-test-dlock \
  --gtest_filter='DmnDLockManager.*NoWait*:DmnDLockManager.*Blocking*'
ctest --test-dir build -R '^dmn-test-dlock$' --output-on-failure
```

Expected capability: all three acquisition entry modes work against the
in-memory authority.

## 15. Increment 10: Cancellable Delayed Executor and Retry

### Test first

Cover:

- version conflict schedules retry;
- retry reuses operation id;
- exponential delay saturates at max;
- deterministic jitter stays within bounds;
- snapshot refresh updates expected version;
- terminal request cancels retry;
- terminal request preserves only required cancel/late-grant cleanup;
- dropped response retries;
- permanent backend error does not retry.

The manager normally omits the optional version precondition. Exercise the
version-conflict retry path by configuring the fault backend to return
`kVersionConflict` plus a snapshot for an explicitly preconditioned internal
command; do not make normal public API contention version-conditional.

### Implement

Replace the minimal executor with the final manager-owned worker:

- immediate FIFO;
- delayed min-heap;
- insertion sequence for stable equal-time ordering;
- cancellation token per request;
- manual-clock wake support;
- close/drain/join.

Use overflow-safe backoff calculation. Check lifecycle before submit and before
reschedule.

### Checkpoint

Build and run both lock targets.

Expected capability: recoverable conflicts and transport loss converge without
blocking a worker.

## 16. Increment 11: Cancel, Renew, Release, and Expiry APIs

### Test first

Add tests for every public operation and result:

- explicit cancel and cancel token;
- cancel of granted request;
- renew success, owner mismatch, invalid state, and lease expiry;
- release success, repeat release, owner mismatch, waiting, and not found;
- acquisition expiry distinct from lease expiry;
- local lease deadline derived from reply RTT never exceeds the authority
  deadline, including a cached retry reply;
- snapshot never extends or newly establishes usable lease validity;
- lifecycle query for released/cancelled/timeout/expired/error outcomes.

### Implement

Add command creation and result projection for each API. External backend calls
must occur without the lifecycle mutex held.

Maintenance uses the same executor:

- observe cancel tokens;
- terminalize acquisition expiry;
- conservatively mark local lease expiry;
- enqueue corresponding authority operations where required.

Treat each create/cancel/renew/release as a separate logical operation with a
separate operation id. Retries reuse only that operation's id.

### Checkpoint

Run `dmn-test-dlock`, then both lock targets.

Expected capability: the complete public lifecycle works locally.

## 17. Increment 12: Retention and Strict Shutdown

### Test first

Using the manual clock and backend barriers, cover:

- terminal record exists before TTL;
- full terminal record is compacted after TTL without a query trigger;
- compact request/operation tombstone survives for the authority epoch and
  prevents a late duplicate create;
- granted record is never TTL-pruned;
- query observes a prune committed before query lock acquisition;
- shutdown wakes blocking callers;
- shutdown cancels pending retries;
- callback racing shutdown cannot access destroyed state;
- internal callbacks and worker tasks hold no strong ownership of the public
  manager;
- post-shutdown query persists;
- post-shutdown release returns `kShutdown`;
- an unreleased retained grant becomes locally invalid at its lease deadline;
- no post-shutdown mutation succeeds;
- shutdown leaves authority-side waiters to their bounded acquisition expiry
  rather than claiming transport cleanup after backend closure.

### Implement

Add `retention_version`, periodic full-record compaction, authority-lifetime
dedup tombstones, inflight callback accounting, and the exact shutdown sequence
from the specification.

Manager maintenance compacts manager records only. The in-memory backend owns
authority maintenance and compaction on its serialized authority lane; the
manager never calls authority internals through the backend abstraction.
Cleanup tombstones are not time-pruned while the manager is live because the
transport has no maximum command-delivery delay.

Prefer an explicit internal lifecycle enum (`kRunning`, `kClosing`, `kClosed`)
over several loosely related booleans. Backend callbacks capture `weak_ptr`.
Close the public mutation gate before backend shutdown, let backend shutdown
stop and drain callback dispatch, drain only already-accepted manager tasks,
then join the worker. Do not claim that a closed backend can release a grant;
applications release grants before shutdown and leases provide final recovery.
Callbacks and worker tasks operate on separately guarded internal state rather
than taking strong ownership of the public manager.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock
ctest --test-dir build -R '^dmn-test-dlock$' --output-on-failure
```

Run the shutdown-focused filter repeatedly if supported by the local test
runner, but do not replace deterministic synchronization with repetition.

Expected capability: manager lifetime and teardown are deterministic.

## 18. Increment 13: Observability

### Test first

Cover payloads for accept, retry, grant, renewal, release, every terminal
reason, and authority restart. Add emitter-throws tests for both immediate API
results and later query state.

### Implement

Add `Dmn_DLock_Event_Emitter` to the backend header or a dedicated public
header. Build immutable event values under the lifecycle lock, then invoke the
emitter after releasing it.

Use existing diagnostic conventions to report emitter exceptions. Never map an
emitter failure to `kPublisherError`. Document that emitters run on the manager
worker, must return promptly, must not re-enter manager APIs, and must not own
the invoking manager's lifetime. Keep a control-thread manager handle through
shutdown in tests.

### Checkpoint

Build and run `dmn-test-dlock`.

Expected capability: full lifecycle is observable without coupling
observability to correctness.

## 19. Increment 14: Protobuf Schema

### Test first

Create `test/dmn-test-dlock-dmesg.cpp` with pure serialization tests for every
command, reply, snapshot, state, terminal reason, and fence field.

### Implement

Create `src/proto/dmn-dlock.proto`.

Update:

- `src/proto/dmn-dmesg-type.proto` with reserved library value `dlock = 2`;
- `src/proto/dmn-dmesg-body.proto` with `DLockEnvelopePb dlock = 2`;
- `src/CMakeLists.txt` `GENERATE_PROTOBUF` input and generated source/header
  lists;
- `test/CMakeLists.txt` for `dmn-test-dlock-dmesg`.

Use explicit enum zero values such as `DLOCK_*_UNSPECIFIED`. Never serialize
`steady_clock::time_point`; serialize checked millisecond durations and
diagnostic timestamps. Include protocol version, domain, authority id,
authority generation/nonce where known, client id, correlation id, request id,
operation id, optional expected table version, command/reply variant, and
complete snapshot data including remaining-duration metadata for active
entries.

Use the repository's existing proto import layout:
`dmn-dmesg-body.proto` imports `"proto/dmn-dlock.proto"`. Add generated
`dmn-dlock.pb.cc` to the library's private sources and `dmn-dlock.pb.h` to its
interface sources.

### Checkpoint

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dmn-test-dlock-dmesg
ctest --test-dir build -R '^dmn-test-dlock-dmesg$' --output-on-failure
```

Expected capability: the complete wire model round-trips independently of
networking.

## 20. Increment 15: DMesg Backend

### Test first

Build an in-process DMesg harness with explicit synchronization. Cover:

- topic encoding and filtering;
- client-specific command topics and independent running counters;
- rejection of unrelated messages received by the authority's no-topic-filter
  handler;
- command correlation to reply;
- full snapshot delivery;
- duplicate/reordered reply and snapshot convergence;
- reconnect snapshot request;
- DMesg running-counter conflict recovery;
- backend shutdown/handler closure;
- unexpected authority id/epoch behavior;
- playback command rejection;
- client bootstrap before first command submission;
- authority reply/snapshot publication occurs off the DMesg callback context;
- authority conflict exceptions map to retryable failure and recover before
  retry;
- stale command replay after authority restart does not mutate the new
  authority;
- playback reply from a prior client incarnation is ignored;
- epoch mismatch reply triggers snapshot resynchronization and
  `kAuthorityRestarted`, not publisher error;
- authority snapshot conflict is recovered and republished or stops service.

### Implement

Create:

- `src/dmn-dlock-backend-dmesg.cpp`;
- `src/dmn-dlock-authority-service-dmesg.cpp`.

The adapter owns separate handlers for command, client reply, and snapshot
topics. Handler callbacks copy protocol data and hand it to the manager
callback without retaining references to protobuf objects.

Each client publishes to
`dmn.dlock.v1.<domain-token>.command.<client-token>`. Implement both tokens as
canonical unpadded RFC 4648 Base64url over the raw identifier bytes, rejecting
non-canonical tokens and token/envelope mismatches. The authority uses a
no-topic-filter handler because DMesg has exact matching but no wildcard
matching; it must parse and validate the topic prefix and envelope identity
before dispatch. The authority is the sole writer of
`reply.<client-token>` and `snapshot` topics.

Authority service mode executes the authority core and publishes reply plus
snapshot. It is enabled only for the explicitly configured authority id. Do
not use the `sys` message type/topic or infer this role from `Dmn_DMesgNet`
`masteridentifier`.

Give the authority service a dedicated single-thread executor. DMesg callbacks
only copy and enqueue. Publishing, counter/conflict inspection, conflict
recovery, snapshot requests, and handler closure all run outside DMesg callback
context. Drop command messages marked as playback and require the client's
generation/nonce to match the announced authority epoch.

### Checkpoint

```bash
cmake --build build --target dmn-test-dlock-dmesg
ctest --test-dir build -R '^dmn-test-dlock-dmesg$' --output-on-failure
```

Expected capability: two clients communicating with one configured authority
preserve the same semantics as the in-memory backend.

## 21. Increment 16: Contention and Failure Integration

### Test first

Add bounded integration tests:

- many overlapping clients never hold overlapping grants;
- non-overlapping clients make concurrent progress;
- duplicate, loss, and reorder faults converge;
- timeout/cancel/release churn leaves no active orphan;
- delayed grants after caller timeout are released by cleanup;
- authority restart terminates old-epoch requests;
- shutdown during contention terminates within a test-controlled bound;
- all observed fencing tokens are monotonic.

Use a shared assertion monitor that fails immediately if two granted snapshots
contain overlap.

Extend `test/dmn-test-dlock-fakes.hpp` with the shared assertion monitor and
contention barriers rather than adding an unlinked support `.cpp`.

### Implement

Only fix defects revealed by these tests. Do not weaken timeouts or add sleeps.
If a race cannot be made deterministic, add a narrow internal test hook rather
than exposing a production API.

### Checkpoint

```bash
cmake --build build --target \
  dmn-test-dlock-authority dmn-test-dlock dmn-test-dlock-dmesg
ctest --test-dir build \
  -R '^dmn-test-dlock(-authority|-dmesg)?$' --output-on-failure
```

Expected capability: end-to-end contention and failure behavior is proven.

## 22. Increment 17: Documentation and Full Regression

### Change

- add public API usage to `README`;
- document the single-authority deployment requirement prominently;
- show fence propagation to a protected resource;
- document renewal timing and shutdown order;
- document that grants must be released before manager shutdown;
- document client-specific command topics and the future DMesgNet
  single-authority/single-writer constraints;
- ensure every public header has the repository-standard Doxygen prologue;
- add lock test targets to valgrind registration when compatible;
- run formatting only on new/changed C++ files.

### Final checkpoint

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DENABLE_VALGRIND=ON
cmake --build build
ctest --test-dir build -R '^dmn-test-dlock' --output-on-failure
ctest --test-dir build -L dmn --output-on-failure
ctest --test-dir build -L valgrind --output-on-failure
```

If valgrind is unavailable, configure without `ENABLE_VALGRIND` and record that
environmental limitation; do not install a new analysis tool solely for this
feature.

## 23. Suggested Commit Boundaries

Keep each commit buildable:

1. value types and validation;
2. authority ordering;
3. versioning/idempotency;
4. authority lifecycle;
5. leases/fencing;
6. in-memory backend;
7. manager async skeleton;
8. mirror and query;
9. blocking/no-wait APIs;
10. retry executor;
11. mutation APIs and expiry;
12. retention/shutdown;
13. observability;
14. protobuf schema;
15. DMesg backend;
16. integration/docs.

Each commit should include its tests and the required
`Co-authored-by: Copilot <223556219+Copilot@users.noreply.github.com>` trailer
when commits are created through Copilot.

## 24. Definition of Done

- Specification invariants map to passing tests.
- All public API paths have success, invalid-input, ownership, terminal, and
  shutdown coverage.
- Authority tests prove exclusivity, ordering, idempotency, leases, and fences.
- Manager tests prove missed-wakeup safety, non-blocking workers, retention,
  and teardown.
- DMesg tests prove protocol filtering and eventual mirror convergence under
  duplicate/reordered delivery.
- timeout/cancel tests prove that undisclosed late grants are cleaned up.
- Existing `dmn` tests remain passing.
- No test relies on arbitrary sleep for correctness.
- Documentation states that v1 requires one configured authority and that
  resource-side fencing is required to reject stale holders.
