# Feature Spec: DMN Distributed Locking (Dmn_DLock)

Status: Proposed (spec-driven, implementation-ready).

## 1) Objective

Define a distributed lock design and execution plan that can be implemented step-by-step by human engineers or AI agents with strict TDD.

This revision adopts your latest design decisions:

- handler-local mirrored lock list
- publisher-authoritative conflict resolution/order
- async conflict retry with backoff
- API-thread waiting (never block worker async task)
- missed-wakeup-safe condition-variable contract

## 2) Repository-grounded constraints

The design follows existing DMN patterns:

- singleton pattern: `include/dmn-singleton.hpp`
- manager-owned object + proxy lifecycle pattern: `include/dmn-dmesg.hpp`
- async runtime/task style and testing conventions: `include/dmn-runtime-state.hpp`, `test/CMakeLists.txt`

## 2.1) Requirements baseline (normative)

Functional requirements:

- FR-1: Exclusive lock ordering is publisher-authoritative.
- FR-2: Handler tables mirror publisher snapshots by `table_version`.
- FR-3: `request_id` + `owner_id` identify ownership for release/cancel.
- FR-4: Conflict updates retry asynchronously with bounded backoff.
- FR-5: API-thread wait uses missed-wakeup-safe predicate+version wait loop.
- FR-6: Async worker must never block on condition wait.
- FR-7: Async requests must have bounded lifetime via per-request or manager-default expiry.

Non-functional requirements:

- NFR-1: Deterministic ordering for same-key contenders.
- NFR-2: No deadlock under contention/retry/shutdown.
- NFR-3: Test-first implementation (fail-first then pass) is mandatory.
- NFR-4: Observability failures cannot alter correctness outcomes.

Invariant requirements:

- IR-1: `sequence` is immutable per request creation.
- IR-2: `table_version` is monotonic per accepted mutation.
- IR-3: Request terminal states are one-way and non-reopenable.

## 3) Core architecture decisions

### 3.1 Authoritative ordering model

- The **publisher is authoritative** for lock-list order and conflict resolution.
- Handlers maintain mirrored copies for local decision/waiting.
- If a handler update conflicts at publisher (counter/version mismatch), handler retries via async worker.

> Note: This is safe only under single authoritative publisher per lock domain.

### 3.2 Handler-local mirrored table

Each handler stores a lock table of `LockingEntry` mirrored from publisher snapshots.

```cpp
enum class LockState {
  kLockWaiting,
  kLocking,
  kLocked,
  kUnlocked
};

struct LockingEntry {
  int start;
  int end;
  LockState state;
  std::string request_id;     // replaces void* RequestHandler
  std::string owner_id;
  int priority;
  uint64_t sequence;          // global monotonic sequence from publisher
  uint64_t table_version;     // snapshot version
};
```

Sort/index order:

1. `start` ascending
2. `end` ascending
3. `priority` descending (higher first)
4. `sequence` ascending (deterministic tie-break)

### 3.3 Why `request_id` + `owner_id`

- stable across threads/process boundaries
- safe for serialization and replay
- avoids unsafe pointer identity semantics

## 4) Request lock flow (normative)

## 4.1 `requestLock()` high-level behavior

1. For accepted submissions, API allocates unique `request_id` and creates
   `LockingEntry`.
2. API publish mode depends on API entrypoint:
   - `requestLockAsync(...)`: always submit async worker task and retain request lifecycle.
     Immediate API return for accepted async submission is always `kWaiting`
     with `request_id`, even if worker can grant internally without delay.
   - `requestLock(...)`: dispatch by `wait_timeout`:
     - `wait_timeout == 0ms`: synchronous single-attempt publish check.
     - `wait_timeout > 0ms`: submit async worker task then block in API thread.
3. Worker attempts publisher update with expected `table_version` / conflict counter.
4. If conflict, worker schedules retry with configurable backoff (10–500ms).
5. API thread waits (if needed) until local mirrored table indicates request is top/locked.
6. Publisher notifications update handler mirrored table and wake waiters.

## 4.2 Important execution rule

- **Never wait/block inside async worker task.**
- Worker performs quick update/retry scheduling only.
- Blocking wait happens in API caller context.

## 4.3 Conflict retry policy

- Retry only on conflict/version-mismatch outcomes.
- Backoff configurable: min 10ms, max 500ms, jitter supported.
- Retryable outcomes: conflict/version-mismatch only.
- Terminal outcomes: granted, wait timeout, cancel, shutdown, terminal publisher failure.
- Support cancellation/shutdown interruption.
- Retry loop applies only to `requestLock(wait_timeout > 0ms)` and
  `requestLockAsync`; no-wait mode (`wait_timeout == 0ms`) is single-attempt.
- `requestLockAsync` without cancel token may remain in `kLockWaiting` lifecycle
  state (query code `kWaiting`) only until bounded async expiry is reached.
- `requestLockAsync` retries continue while request remains pending under
  sustained conflict/version-mismatch
  (bounded backoff), unless cancelled/shutdown/async-expiry.
- async requests must always have bounded lifetime via:
  - per-request `async_expiry` when set, or
  - mandatory manager `default_async_expiry` when `async_expiry` is nullopt.
- async expiry terminalizes retained request as `kTimeout`.
- for blocking requests, timeout terminalization must set shared terminal flag
  checked before each retry attempt; retries must stop once timeout is reached.

## 5) Missed-wakeup-safe wait/notify contract (critical)

Condition-variable notifications are not sticky. Safety requires predicate + mutex + version loop.

Shared state under one mutex:

- mirrored lock table
- `table_version`
- per-request terminal flags (`granted`, `cancelled`, `timed_out`, `failed`)

Wait pattern (normative):

```cpp
cv.wait(lock, [&] {
  return isRequestTopAndLocked(request_id) ||
         isRequestTerminal(request_id) ||
         observed_version != table_version;
});
```

Rules:

- updater acquires same mutex, updates table/version/flags, then `notify_all()`
- waiter must re-check predicate in loop
- waiter captures `observed_version` before sleeping

This prevents lost notifications and stale waits.

## 6) Public API contract (v1)

## 6.0) Class relationship and responsibility guide (normative)

```text
Dmn_DLock_Manager (singleton orchestrator)
  ├─ Request API surface (request/release/cancel/query/shutdown)
  ├─ Local mirrored table state (mutex + cv + version)
  ├─ Async retry worker scheduling
  ├─ Publisher client adapter (authoritative update/read)
  └─ Event emitter adapter (optional)
```

Responsibility boundaries:

- `Dmn_DLock_Manager`: orchestration and correctness enforcement.
- Publisher adapter: authoritative acceptance/rejection + version sequencing.
- Handler mirror state: local wait predicate evaluation only (not authoritative).
- Async worker: publish/retry only; no blocking wait.
- API caller thread: optional blocking wait until terminal/granted.

Forbidden responsibility leakage:

- worker thread must not own wait loop
- handler mirror must not resolve authoritative conflicts
- event emitter must not mutate lock decision paths

```cpp
struct Dmn_DLock_RequestOptions {
  std::string owner_id; // required; identifies request owner
  int priority{0};
  std::chrono::milliseconds wait_timeout{std::chrono::milliseconds{0}}; // 0ms = no-wait (single attempt)
  std::optional<std::chrono::milliseconds> async_expiry{std::nullopt}; // bounded async lifetime; falls back to manager default when nullopt
  std::chrono::milliseconds retry_min_backoff{std::chrono::milliseconds{10}};
  std::chrono::milliseconds retry_max_backoff{std::chrono::milliseconds{500}};
  double retry_jitter_ratio{0.20};
  std::shared_ptr<std::atomic_bool> cancel_token{};
};

struct Dmn_DLock_Result {
  enum class Code {
    kGranted,
    kWaiting,
    kConflict,
    kTimeout,
    kCancelled,
    kNotOwner,
    kNotFound,
    kInvalidArg,
    kLocalSubmissionError,
    kPublisherError,
    kShutdown
  };

  Code code{Code::kInvalidArg};
  std::string message;
  std::string request_id;
  std::string owner_id;
  uint64_t sequence{0};
  uint64_t table_version{0};
  std::optional<LockingEntry> entry;
  std::optional<bool> owner_match;
};

struct Config {
  std::chrono::milliseconds default_async_expiry{std::chrono::minutes{5}}; // mandatory bounded async lifetime fallback
  std::chrono::milliseconds retained_terminal_ttl{std::chrono::hours{1}};  // minimum terminal queryability window
};

class Dmn_DLock_Manager : public dmn::Dmn_Singleton<Dmn_DLock_Manager> {
public:
  static auto createInstance(const Config &cfg) -> std::shared_ptr<Dmn_DLock_Manager>;

  auto requestLock(int start, int end, const Dmn_DLock_RequestOptions &opts)
      -> Dmn_DLock_Result;

  // non-blocking waitable submission: enqueues request lifecycle and returns
  // immediately with request_id and `kWaiting` for accepted submission.
  auto requestLockAsync(int start, int end, const Dmn_DLock_RequestOptions &opts)
      -> Dmn_DLock_Result;

  auto releaseLock(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  auto cancelRequest(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  auto getRequestStateForOwner(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  void shutdown();
};
```

Query mutability contract:

- `getRequestStateForOwner` is concurrency-safe and callable during lifecycle transitions.
- query may perform internal housekeeping (for example, retention pruning/access
  bookkeeping) but must not mutate externally observable request outcome.
- housekeeping must not delete retained terminal records before
  `retained_terminal_ttl` has elapsed from terminalization.
- retained granted records are exempt from TTL pruning and must be preserved
  until explicit `releaseLock` succeeds.
- after `retained_terminal_ttl` elapses, query may legitimately return
  `kNotFound` for previously terminalized non-granted records that were pruned.

Argument validity rules:

- `start <= end` is required; otherwise `kInvalidArg`.
- negative range values are invalid for Phase 1 and return `kInvalidArg`.
- `owner_id` must be non-empty.
- `wait_timeout == 0ms` means no-wait single attempt.
- `async_expiry` (when provided) must be > 0ms.
- manager `default_async_expiry` must be configured > 0ms.
- manager `retained_terminal_ttl` must be configured > 0ms.
- `retry_min_backoff` and `retry_max_backoff` must be >= 0.
- `retry_min_backoff <= retry_max_backoff` is required.
- `retry_jitter_ratio` must be in `[0.0, 1.0]`.
- invalid retry option combinations return `kInvalidArg`.

`requestLock` return behavior:

- if `wait_timeout == 0ms`: return immediately with `kGranted` or terminal error
  code; if not immediately grantable due to ordering/conflict, return
  `kConflict`. Do not enqueue background retry/waiter state.
- no-wait mode allocates `request_id` only after immediate acceptance; conflicted
  no-wait submissions do not create lifecycle records.
- accepted no-wait grant (`kGranted`) is retained as lifecycle record and is
  queryable/releasable by `request_id`.
- if `wait_timeout > 0ms`: block in API thread until one of
  `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`; do not
  return `kWaiting` before timeout.
- accepted blocking requests (`wait_timeout > 0ms`) are retained as addressable
  request lifecycle records and remain queryable by `request_id` after return.
- when blocking request returns `kTimeout`, associated retry loop must be
  terminalized and no further retries may run for that request.
- queued requests must transition to `kGranted` when they become top-of-list
  and publisher accepts lock transition.
- `requestLockAsync` always returns immediately with request lifecycle retained
  for later `getRequestStateForOwner`/`cancelRequest`/`releaseLock`, and returns
  `kWaiting` on accepted submission.
- even if request is immediately top/lockable, async API still returns `kWaiting`
  and transitions to `kGranted` through subsequent lifecycle update/query.
- async API return code `kWaiting` represents submission acceptance, not the
  internal persisted lifecycle state at that exact instant.
- accepted async submissions must return non-empty `request_id`.
- `requestLockAsync` immediate rejection mapping:
  - invalid args/options -> `kInvalidArg`
  - shutdown gate active -> `kShutdown`
  - local enqueue/scheduling failure before lifecycle acceptance -> `kLocalSubmissionError`
- `kLocalSubmissionError` is reserved for pre-acceptance local
  submission/scheduling failures only.
- `kPublisherError` is reserved strictly for post-acceptance publisher
  transport/logic terminal failures only (not local submission, shutdown,
  timeout, cancellation, or observability/emitter issues).
- shutdown-triggered termination must map to `kShutdown`, not `kPublisherError`.
- observability/emitter failures must not alter operation result code and must
  never remap outcomes to `kPublisherError`.

Deterministic result mapping:

- invalid args/options -> `kInvalidArg`
- pre-acceptance local enqueue/scheduling failure -> `kLocalSubmissionError`
- synchronous no-wait accepted and on-top lockable -> `kGranted`
- synchronous blocking-wait accepted and eventually/top granted -> `kGranted`
- synchronous no-wait not immediately grantable or preflight
  conflict/version-mismatch -> `kConflict` (no lifecycle created)
- conflict detected and retry scheduled (wait/async modes) -> transient internal state;
  final API result is one of `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`
- async retry-pending query state reports `kWaiting` until terminal transition.
- owner-scoped query for granted async request returns `kGranted` with `entry`.
- owner-scoped query must collapse in-progress internal `kLocking` state to
  external result code `kWaiting` until terminal/granted transition is observed.
- owner-scoped query for terminal async outcomes returns:
  - timeout -> `kTimeout`
  - cancelled -> `kCancelled`
  - shutdown-aborted -> `kShutdown`
  - publisher terminal failure -> `kPublisherError`
- timeout expiry in wait mode -> `kTimeout`
- cancel token/request cancellation -> `kCancelled`
- release/cancel owner mismatch -> `kNotOwner`
- owner-scoped query (`getRequestStateForOwner`) validates owner identity.
- owner-scoped query mismatch or foreign request -> `kNotFound` (existence-safe)
- query request not found -> `kNotFound`
- shutdown gate for submission/mutation APIs (`requestLock`, `requestLockAsync`,
  `cancelRequest`) -> `kShutdown`
- shutdown gate exception: `releaseLock` is the only mutation allowed
  during/after shutdown, and only for retained previously granted requests.
- `releaseLock` remains permitted during/after shutdown only for retained
  already-granted requests; release of retained non-granted/terminal requests
  returns `kNotFound`.
- retained granted requests must remain discoverable for `releaseLock`
  (not TTL-pruned) until explicit release succeeds.
- owner-scoped query during/after shutdown returns persisted request outcome:
  `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError` or `kNotFound`.
- owner-scoped query returns `kShutdown` only when that request lifecycle was
  terminated by shutdown.
- publisher transport/logic failure -> `kPublisherError`

`request_id` field population rules:

- `request_id` is acceptance-scoped, not result-code-scoped.
- accepted async submissions return immediately as `kWaiting` with non-empty
  `request_id`; later terminal outcomes are observed via
  `getRequestStateForOwner`, not async immediate return code.
- `request_id` must be populated for any accepted request lifecycle, including
  accepted submissions whose persisted lifecycle later terminates as
  `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`.
- this includes synchronous no-wait `kGranted` outcomes and blocking wait-mode
  returns (`kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`)
  after accepted submission.
- `request_id` must be empty for pre-acceptance immediate rejections
  (`kInvalidArg`, immediate `kShutdown`, pre-acceptance `kLocalSubmissionError`,
  `kConflict` in no-wait mode).

`shutdown()` completion contract:

- `shutdown()` is synchronous.
- On return, pending waiters are woken, pending retries are cancelled, and
  retained request outcomes are deterministically persisted.
- requests already in granted/locked outcome remain `kGranted` (not rewritten).
- retained granted requests may still be explicitly released via `releaseLock`
  during/after shutdown completion.
- requests still pending/nonterminal at shutdown are terminalized to
  `kShutdown` and lifecycle-closed (`kUnlocked`) **unless** authoritative
  publisher grant confirmation was already persisted, in which case outcome
  must remain `kGranted`.
- post-shutdown, `releaseLock` remains intentionally available as the sole
  mutation to allow deterministic cleanup of retained granted requests.
- `shutdown()` is guaranteed non-throwing and does not return failure status
  (best-effort completion with deterministic terminalization semantics).

## 7) State machine semantics

Section 7 defines **internal lifecycle state transitions**. External API/query
result projection rules are defined in Section 6 and may report `kWaiting`
while internal lifecycle has advanced to `kLocking` for accepted async requests.

Valid transitions:

- `kLockWaiting -> kLocking -> kLocked -> kUnlocked`
- `kLocking -> kLocked -> kUnlocked` (accepted immediately-top async request path)
- `kLocking -> kLocked` (accepted immediately-top synchronous no-wait grant path)
- `kLocking -> kLocked` (accepted immediately-top synchronous blocking-wait grant path)
- `kLockWaiting -> kUnlocked` (cancel/timeout/shutdown terminalization)
- `kLocking -> kUnlocked` (publisher reject/cancel/shutdown/publisher terminal failure)

Forbidden:

- `kUnlocked -> kLocked` reuse of same request_id

Terminal publisher failure representation:

- retained request may transition to terminal lifecycle state
  `kLockWaiting -> kUnlocked` or `kLocking -> kUnlocked`
  with result code `kPublisherError` depending on failure timing.

State entry triggers:

- enter `kLockWaiting` when request is accepted but not currently top/lockable.
- enter `kLocking` when request becomes top candidate and a publish/grant
  transition attempt is in progress at publisher.
- accepted immediately-top requests (async/no-wait/blocking-wait) may enter
  `kLocking` directly.
- for accepted async requests, external API/query state may still report
  `kWaiting` while internal lifecycle is `kLocking`.
- enter `kLocked` when publisher confirms lock grant for the request.

## 8) Data consistency and ordering rules

- Publisher increments global `sequence` only on authoritative
  publisher-accepted request-create mutation.
- Publisher increments `table_version` on every accepted table mutation.
- `sequence` is assigned once at request creation and is immutable for that
  request across release/cancel/state mutations.
- mutation ordering uses `table_version` (not reassigned `sequence`).
- Handlers apply snapshots only if incoming version is newer.
- Top-of-list decision always based on latest mirrored version.

## 9) Starvation policy

Phase 1:

- priority ordering enabled
- no mandatory aging uplift yet
- uplift policy intentionally deferred

Accepted Phase-1 behavior: starvation of lower-priority requests is possible.

Operational guidance:

- run with bounded wait timeout and cancellation policies
- monitor queue age by priority via observability metrics
- treat sustained starvation as SLO breach and operationally rebalance priority inputs

Non-goal for Phase 1: formal starvation-freedom guarantee.

## 10) Observability contract

Emit events for:

- request created
- request published
- conflict retry scheduled
- granted
- waiting entered
- release success
- timeout/cancel/shutdown

Minimum payload:

- `request_id`, `owner_id`
- `start`, `end`, `priority`
- `state_before`, `state_after`
- `sequence`, `table_version`
- `result_code`, `timestamp_ms`

Emitter failures must never change lock correctness result and must never remap
an operation outcome to `kPublisherError`.

## 11) Strict TDD execution protocol (mandatory)

For **each test case**:

1. Add exactly one new test.
2. Build target.
3. Run focused test and confirm fail.
4. Implement minimal code for that test.
5. Build target.
6. Run focused test and confirm pass.
7. Run full dlock test target.
8. Record fail-first and pass evidence in PR notes (or equivalent progress
   log) including test name and command outputs.

Normative command checkpoints:

- build: `cmake --build <build_dir> --target dmn-test-dlock`
- list tests: `ctest --test-dir <build_dir> -N`
- focused test run (normative, deterministic):
  1. resolve exact test executable command via
     `ctest --test-dir <build_dir> -N -V -R '^dmn-test-dlock$'`
  2. run resolved executable with
     `--gtest_filter=<Suite.Test>`
- full test entry: `ctest --test-dir <build_dir> -R 'dmn-test-dlock' --output-on-failure`

## 12) Step-by-step implementation plan

### Phase 0 — scaffolding

1. Add/refresh dlock headers/source files.
2. Define `LockState`, `LockingEntry`, result/options structures.
3. Wire singleton manager skeleton.
4. Add `dmn-test-dlock` target.
5. Build checkpoint.

### Phase 1 — local table + ordering

1. Implement handler-local table container and index/sort rules.
2. Implement deterministic top selection.
3. Add tests for ordering by start/end/priority/sequence.
4. TDD loop + build checkpoint.

### Phase 2 — publisher update + conflict retries

1. Implement publisher submit path with expected `table_version`.
2. Implement conflict detection and async retry scheduling.
3. Add configurable 10–500ms backoff + jitter.
4. Add conflict retry tests.
5. TDD loop + build checkpoint.

### Phase 3 — API wait path + missed wakeup safety

1. Implement API-side wait on condition variable.
2. Implement mutex-protected predicate + version pattern.
3. Ensure worker never blocks.
4. Add tests for missed-notify race, version change wakeup.
5. TDD loop + build checkpoint.

### Phase 4 — grant/release/cancel lifecycle

1. Implement state transitions and terminal states.
2. Implement `releaseLock` ownership checks (`request_id` + `owner_id`).
3. Implement `cancelRequest` behavior while waiting/retrying.
4. Add lifecycle tests.
5. TDD loop + build checkpoint.

### Phase 5 — shutdown behavior

1. Reject new requests with `kShutdown`.
2. Wake blocked waiters and mark terminal.
3. Cancel pending retries safely.
4. Add shutdown tests.
5. TDD loop + build checkpoint.

### Phase 6 — observability

1. Add event emission hooks.
2. Validate payload schema.
3. Ensure emitter failure isolation.
4. Add observability tests for:
   - success transition payload (`kWaiting -> kGranted`)
   - terminal timeout transition payload
   - terminal cancel transition payload
   - terminal shutdown transition payload
5. TDD loop + build checkpoint.

### Phase 7 — stress/integration

1. High contention overlapping ranges.
2. Reorder and retry churn.
3. Cancellation/timeouts under load.
4. No worker-thread blocking assertions.
5. TDD loop + final build/test checkpoint.

## 12.1) File-by-file implementation detail checklist (no-gap guide)

`include/dmn-dlock.hpp`

- declare `LockState`, `LockingEntry`, `Dmn_DLock_RequestOptions`, `Dmn_DLock_Result`
- declare `Dmn_DLock_Manager` API signatures exactly as Section 6
- document preconditions/postconditions on each API

`include/dmn-dlock-backend.hpp`

- declare publisher-authoritative backend adapter interface:
  - single-attempt publish for no-wait mode
  - versioned publish for async retry mode
  - snapshot fetch/update callbacks

`src/dmn-dlock.cpp`

- implement argument validation and deterministic result mapping
- implement no-wait synchronous single-attempt path
- implement wait-timeout async path with worker retries
- implement mutex/cv wait predicate loop with `table_version`
- implement release/cancel ownership checks
- implement shutdown rejection and waiter wakeup

`src/dmn-dlock-backend-memory.cpp`

- implement in-memory authoritative table behavior
- enforce monotonic `sequence` (create only) and monotonic `table_version` (all mutations)
- implement conflict detection and expected-version matching

`test/dmn-test-dlock.cpp`

- add tests in exact order from the mandatory ordered test matrix in this spec
- each test must follow fail-first -> minimal code -> pass loop
- add race-focused tests for missed wakeup and worker non-blocking guarantee

`test/CMakeLists.txt`

- ensure `dmn-test-dlock` is registered as runnable ctest entry

## 13) Mandatory ordered test matrix

1. `RequestLock_InvalidRange_ReturnsInvalidArg`
2. `RequestLock_EmptyOwnerId_ReturnsInvalidArg`
3. `RequestLock_InvalidRetryBounds_ReturnsInvalidArg`
4. `RequestLock_InvalidJitterRatio_ReturnsInvalidArg`
5. `ManagerConfig_DefaultAsyncExpiry_NonPositiveRejected`
6. `LockingEntry_OrderByStartEndPrioritySequence`
7. `RequestLockSync_PublisherAccept_ReturnsGranted`
8. `RequestLockSync_PublisherConflict_SchedulesRetry`
9. `RequestLock_RetryBackoff_RespectsConfiguredBounds`
10. `RequestLock_NoWaitMode_NotTopReturnsConflictCode`
11. `RequestLock_NoWaitMode_VersionMismatchReturnsConflictCode`
12. `RequestLock_ApiWait_GrantsWhenTop`
13. `RequestLock_MissedWakeupRace_DoesNotHang`
14. `RequestLock_VersionChange_WakesWaiter`
15. `ReleaseLock_NotOwner_ReturnsNotOwner`
16. `ReleaseLock_Owner_SetsUnlocked`
17. `ReleaseLock_RequestNotFound_ReturnsNotFound`
18. `CancelRequest_RequestNotFound_ReturnsNotFound`
19. `GetRequestStateForOwner_RequestNotFound_ReturnsNotFound`
20. `GetRequestStateForOwner_OwnerMismatch_ReturnsNotFound`
21. `RequestLockAsync_ReturnsRequestIdAndWaiting`
22. `RequestLockAsync_InvalidArgs_ReturnsInvalidArgAndEmptyRequestId`
23. `RequestLockAsync_ShutdownGate_ReturnsShutdownAndEmptyRequestId`
24. `RequestLockAsync_LocalEnqueueFailure_ReturnsLocalSubmissionErrorAndEmptyRequestId`
25. `RequestLock_PublisherTerminalFailure_ReturnsPublisherError`
26. `GetRequestStateForOwner_GrantedState_ReturnsGrantedWithEntry`
27. `GetRequestStateForOwner_TimeoutState_ReturnsTimeout`
28. `GetRequestStateForOwner_CancelledState_ReturnsCancelled`
29. `GetRequestStateForOwner_ShutdownState_ReturnsShutdown`
30. `GetRequestStateForOwner_PublisherFailureState_ReturnsPublisherError`
31. `GetRequestStateForOwner_PostShutdownGrantedState_ReturnsGranted`
32. `GetRequestStateForOwner_PostShutdownTimeoutState_ReturnsTimeout`
33. `GetRequestStateForOwner_PostShutdownCancelledState_ReturnsCancelled`
34. `GetRequestStateForOwner_PostShutdownPublisherErrorState_ReturnsPublisherError`
35. `BlockingRequest_PostReturnQuery_Granted_ReturnsGranted`
36. `BlockingRequest_PostReturnQuery_Timeout_ReturnsTimeout`
37. `BlockingRequest_PostReturnQuery_Cancelled_ReturnsCancelled`
38. `BlockingRequest_PostReturnQuery_Shutdown_ReturnsShutdown`
39. `BlockingRequest_PostReturnQuery_PublisherError_ReturnsPublisherError`
40. `CancelRequest_WaitingRequest_Terminates`
41. `BlockingRequest_ExternalCancelRequest_TerminatesAndStopsRetries`
42. `BlockingRequest_ExternalCancelRequest_WithoutRetryLoop_ReturnsCancelled`
43. `Shutdown_NewRequests_ReturnShutdown`
44. `Shutdown_CancelRequest_ReturnsShutdown`
45. `Shutdown_WakesWaiters`
46. `Shutdown_CancelsPendingRetries`
47. `Shutdown_RetainedGrantedRequest_ReleaseStillAllowed`
48. `Shutdown_RetainedNonGrantedRequest_ReleaseReturnsNotFound`
49. `Shutdown_RetainedGrantedRequest_NotPrunedBeforeRelease`
50. `RequestLockAsync_DefaultAsyncExpiry_ExpiresWithTimeout`
51. `Observability_EmitPayloadSchema_Valid`
52. `Observability_EmitSuccessTransitionPayload_Valid`
53. `Observability_EmitTimeoutTransitionPayload_Valid`
54. `Observability_EmitCancelTransitionPayload_Valid`
55. `Observability_EmitShutdownTransitionPayload_Valid`
56. `Observability_EmitterFailure_DoesNotChangeResult`
57. `Observability_EmitterFailure_DoesNotChangePersistedQueryState`
58. `RequestLock_WaitTimeout_ExpiresWithTimeoutCode`
59. `RequestLock_CancelToken_InterruptsWithCancelledCode`
60. `Stress_HighContention_NoDeadlock`
61. `Stress_WorkerThreads_NeverBlockOnWait`
62. `RequestLockAsync_AcceptedThenTimeout_PersistedLifecycleHasSameRequestId`
63. `RequestLockAsync_AcceptedThenCancelled_PersistedLifecycleHasSameRequestId`
64. `RequestLockAsync_AcceptedThenShutdown_PersistedLifecycleHasSameRequestId`
65. `RequestLockAsync_AcceptedThenPublisherError_PersistedLifecycleHasSameRequestId`
66. `BlockingRequest_AcceptedThenTimeout_ReturnValueHasRequestId`
67. `BlockingRequest_AcceptedThenCancelled_ReturnValueHasRequestId`
68. `BlockingRequest_AcceptedThenShutdown_ReturnValueHasRequestId`
69. `BlockingRequest_AcceptedThenPublisherError_ReturnValueHasRequestId`
70. `ManagerConfig_RetainedTerminalTtl_NonPositiveRejected`

## 14) Definition of Done

- All sections implemented without contradiction.
- All tests in Section 13 pass with mandatory TDD loop evidence.
- No async worker waits.
- Missed-wakeup-safe wait contract verified by race tests.
- Docs/spec internally consistent and complete.
