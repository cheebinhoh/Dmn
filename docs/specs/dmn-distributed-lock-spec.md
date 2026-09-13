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
2. API submits async worker task to publish/update at authoritative publisher.
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
- Include max-attempt or timeout guard.
- Support cancellation/shutdown interruption.

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

```cpp
struct Dmn_DLock_RequestOptions {
  std::string owner_id; // required; identifies request owner
  int priority{0};
  std::chrono::milliseconds wait_timeout{0ms}; // 0ms = no-wait (single attempt)
  std::chrono::milliseconds retry_min_backoff{10ms};
  std::chrono::milliseconds retry_max_backoff{500ms};
  double retry_jitter_ratio{0.20};
  std::shared_ptr<std::atomic_bool> cancel_token{};
};

struct Dmn_DLock_Result {
  enum class Code {
    kGranted,
    kWaiting,
    kConflictRetrying,
    kTimeout,
    kCancelled,
    kNotOwner,
    kInvalidArg,
    kPublisherError,
    kShutdown
  };

  bool ok{false};
  Code code{Code::kInvalidArg};
  std::string message;
  std::string request_id;
  std::string owner_id;
  uint64_t sequence{0};
  uint64_t table_version{0};
};

class Dmn_DLock_Manager : public dmn::Dmn_Singleton<Dmn_DLock_Manager> {
public:
  static auto createInstance(const Config &cfg) -> std::shared_ptr<Dmn_DLock_Manager>;

  auto requestLock(int start, int end, const Dmn_DLock_RequestOptions &opts = {})
      -> Dmn_DLock_Result;

  auto releaseLock(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  auto cancelRequest(const std::string &request_id, const std::string &owner_id)
      -> Dmn_DLock_Result;

  auto getRequestState(const std::string &request_id) const
      -> std::optional<LockingEntry>;

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

- if `wait_timeout == 0ms`: return immediately with `kGranted`, `kWaiting`, or
  terminal error code.
- if `wait_timeout > 0ms`: block in API thread until one of
  `kGranted`/`kTimeout`/`kCancelled`/`kShutdown`/`kPublisherError`; do not
  return `kWaiting` before timeout.

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
- focused test: `<build_dir>/test/dmn-test-dlock --gtest_filter=<Suite.Test>`
- full test entry: `ctest --test-dir <build_dir> -R '^dmn-test-dlock$' --output-on-failure`

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

## 13) Mandatory ordered test matrix

1. `RequestLock_InvalidRange_ReturnsInvalidArg`
2. `LockingEntry_OrderByStartEndPrioritySequence`
3. `RequestLock_PublisherAccept_ReturnsGranted`
4. `RequestLock_PublisherConflict_SchedulesRetry`
5. `RequestLock_RetryBackoff_RespectsConfiguredBounds`
6. `RequestLock_NoWaitMode_ReturnsWaitingWhenNotTop`
7. `RequestLock_ApiWait_GrantsWhenTop`
8. `RequestLock_MissedWakeupRace_DoesNotHang`
9. `RequestLock_VersionChange_WakesWaiter`
10. `ReleaseLock_NotOwner_ReturnsNotOwner`
11. `ReleaseLock_Owner_SetsUnlocked`
12. `CancelRequest_WaitingRequest_Terminates`
13. `Shutdown_NewRequests_ReturnShutdown`
14. `Shutdown_WakesWaiters`
15. `Shutdown_CancelsPendingRetries`
16. `Observability_EmitPayloadSchema_Valid`
17. `Observability_EmitterFailure_DoesNotChangeResult`
18. `RequestLock_WaitTimeout_ExpiresWithTimeoutCode`
19. `RequestLock_CancelToken_InterruptsWithCancelledCode`
20. `Stress_HighContention_NoDeadlock`
21. `Stress_WorkerThreads_NeverBlockOnWait`

## 14) Definition of Done

- All sections implemented without contradiction.
- All tests in Section 13 pass with mandatory TDD loop evidence.
- No async worker waits.
- Missed-wakeup-safe wait contract verified by race tests.
- Docs/spec internally consistent and complete.
