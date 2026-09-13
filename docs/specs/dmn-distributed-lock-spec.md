# Feature Spec: DMN Distributed Locking (Dmn_DLock)

Status: Proposed.

## 1. Summary

This specification defines a new distributed lock subsystem for DMN, named
`Dmn_DLock`, that provides lease-based mutual exclusion across multiple
processes and hosts. The design targets correctness first (safety and bounded
ownership), then operability (timeouts, cancellation, observability), and then
performance.

The lock service must integrate with the existing DMN runtime model and use
spec-driven, test-driven development (TDD) as the implementation method.

## 2. Goals and Non-Goals

### 2.1 Goals

- Provide mutually exclusive lock ownership for a named lock key across nodes.
- Provide lease semantics with bounded lock duration and renewal.
- Provide fencing tokens to protect downstream systems from stale lock holders.
- Provide blocking and non-blocking acquisition APIs.
- Provide deterministic behavior for timeout, cancellation, and process shutdown.
- Provide unit and integration test coverage before and during implementation.

### 2.2 Non-Goals (Phase 1)

- Cross-region consensus replication.
- Re-entrant locks across independent processes.
- Reader-writer lock mode.
- Transactional multi-lock atomic acquisition.

## 3. Terminology

- **Lock key**: globally unique string name for a protected resource.
- **Owner ID**: opaque, cryptographically random client identity token.
- **Owner metadata**: optional diagnostic data stored separately from owner ID.
- **Lease**: finite ownership interval with expiration timestamp.
- **Fencing token**: strictly increasing numeric token issued on successful lock grant.
- **Backend**: storage/coordinator implementation used by lock manager.

## 4. High-Level Architecture

### 4.1 Components

1. `Dmn_DLock_Manager` (singleton)
   - User-facing API for acquire/renew/release.
   - Owns lifecycle, retry policy, and shutdown behavior.
   - Tracks `shutdownCutoffGeneration` for post-shutdown renew eligibility.
2. `Dmn_DLockLease`
   - Represents one acquired lease.
   - Carries key, owner ID, lease ID, expiration, fencing token.
   - Carries manager `acquire_generation` captured at successful acquire time.
3. `Dmn_DLock_Backend` (interface)
   - Pluggable backend contract for compare-and-swap lock state.
4. `Dmn_DLock_Clock`
   - Monotonic time abstraction for deterministic testing.
5. `Dmn_DLock_Events`
   - Optional observability hooks for metrics/logging.

### 4.4 Class Hierarchy and DMesg Relationship (Normative)

Best-call decision: `Dmn_DLock` uses **composition**, not inheritance, with
`Dmn_DMesg`.

- `Dmn_DLock_Manager` **must not inherit** from `Dmn_DMesg`.
- `Dmn_DLock_Manager` may optionally compose a `Dmn_DMesg*` (or wrapper) for
  lock event publication/diagnostics.
- Lock correctness (acquire/renew/release CAS) must remain independent from
  message delivery state.

Normative class shape:

- `class Dmn_DLock_Manager : public dmn::Dmn_Singleton<Dmn_DLock_Manager>`
- `class Dmn_DLock_Manager::Dmn_DLockLease` (internal lease object)
- `class Dmn_DLock_Manager::Dmn_DLockLeaseProxy` (weak reference proxy)
- `using LeaseType = Dmn_DLock_Manager::Dmn_DLockLeaseProxy` (public alias)

Ownership model must mirror `Dmn_DMesg` handler pattern:

- DMesg pattern reference:
  - `openHandler(...) -> Dmn_DMesgHandlerProxy` (weak proxy to manager-owned shared_ptr)
  - `closeHandler(HandlerType&)` explicitly unregisters/frees and resets proxy
- DLock equivalent:
  - successful acquire returns `LeaseType`
  - manager owns live lease objects in `std::shared_ptr`
  - `closeLease(LeaseType&)` explicitly releases/free/reset proxy

### 4.2 Data Model (logical)

Per lock key, backend stores:

- `key: string`
- `owner_id: string`
- `lease_id: string`
- `fencing_token: uint64`
- `expires_at_ms: uint64` (backend-owned expiration timestamp in backend time domain)
- `version: uint64` (optimistic CAS version)

### 4.3 Safety Invariants

- At most one valid (non-expired) lease owner per lock key at any instant.
- Successful acquire always returns a fencing token greater than any prior token
  for that key.
- Renew is valid only for the current owner and lease ID.
- Release is idempotent for missing/expired leases; stale-handle release after
  expiry/reacquisition is a no-op success.

## 5. Public API Requirements

### 5.1 Types

- `Dmn_DLock_Key = std::string`
- `Dmn_DLock_Token = uint64_t`
- `Dmn_DLock_Duration = std::chrono::milliseconds`

`Dmn_DLock_AcquireResult`:

- `bool ok`
- `enum Code { Acquired, Busy, Timeout, Cancelled, BackendError, InvalidArg }`
- `LeaseType lease` (present when `ok=true`)
- `std::string message`

`Dmn_DLock_OpResult` (renew/release):

- `bool ok`
- `enum Code { Ok, Cancelled, BackendError, InvalidArg, NotOwner, Expired }`
- `std::optional<uint64_t> expires_at_ms` (backend time domain; set on successful renew)
- `std::string message`

`Dmn_DLock_ManagerCreateResult`:

- `bool ok`
- `std::shared_ptr<Dmn_DLock_Manager> manager` (this project's singleton handle type)
- `enum Code { Ok, InvalidConfig, BackendInitFailed, ClockInitFailed }`
- `std::string message`

`Dmn_DLock_Manager::Dmn_DLockLease`:

- `key()`
- `ownerId()`
- `leaseId()`
- `fencingToken()`
- `expiresAt()`
- `isValid()`
- `acquireGeneration()`

`Dmn_DLock_Manager::Dmn_DLockLeaseProxy` (DMesg-style proxy):

- stores `std::weak_ptr<Dmn_DLockLease>`
- `lockShared() -> std::shared_ptr<Dmn_DLockLease>` (returns null if closed)
- `isOpen() const noexcept -> bool`
- `reset()`

### 5.2 Manager API

- `createManager(const Dmn_DLock_ManagerConfig &config) -> Dmn_DLock_ManagerCreateResult`
  - Internally calls singleton `Dmn_Singleton<Dmn_DLock_Manager>::createInstance(...)`.
  - Never creates multiple manager instances; returned shared_ptr aliases the singleton.
  - Singleton lifecycle contract in this spec is shared_ptr-based (same as
    `Dmn_Singleton` in this repository).
- `tryAcquire(const Dmn_DLock_Key &key, Dmn_DLock_Duration lease_ttl, const Dmn_DLock_AcquireOptions &opts) -> Dmn_DLock_AcquireResult`
  - Returns immediately.
  - If busy, returns `Busy`.
- `acquire(const Dmn_DLock_Key &key, Dmn_DLock_Duration lease_ttl, Dmn_DLock_Duration wait_timeout, const Dmn_DLock_AcquireOptions &opts) -> Dmn_DLock_AcquireResult`
  - Retries until acquired, timeout, or cancellation.
- `renew(LeaseType &lease, Dmn_DLock_Duration lease_ttl) -> Dmn_DLock_OpResult`
  - Extends lease only if current owner + lease ID match.
  - On success, updates lease expiration and returns updated `expires_at_ms`.
- `release(LeaseType &lease) -> Dmn_DLock_OpResult`
  - Releases if owner matches; stale/missing lease release is idempotent success.
- `closeLease(LeaseType &lease) -> Dmn_DLock_OpResult`
  - DMesg-like explicit free path; performs best-effort release then resets proxy.
- `isHeldByCaller(const Dmn_DLock_Key &key) const -> bool`
  - `caller` means the manager instance's configured `owner_id`.
- `shutdown() -> void`
  - Cancels pending acquire waiters and prevents new acquisitions.
- `shutdownCutoffGeneration() const -> uint64_t`

### 5.3 Behavioral Requirements

- Any API requiring positive duration must reject zero/negative durations.
- `lease_ttl` must be strictly positive for `tryAcquire`, `acquire`, and `renew`.
- `wait_timeout` for `acquire` may be zero to request no-wait behavior
  (exact mapping: one immediate attempt; returns `Acquired` on success or `Busy`
  on contention).
- If shutdown has started, `Cancelled` takes precedence over `Busy` for
  zero-timeout `acquire`.
- `acquire` timeout must be monotonic-clock based.
- `acquire` wait loop must support cancellation token.
- After `shutdown`, `tryAcquire` fails with `Cancelled`.
- After `shutdown`, all acquire attempts fail with `Cancelled`.
- After `shutdown`, renew is allowed only for handles that were successfully
  acquired before shutdown began, identified by
  `handle.acquireGeneration() < manager.shutdownCutoffGeneration()` where
  `shutdownCutoffGeneration` is the first disallowed generation value (set when
  shutdown begins).
- After `shutdown`, `release` remains allowed and must preserve idempotent
  semantics.
- Renewing after lease expiration returns `Expired`.
- `expiresAt()` and `expires_at_ms` both use backend time-domain milliseconds
  (no local wall-clock translation in API).

## 6. Detailed Functional Requirements

### FR-1: Mutual Exclusion

For one lock key, exactly one non-expired owner may hold the lock at a time.
Concurrent successful acquires for the same key are forbidden.

### FR-2: Lease Expiration

Lease automatically becomes invalid after `expires_at_ms`.
Expired lease is considered free for next acquisition.

### FR-3: Fencing Token Monotonicity

Each successful lock grant increments and returns token `N+1`.
No successful acquire may return token <= last granted token.

### FR-4: Owner-Scoped Renew/Release

Renew/release must verify both `owner_id` and `lease_id`.
Requests from stale/non-owner callers must not mutate current owner state.

### FR-5: Blocking Acquire Semantics

`acquire()` retries using backoff + jitter until one of:

- lock granted
- wait timeout reached
- cancellation signaled
- manager shutdown (for new acquisitions)

### FR-6: Idempotent Release

`release()` is idempotent for stale/missing/expired handles. If the handle
matches active ownership, it releases ownership; otherwise it returns success
with no state change.

### FR-7: Process Crash Tolerance

No explicit release is required for eventual progress; lease expiry enables
future acquisition by other clients.

### FR-8: Observability

Emit structured events/counters for acquire attempt, acquire success/failure,
renew success/failure, timeout, cancellation, backend error, and release
outcomes:

- `release_mutated` (owner release performed)
- `release_noop_stale_or_missing` (idempotent no-op)
- `release_backend_error`

## 7. Error Model

- Validation errors: `InvalidArg`
- Contention: `Busy` or `Timeout`
- Ownership mismatch: `NotOwner` (renew path; release stale/non-owner is no-op success)
- Lease stale: `Expired`
- Backend operation failure: `BackendError`
- Shutdown/cancellation: `Cancelled`

All operational errors must be surfaced through result codes and must not be
reported by exceptions.

This non-throwing contract applies to runtime lock operations (`tryAcquire`,
`acquire`, `renew`, `release`, `closeLease`) and manager creation
(`createManager`).
Construction/setup failures must be exposed through
`Dmn_DLock_ManagerCreateResult` rather than constructor throws.

## 8. Concurrency and Threading Model

- Manager methods are thread-safe.
- Backend interaction is serialized per key by CAS/version semantics, not by a
  global process lock.
- Local process synchronization strategy is implementation-defined and must not
  change lock correctness semantics.
- No unbounded busy loops; all retries sleep/yield with jittered backoff.

## 9. Security and Abuse Considerations

- Owner IDs must be unguessable and metadata must not be embedded in owner ID
  bytes; optional metadata is stored separately.
- API must not trust client wall clock for lease validity decisions.
- Fencing token must be propagated by lock users to guarded side effects.
- Logging must not leak secrets in lock key metadata.

## 10. Test-Driven Development (TDD) Strategy

### 10.1 TDD Rules

1. Write failing test first.
2. Implement minimal code to pass.
3. Refactor without changing behavior.
4. Keep each commit scoped to one behavioral slice.
5. Preserve deterministic tests (mock clock/backend).

### 10.2 Test Layers

- **Unit tests**: manager logic with fake backend + fake clock.
- **Contract tests**: backend interface conformance suite reusable by any backend implementation.
- **Integration tests**: multi-thread contention and timing with real runtime.
- **Stress tests**: long-running contention and churn scenarios.

### 10.3 Unit Test Matrix

1. `tryAcquire` succeeds on free lock.
2. `tryAcquire` returns `Busy` on held lock.
3. `acquire` succeeds after lock becomes free.
4. `acquire` times out when lock remains held.
5. `acquire` exits on cancellation.
6. `renew` succeeds for current owner before expiration.
7. `renew` fails `NotOwner` for stale owner.
8. `renew` fails `Expired` after expiration.
9. `release` succeeds for owner.
10. `release` is idempotent when lease already gone.
11. fencing token increases on every new grant.
12. concurrent acquire: only one winner.
13. shutdown rejects new acquire and cancels waiters.
14. renew after shutdown succeeds only for pre-shutdown leases.
15. renew after shutdown fails for post-shutdown/rejected acquisition paths.
16. lease acquired at shutdown boundary (immediately pre-shutdown) remains renewable.
17. backend error propagation maps to `BackendError`.
18. invalid durations return `InvalidArg`.

### 10.4 Integration/Stress Test Matrix

- 2, 8, 32 contenders on one key with safety checks (single-owner invariant and
  no dual-owner overlap).
- Owner crash simulation (no release) followed by lease expiry takeover.
- Rapid renew loop under intermittent backend failures.
- High churn across many keys (hot/cold distribution).

### 10.5 Backend Contract Test Matrix

1. Acquire CAS succeeds only when key is free or lease is expired.
2. Acquire CAS rejects when an active lease exists for another owner.
3. Renew CAS succeeds only for matching `owner_id` + `lease_id`.
4. Renew CAS rejects stale/non-owner lease updates.
5. Release CAS succeeds for owner and is idempotent when lease is already gone.
6. Release CAS reports active-other-owner without mutation; manager maps that
   outcome to public API success-no-op semantics.
7. Fencing token increments strictly on successful ownership transfer.
8. Backend read/modify/write paths preserve per-key version monotonicity.

## 11. Step-by-Step Implementation Plan

### 11.0 Files, Classes, and Hierarchy (must implement first)

New files:

- `include/dmn-dlock.hpp`
- `include/dmn-dlock-backend.hpp`
- `src/dmn-dlock.cpp`
- `src/dmn-dlock-backend-memory.cpp` (test/dev backend)
- `test/dmn-test-dlock.cpp`

Class map:

- `dmn::Dmn_DLock_Manager` (singleton root)
- `dmn::Dmn_DLock_Manager::Dmn_DLockLease` (internal state object)
- `dmn::Dmn_DLock_Manager::Dmn_DLockLeaseProxy` (client handle proxy)
- `dmn::Dmn_DLock_Manager::LeaseType` (public alias)
- `dmn::Dmn_DLock_Backend` (backend interface)
- `dmn::Dmn_DLock_Clock` (clock abstraction)

API signatures to implement exactly:

- `static auto createManager(const Dmn_DLock_ManagerConfig &config) -> Dmn_DLock_ManagerCreateResult;`
- `auto tryAcquire(const Dmn_DLock_Key &key, Dmn_DLock_Duration lease_ttl, const Dmn_DLock_AcquireOptions &opts) -> Dmn_DLock_AcquireResult;`
- `auto acquire(const Dmn_DLock_Key &key, Dmn_DLock_Duration lease_ttl, Dmn_DLock_Duration wait_timeout, const Dmn_DLock_AcquireOptions &opts) -> Dmn_DLock_AcquireResult;`
- `auto renew(LeaseType &lease, Dmn_DLock_Duration lease_ttl) -> Dmn_DLock_OpResult;`
- `auto release(LeaseType &lease) -> Dmn_DLock_OpResult;`
- `auto closeLease(LeaseType &lease) -> Dmn_DLock_OpResult;`
- `auto isHeldByCaller(const Dmn_DLock_Key &key) const -> bool;`
- `auto shutdownCutoffGeneration() const -> uint64_t;`
- `void shutdown();`

### Phase 0: Scaffolding and Contracts

1. Add public headers and source files listed in 11.0.
2. Define `LeaseType` (alias to `Dmn_DLockLeaseProxy`) using DMesg-style weak proxy semantics.
3. Define result structs/enums and options structs.
4. Add backend abstract interface + in-memory fake backend for tests.
5. Add clock abstraction and fake clock for deterministic tests.
6. Add test target `dmn-test-dlock` to build system.

### Phase 1: Non-blocking Acquire

1. Write failing tests for `tryAcquire` success + busy.
2. Implement manager creation (`createManager`) with non-throwing result path.
3. Implement backend CAS acquire path.
4. Return `LeaseType` with `owner_id`, `lease_id`, `fencing_token`,
   `expires_at_ms`, and `acquire_generation`.
5. Refactor result mapping and error helpers.

### Phase 2: Release and Ownership Validation

1. Write failing tests for owner release + idempotent release.
2. Implement release path with owner/lease validation.
3. Implement `closeLease()` to mirror DMesg `closeHandler()` semantics:
   best-effort release + proxy reset.
4. Add stale-owner/no-op release tests and proxy reset tests.
5. Refactor shared key validation paths.

### Phase 3: Renew Semantics

1. Write failing tests for renew success, not-owner, expired.
2. Implement renew CAS update path.
3. Update lease object + `expires_at_ms` result on success.
4. Validate expiration edge boundaries.
5. Refactor expiration utility helpers.

### Phase 4: Blocking Acquire + Cancellation

1. Write failing tests for acquire timeout and cancellation.
2. Implement retry loop with exponential backoff + jitter.
3. Add cancellation token plumbing and shutdown checks.
4. Add deterministic timing tests via fake clock.

### Phase 5: Shutdown and Lifecycle

1. Write failing tests for shutdown behavior.
2. Implement shutdown state flag and waiter cancellation.
3. Capture `acquire_generation` on successful acquire and set
   `shutdownCutoffGeneration` when shutdown begins.
4. Ensure no new `tryAcquire`/`acquire` operations proceed post-shutdown, allow
   renew only for pre-shutdown handles, and keep `release` allowed/idempotent.
5. Add race tests between shutdown and acquire.

### Phase 6: Observability

1. Write failing tests for event emission on key transitions.
2. Implement metrics/event hook calls.
3. Validate no double-emission for retries unless intended.

### Phase 7: Integration and Hardening

1. Add multi-thread contention integration tests.
2. Add crash/expiry takeover tests.
3. Add backend-failure resilience tests.
4. Run stress suite and remove flakiness with deterministic controls.

### Phase 8: Documentation and Examples

1. Add usage documentation and fencing-token guidance.
2. Add minimal lock lifecycle sample.
3. Publish backend conformance checklist for future backends.

## 12. Unit Test File and Naming Plan

Suggested file: `test/dmn-test-dlock.cpp`

Suggested suites:

- `DmnDLockAcquireTest`
- `DmnDLockRenewTest`
- `DmnDLockReleaseTest`
- `DmnDLockShutdownTest`
- `DmnDLockConcurrencyTest`
- `DmnDLockLeaseProxyOwnershipTest` (DMesg-like lifetime/close behavior)

Naming examples:

- `TryAcquire_FreeKey_ReturnsAcquired`
- `Acquire_Contention_TimesOut`
- `Renew_StaleLease_ReturnsNotOwner`
- `Release_AlreadyExpired_IsIdempotent`
- `Acquire_AfterShutdown_ReturnsCancelled`
- `ConcurrentTryAcquire_OnlyOneSucceeds`
- `CloseLease_ResetsProxy_AndIsNoThrowOnClosedLease`
- `LeaseProxy_LockShared_ReturnsNullAfterCloseLease`

## 13. Definition of Ready

Implementation can start when:

- backend interface is approved,
- lock semantics and error codes are approved,
- timeout/cancellation semantics are approved,
- fencing token contract is approved.

## 14. Definition of Done

Feature is done when:

- all FR requirements are implemented,
- unit + integration tests pass,
- concurrency tests show single-owner safety,
- documentation includes fencing-token usage guidance,
- CI is green and no unresolved high-severity defects remain.
