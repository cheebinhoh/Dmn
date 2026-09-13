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

1. API creates new `LockingEntry` with `kLockWaiting` and unique `request_id`.
2. API publish mode depends on API entrypoint:
   - `requestLockAsync(...)`: always submit async worker task and retain request lifecycle.
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
- Retry continues until one terminal boundary: granted, wait timeout, cancel,
  shutdown, or publisher error.
- Support cancellation/shutdown interruption.
- Retry loop applies only to `requestLock(wait_timeout > 0ms)` and
  `requestLockAsync`; no-wait mode (`wait_timeout == 0ms`) is single-attempt.

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
    kPublisherError,
    kShutdown
  };

  bool ok{false}; // derived strictly from `code` per normative mapping below
  Code code{Code::kInvalidArg};
  std::string message;
  std::string request_id;
  std::string owner_id;
  uint64_t sequence{0};
  uint64_t table_version{0};
  std::optional<LockingEntry> entry;
  std::optional<bool> owner_match;
};

class Dmn_DLock_Manager : public dmn::Dmn_Singleton<Dmn_DLock_Manager> {
public:
  static auto createInstance(const Config &cfg) -> std::shared_ptr<Dmn_DLock_Manager>;

  auto requestLock(int start, int end, const Dmn_DLock_RequestOptions &opts)
      -> Dmn_DLock_Result;

  // non-blocking waitable submission: enqueues request lifecycle and returns
  // immediately with request_id and either:
  // - `kWaiting` for accepted queued submission
  // - `kGranted` for immediate lockable submission
  auto requestLockAsync(int start, int end, const Dmn_DLock_RequestOptions &opts)
      -> Dmn_DLock_Result;

  auto releaseLock(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  auto cancelRequest(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  auto getRequestStateForOwner(const std::string &request_id, const std::string &owner_id) const
      -> Dmn_DLock_Result;

  void shutdown();
};
```

Argument validity rules:

- `start <= end` is required; otherwise `kInvalidArg`.
- negative range values are invalid for Phase 1 and return `kInvalidArg`.
- `owner_id` must be non-empty.
- `wait_timeout == 0ms` means no-wait single attempt.
- `retry_min_backoff` and `retry_max_backoff` must be >= 0.
- `retry_min_backoff <= retry_max_backoff` is required.
- `retry_jitter_ratio` must be in `[0.0, 1.0]`.
- invalid retry option combinations return `kInvalidArg`.

`requestLock` return behavior:

- if `wait_timeout == 0ms`: return immediately with `kGranted` or terminal error
  code; if not immediately grantable due to ordering/conflict, return
  `kConflict`. Do not enqueue background retry/waiter state.
- if `wait_timeout > 0ms`: block in API thread until one of
  `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`; do not
  return `kWaiting` before timeout.
- queued requests must transition to `kGranted` when they become top-of-list
  and publisher accepts lock transition.
- `requestLockAsync` always returns immediately with request lifecycle retained
  for later `getRequestStateForOwner`/`cancelRequest`/`releaseLock`, and returns
  `kWaiting` on accepted queued submission or `kGranted` if immediately lockable.
- `requestLockAsync` immediate rejection mapping:
  - invalid args/options -> `kInvalidArg`
  - shutdown gate active -> `kShutdown`
  - publisher immediate submission failure -> `kPublisherError`

Deterministic result mapping:

- invalid args/options -> `kInvalidArg`
- synchronous no-wait accepted and on-top lockable -> `kGranted`
- synchronous no-wait not immediately grantable or publish conflict/version mismatch -> `kConflict`
- conflict detected and retry scheduled (wait/async modes) -> transient internal state;
  final API result is one of `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`
- async retry-pending query state reports `kWaiting` until terminal transition.
- owner-scoped query for granted async request returns `kGranted` with `entry`.
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
- shutdown gate -> `kShutdown`
- publisher transport/logic failure -> `kPublisherError`

Normative `ok` mapping:

- `ok=true`: `kGranted`
- `ok=false`: `kConflict`, `kWaiting`, `kTimeout`, `kCancelled`, `kNotOwner`, `kNotFound`,
  `kInvalidArg`, `kPublisherError`, `kShutdown`

## 7) State machine semantics

Valid transitions:

- `kLockWaiting -> kLocking -> kLocked -> kUnlocked`
- `kLockWaiting -> kUnlocked` (cancel/timeout)
- `kLocking -> kUnlocked` (publisher reject/cancel/shutdown)

Forbidden:

- `kUnlocked -> kLocked` reuse of same request_id

## 8) Data consistency and ordering rules

- Publisher increments global `sequence` **only on accepted request creation**.
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

Emitter failures must never change lock correctness result.

## 11) Strict TDD execution protocol (mandatory)

For **each test case**:

1. Add exactly one new test.
2. Build target.
3. Run focused test and confirm fail.
4. Implement minimal code for that test.
5. Build target.
6. Run focused test and confirm pass.
7. Run full dlock test target.

Normative command checkpoints:

- build: `cmake --build <build_dir> --target dmn-test-dlock`
- list tests: `ctest --test-dir <build_dir> -N`
- focused test run (normative): run one exact gtest case from the dlock test
  binary with `--gtest_filter=<Suite.Test>`.
- full test entry: `ctest --test-dir <build_dir> -R 'dmn-test-dlock' --output-on-failure`

If test binary path differs by generator/layout, use build output discovery to
locate `dmn-test-dlock` first.

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
4. Add observability tests.
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
5. `LockingEntry_OrderByStartEndPrioritySequence`
6. `RequestLock_PublisherAccept_ReturnsGranted`
7. `RequestLock_PublisherConflict_SchedulesRetry`
8. `RequestLock_RetryBackoff_RespectsConfiguredBounds`
9. `RequestLock_NoWaitMode_NotTopReturnsConflictCode`
10. `RequestLock_NoWaitMode_VersionMismatchReturnsConflictCode`
11. `RequestLock_ApiWait_GrantsWhenTop`
12. `RequestLock_MissedWakeupRace_DoesNotHang`
13. `RequestLock_VersionChange_WakesWaiter`
14. `ReleaseLock_NotOwner_ReturnsNotOwner`
15. `ReleaseLock_Owner_SetsUnlocked`
16. `ReleaseLock_RequestNotFound_ReturnsNotFound`
17. `CancelRequest_RequestNotFound_ReturnsNotFound`
18. `GetRequestStateForOwner_RequestNotFound_ReturnsNotFound`
19. `GetRequestStateForOwner_OwnerMismatch_ReturnsNotFound`
20. `RequestLockAsync_ReturnsRequestIdAndWaitingOrGranted`
21. `GetRequestStateForOwner_TimeoutState_ReturnsTimeout`
22. `GetRequestStateForOwner_CancelledState_ReturnsCancelled`
23. `GetRequestStateForOwner_ShutdownState_ReturnsShutdown`
24. `GetRequestStateForOwner_PublisherFailureState_ReturnsPublisherError`
25. `CancelRequest_WaitingRequest_Terminates`
26. `Shutdown_NewRequests_ReturnShutdown`
27. `Shutdown_WakesWaiters`
28. `Shutdown_CancelsPendingRetries`
29. `Observability_EmitPayloadSchema_Valid`
30. `Observability_EmitterFailure_DoesNotChangeResult`
31. `ResultCodeMapping_GrantedSetsOkTrue`
32. `ResultCodeMapping_ConflictSetsOkFalse`
33. `ResultCodeMapping_TimeoutSetsOkFalse`
34. `ResultCodeMapping_WaitingSetsOkFalse`
35. `ResultCodeMapping_CancelledSetsOkFalse`
36. `ResultCodeMapping_NotOwnerSetsOkFalse`
37. `ResultCodeMapping_NotFoundSetsOkFalse`
38. `ResultCodeMapping_InvalidArgSetsOkFalse`
39. `ResultCodeMapping_PublisherErrorSetsOkFalse`
40. `ResultCodeMapping_ShutdownSetsOkFalse`
41. `RequestLock_WaitTimeout_ExpiresWithTimeoutCode`
42. `RequestLock_CancelToken_InterruptsWithCancelledCode`
43. `Stress_HighContention_NoDeadlock`
44. `Stress_WorkerThreads_NeverBlockOnWait`

## 14) Definition of Done

- All sections implemented without contradiction.
- All tests in Section 13 pass with mandatory TDD loop evidence.
- No async worker waits.
- Missed-wakeup-safe wait contract verified by race tests.
- Docs/spec internally consistent and complete.
