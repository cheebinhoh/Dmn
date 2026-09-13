# Feature Spec: DMN Distributed Locking (Dmn_DLock)

Status: Proposed (implementation-ready).

## 1. Purpose

Define a production-grade distributed lock subsystem for DMN that is explicit
enough for strict spec-driven implementation by either humans or AI agents,
with deterministic behavior and test-first development.

## 2. Repository-Grounded Design Decisions

This spec intentionally follows existing repository patterns:

- Singleton creation/lifetime pattern: `include/dmn-singleton.hpp`
- Manager-owned shared object + weak proxy handle pattern:
  `include/dmn-dmesg.hpp` (`openHandler` / `closeHandler`)
- Runtime-state lifecycle + future/wait style:
  `include/dmn-runtime-state.hpp`
- Test target registration style: `test/CMakeLists.txt`

## 3. Core Decision: Dmn_DLock vs Dmn_DMesg Relationship

### Decision

`Dmn_DLock_Manager` **composes** `Dmn_DMesg` (optional) and **does not inherit**
from `Dmn_DMesg`.

### Rationale

- Lock correctness must be independent of message transport.
- `Dmn_DMesg` can be used for diagnostics/events, not lock state authority.
- Composition preserves clean boundaries and simpler failure semantics.

## 4. Scope

### In scope (Phase 1)

- Lease-based exclusive lock per key.
- Blocking + non-blocking acquisition.
- Renew + release with ownership checks.
- Fencing token monotonicity per key.
- Manager-owned lease objects with proxy handles.
- Deterministic shutdown semantics.
- Complete TDD/contract/integration test suite.

### Out of scope (Phase 1)

- Multi-key atomic lock transactions.
- RW lock mode.
- Cross-region consensus orchestration.

## 5. Type System and Class Hierarchy

## 5.1 Namespaces and Files

- `include/dmn-dlock.hpp`
- `include/dmn-dlock-backend.hpp`
- `src/dmn-dlock.cpp`
- `src/dmn-dlock-backend-memory.cpp` (reference backend for tests)
- `test/dmn-test-dlock.cpp`

## 5.2 Class hierarchy (normative)

```text
dmn::Dmn_DLock_Manager : public dmn::Dmn_Singleton<Dmn_DLock_Manager>
  ├─ class Dmn_DLockLease
  └─ class Dmn_DLockLeaseProxy
```

Required friend declaration:

- `friend class dmn::Dmn_Singleton<Dmn_DLock_Manager>;`

## 5.3 Ownership model (must mirror Dmn_DMesg)

Exactly mirror the memory ownership model used by `Dmn_DMesg` handlers:

- Manager stores active leases in
  `std::unordered_map<std::string, std::shared_ptr<Dmn_DLockLease>>` keyed by
  `lease_id` for O(1)-style close/remove lookup under churn.
- Caller receives `Dmn_DLockLeaseProxy` with `std::weak_ptr<Dmn_DLockLease>`.
- `closeLease(LeaseType&)` explicitly releases/unregisters and resets proxy.
- A closed/expired proxy must not access released lease object.

## 6. Public API Contract (final)

## 6.1 Fundamental types

- `using Dmn_DLock_Key = std::string;`
- `using Dmn_DLock_Token = uint64_t;`
- `using Dmn_DLock_Duration = std::chrono::milliseconds;`

## 6.2 Config and options

```cpp
struct Dmn_DLock_ManagerConfig {
  std::shared_ptr<Dmn_DLock_Backend> backend;
  std::shared_ptr<Dmn_DLock_Clock> clock;
  std::string owner_id;               // required, opaque random token
  bool enable_dmesg_events{false};
  Dmn_DMesg *dmesg{nullptr};          // optional diagnostics channel
};

struct Dmn_DLock_AcquireOptions {
  Dmn_Runtime_Job::Priority priority{Dmn_Runtime_Job::Priority::kMedium};
  std::chrono::milliseconds initial_backoff{1};
  std::chrono::milliseconds max_backoff{200};
  double jitter_ratio{0.20};
  std::shared_ptr<std::atomic_bool> cancel_token{}; // optional
};
```

## 6.3 Result types

Header declaration order requirement:

1. Declare `Dmn_DLockLease` and `Dmn_DLockLeaseProxy` first.
2. Then declare `Dmn_DLock_AcquireResult`/`Dmn_DLock_OpResult` using `LeaseType`.

```cpp
struct Dmn_DLock_AcquireResult {
  enum class Code {
    kAcquired,
    kBusy,
    kTimeout,
    kCancelled,
    kBackendError,
    kInvalidArg
  };

  bool ok{false};
  Code code{Code::kInvalidArg};
  std::string message;
  // set only when ok=true
  LeaseType lease;
};

struct Dmn_DLock_OpResult {
  enum class Code {
    kOk,
    kCancelled,
    kBackendError,
    kInvalidArg,
    kNotOwner,
    kExpired
  };

  bool ok{false};
  Code code{Code::kInvalidArg};
  std::string message;
  // set on renew success; backend time domain milliseconds
  std::optional<uint64_t> expires_at_ms;
};

struct Dmn_DLock_ManagerCreateResult {
  enum class Code {
    kOk,
    kInvalidConfig,
    kBackendInitFailed,
    kClockInitFailed
  };

  bool ok{false};
  Code code{Code::kInvalidConfig};
  std::string message;
  bool reused_existing{false};
  // singleton shared_ptr type used across this repository
  std::shared_ptr<Dmn_DLock_Manager> manager;
};
```

## 6.4 Lease and proxy API

```cpp
class Dmn_DLock_Manager::Dmn_DLockLease {
public:
  auto key() const -> const Dmn_DLock_Key &;
  auto ownerId() const -> const std::string &;
  auto leaseId() const -> const std::string &;
  auto fencingToken() const -> Dmn_DLock_Token;
  auto expiresAtMs() const -> uint64_t;            // backend time domain
  auto acquireGeneration() const -> uint64_t;
  auto isValid() const -> bool;
};

class Dmn_DLock_Manager::Dmn_DLockLeaseProxy {
  friend class Dmn_DLock_Manager;
public:
  auto lockShared() const -> std::shared_ptr<Dmn_DLockLease>;
  explicit operator bool() const noexcept;
private:
  void reset() noexcept;
  std::weak_ptr<Dmn_DLockLease> m_lease;
};

using LeaseType = Dmn_DLock_Manager::Dmn_DLockLeaseProxy;
```

Proxy behavior:

- `lockShared()` returns null if lease already closed/reset.
- Callers should use `if (lease)` then `auto l = lease.lockShared()` before access.

## 6.5 Manager API signatures (normative)

```cpp
class Dmn_DLock_Manager : public dmn::Dmn_Singleton<Dmn_DLock_Manager> {
public:
  static auto createManager(const Dmn_DLock_ManagerConfig &config)
      -> Dmn_DLock_ManagerCreateResult;

  auto tryAcquire(const Dmn_DLock_Key &key,
                  Dmn_DLock_Duration lease_ttl,
                  const Dmn_DLock_AcquireOptions &opts = {})
      -> Dmn_DLock_AcquireResult;

  auto acquire(const Dmn_DLock_Key &key,
               Dmn_DLock_Duration lease_ttl,
               Dmn_DLock_Duration wait_timeout,
               const Dmn_DLock_AcquireOptions &opts = {})
      -> Dmn_DLock_AcquireResult;

  auto renew(LeaseType &lease, Dmn_DLock_Duration lease_ttl)
      -> Dmn_DLock_OpResult;

  auto release(LeaseType &lease)
      -> Dmn_DLock_OpResult;
  // release postcondition: lease proxy is reset/closed on successful backend
  // release or successful no-op release outcome.

  auto closeLease(LeaseType &lease)
      -> Dmn_DLock_OpResult;
  // closeLease is idempotent hard-close: best-effort release + always reset proxy.

  auto isHeldByCaller(const Dmn_DLock_Key &key) const
      -> bool;

  auto shutdownCutoffGeneration() const
      -> uint64_t;

  void shutdown();
};
```

Singleton contract:

- `createManager()` must call `Dmn_Singleton<Dmn_DLock_Manager>::createInstance(...)`.
- It must never create more than one manager instance.
- Repeated successful calls return `kOk` with `reused_existing=true`;
  backend/clock init failure codes apply only before
  the singleton exists.

## 7. Backend Contract (interface)

```cpp
class Dmn_DLock_Backend {
public:
  virtual ~Dmn_DLock_Backend() = default;
  virtual auto initialize() -> bool = 0;

  struct AcquireReply {
    bool acquired{false};
    bool busy{false};
    uint64_t fencing_token{0};
    uint64_t expires_at_ms{0};
    uint64_t version{0};
    std::string owner_id;
    std::string lease_id;
    std::string error;
  };

  struct RenewReply {
    enum class Code { kOk, kNotOwner, kExpired, kBackendError };
    Code code{Code::kBackendError};
    uint64_t expires_at_ms{0};
    std::string error;
  };

  struct ReleaseReply {
    enum class Code {
      kReleased,
      kNoopMissingOrExpired,
      kNoopActiveOtherOwner,
      kBackendError
    };
    Code code{Code::kBackendError};
    std::string error;
  };

  virtual auto tryAcquire(const Dmn_DLock_Key &key,
                          const std::string &owner_id,
                          const std::string &lease_id,
                          uint64_t now_ms,
                          uint64_t ttl_ms)
      -> AcquireReply = 0;

  virtual auto renew(const Dmn_DLock_Key &key,
                     const std::string &owner_id,
                     const std::string &lease_id,
                     uint64_t now_ms,
                     uint64_t ttl_ms)
      -> RenewReply = 0;

  virtual auto release(const Dmn_DLock_Key &key,
                       const std::string &owner_id,
                       const std::string &lease_id,
                       uint64_t now_ms)
      -> ReleaseReply = 0;
};

class Dmn_DLock_Clock {
public:
  virtual ~Dmn_DLock_Clock() = default;
  virtual auto initialize() -> bool = 0;
  virtual auto nowMs() const -> uint64_t = 0; // backend time domain
};
```

## 8. Behavioral Requirements

- `lease_ttl` must be > 0 for `tryAcquire`, `acquire`, `renew`.
- `wait_timeout` may be 0 (`acquire` behaves as one immediate attempt).
- Shutdown precedence: after shutdown starts, `Cancelled` overrides `Busy`.
- Post-shutdown acquire mapping:
  - `tryAcquire` -> `Dmn_DLock_AcquireResult::Code::kCancelled`
  - `acquire` -> `Dmn_DLock_AcquireResult::Code::kCancelled`
- `renew` allowed post-shutdown only for leases acquired before cutoff generation.
- `release` and `closeLease` remain allowed post-shutdown.
- `expiresAtMs` values are backend time-domain milliseconds (no local translation).

## 9. Error/Exception Policy

- Runtime lock operations are result-code driven:
  `tryAcquire`, `acquire`, `renew`, `release`, `closeLease`.
- `createManager` is result-code driven.
- Proxy access uses `lockShared()` null checks (non-throwing).

## 10. Observability Contract

Required event outcomes:

- `acquire_attempt`
- `acquire_acquired`
- `acquire_busy`
- `acquire_timeout`
- `acquire_cancelled`
- `renew_ok`
- `renew_not_owner`
- `renew_expired`
- `release_mutated`
- `release_noop_missing_or_expired`
- `release_noop_active_other_owner`
- `release_backend_error`

Minimum event payload schema (all events):

- `event_name: string`
- `lock_key: string`
- `owner_id: string`
- `lease_id: string` (empty when unavailable)
- `fencing_token: uint64` (0 when unavailable)
- `result_code: string`
- `backend_now_ms: uint64`
- `manager_generation: uint64`
- `message: string` (optional diagnostic)

## 11. Implementation Plan (step-by-step, implementation-ready)

### 11.0 Mandatory TDD execution loop (for every test case)

For each test in Section 12, execute this exact loop:

1. Add exactly one new test case.
2. Build the target.
3. Run the new test (or focused filter) and confirm it fails for the expected reason.
4. Implement minimal code change for that test only.
5. Build again.
6. Re-run the same focused test and confirm it passes.
7. Run the full `dmn-test-dlock` target to guard regressions.
8. Proceed to the next test.

Required checkpoint commands (example form; adapt to project scripts):

- Build checkpoint: `cmake --build <build_dir> --target dmn-test-dlock`
- Discover exact CTest name checkpoint: `ctest --test-dir <build_dir> -N`
- Focused test checkpoint: `ctest --test-dir <build_dir> -R <exact_ctest_name_or_regex> --output-on-failure`
- Full target checkpoint (normative): run exact discovered dlock CTest entry by
  exact-name regex (for example `-R '^dmn-test-dlock$'` when applicable).

### Phase 0 — Scaffolding and type contracts

1. Create files in Section 5.1.
2. Declare all structs/enums/signatures from Sections 6 and 7.
3. Add friend declaration for singleton access.
4. Add CMake entries:
   - source compilation in `src/CMakeLists.txt`
   - new test target entry `dmn-test-dlock` in `test/CMakeLists.txt`.
5. Write compile-only tests for API type existence.
6. **Build checkpoint**: build `dmn-test-dlock` after scaffolding compiles.

### Phase 1 — Manager creation and singleton wiring

1. Implement `createManager(config)` validation.
2. Validate non-null backend/clock and non-empty owner_id.
3. Call `backend->initialize()` and `clock->initialize()` exactly once before
   first singleton creation.
4. Call singleton `createInstance(...)`.
5. Return `Dmn_DLock_ManagerCreateResult` codes.
6. Tests:
   - same shared_ptr returned across repeated create calls
   - invalid config error mapping
7. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
8. **Build checkpoint**: clean build after completing all Phase 1 tests.

### Phase 2 — Lease proxy ownership (DMesg pattern)

1. Implement `Dmn_DLockLease` fields + getters.
2. Implement `LeaseType` proxy weak_ptr semantics.
3. Implement manager internal lease storage container.
4. Implement proxy reset path.
5. Tests:
   - valid proxy after acquire
   - closed proxy after `closeLease`
   - `lockShared()` returns null after close
6. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
7. **Build checkpoint**: clean build after Phase 2 logical group.

### Phase 3 — tryAcquire()

1. Implement key/ttl validation.
2. Generate `lease_id` for each acquire attempt.
3. Call backend `tryAcquire(...)` once.
4. Map backend reply to acquire result codes.
5. On success:
   - create lease object
   - set acquire generation
   - retain shared_ptr in manager
   - return proxy in result.
6. Tests:
   - free key acquires
   - held key returns busy
   - backend error mapping
7. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
8. **Build checkpoint**: clean build after Phase 3.

### Phase 4 — release() and closeLease()

1. Implement release path using lease identity fields.
   - Postcondition: on released/no-op success, local proxy is reset/closed and
     manager lease retention is removed.
2. Map backend release replies:
   - released -> `kOk`
   - noop missing/expired -> `kOk`
   - noop active other owner -> `kNotOwner`
   - backend error -> `kBackendError`
3. Implement `closeLease(LeaseType&)`:
   - call `release(lease)` first
   - if release returns backend error, keep manager retention + proxy unchanged.
   - if release is `kOk`/`kNotOwner`, remove local retention and reset proxy.
   - Postcondition: hard close only on non-backend-error outcomes.
4. Tests:
   - owner release
   - stale active-other-owner release returns `kNotOwner`
   - missing/expired lease release returns `kOk` no-op
   - close resets proxy and frees manager ownership
5. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
6. **Build checkpoint**: clean build after Phase 4.

### Phase 5 — renew()

1. Validate lease proxy open + ttl.
2. Check shutdown-generation policy.
3. Call backend renew.
4. Map reply to op result codes.
5. On success update lease expiration in object and result.
6. Tests:
   - renew success
   - not-owner
   - expired
   - post-shutdown allowed for pre-cutoff lease
   - post-shutdown denied for non-eligible lease
7. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
8. **Build checkpoint**: clean build after Phase 5.

### Phase 6 — acquire() blocking path

1. Validate args.
2. If `wait_timeout == 0`, do one `tryAcquire` attempt.
3. Else retry loop:
   - start time from clock
   - backoff + jitter bounded by options
   - stop on acquired/timeout/cancelled/shutdown.
4. Tests:
   - eventual success
   - timeout
   - cancellation
   - shutdown precedence over busy
5. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
6. **Build checkpoint**: clean build after Phase 6.

### Phase 7 — shutdown and generation cutoff

1. Add `m_shutdown_started` and `m_shutdown_cutoff_generation`.
2. On shutdown start, atomically set cutoff as first disallowed generation.
3. Reject new acquires after shutdown.
4. Keep release/closeLease allowed.
5. Tests:
   - reject new acquires
   - boundary lease acquired immediately pre-shutdown remains renewable
   - pending waiters cancelled
6. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
7. **Build checkpoint**: clean build after Phase 7.

### Phase 8 — observability + DMesg composition hook

1. Add optional event emitter interface.
2. If configured, publish structured events to DMesg wrapper.
3. Ensure observability failure does not affect lock correctness:
   - event publish failures are swallowed from lock API results
   - optional internal debug logging only; no result-code mutation.
4. Tests:
   - event emission for each required outcome
   - payload schema field validation for each event
   - no correctness regression when emitter disabled/fails
5. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
6. **Build checkpoint**: clean build after Phase 8.

### Phase 9 — integration + stress

1. Multi-thread contention tests (2/8/32 contenders).
2. Crash/restart takeover via lease expiry.
3. Renew under intermittent backend failure.
4. High churn across many keys.
5. No dual-owner overlap assertions.
6. **TDD checkpoint per test**: fail first, implement, build, pass, run full dlock tests.
7. **Build checkpoint**: clean build after Phase 9 and before merge.

## 12. Detailed TDD Matrix (must implement in order)

Execution rule for every item below: add test -> build -> run (fail) -> implement
minimal code -> build -> run (pass) -> run full `dmn-test-dlock`.

1. `CreateManager_InvalidConfig_ReturnsInvalidConfig`
2. `CreateManager_RepeatedCalls_ReturnSingleton`
3. `TryAcquire_FreeKey_ReturnsLeaseProxy`
4. `TryAcquire_HeldKey_ReturnsBusy`
5. `Release_OwnerLease_ReturnsOk`
6. `Release_StaleLease_ReacquiredByOtherOwner_ReturnsNotOwner`
7. `CloseLease_ResetsProxy_AndDropsManagerRetention`
8. `Renew_ValidLease_ReturnsUpdatedExpiry`
9. `Renew_NotOwner_ReturnsNotOwner`
10. `Renew_ExpiredLease_ReturnsExpired`
11. `Acquire_WaitTimeoutZero_PerformsSingleAttempt`
12. `Acquire_ShutdownPrecedence_ReturnsCancelled`
13. `Acquire_TimesOut_WhenLockStaysBusy`
14. `Acquire_CancelToken_ReturnsCancelled`
15. `Shutdown_RejectsNewAcquire`
16. `Shutdown_BoundaryPreShutdownLease_RenewStillAllowed`
17. `FencingToken_MonotonicAcrossTransfers`
18. `Contention_MultiThread_NoDualOwnerOverlap`
19. `Observability_EmitsAcquireBusy_WithRequiredPayloadFields`
20. `Observability_EmitsReleaseNoopOtherOwner_WithRequiredPayloadFields`
21. `Observability_EmitterFailure_DoesNotChangeLockCorrectness`

## 13. Definition of Done

- All signatures in Section 6 implemented exactly.
- All backend behaviors in Section 7 implemented and contract-tested.
- All tests in Section 12 pass consistently.
- `dmn-test-dlock` integrated in existing test pipeline.
- No unresolved correctness ambiguity in shutdown/ownership semantics.
