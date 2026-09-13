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
- **Owner ID**: unique client identity (process UUID + instance metadata).
- **Lease**: finite ownership interval with expiration timestamp.
- **Fencing token**: strictly increasing numeric token issued on successful lock grant.
- **Backend**: storage/coordinator implementation used by lock manager.

## 4. High-Level Architecture

### 4.1 Components

1. `Dmn_DLock_Manager` (singleton)
   - User-facing API for acquire/renew/release.
   - Owns lifecycle, retry policy, and shutdown behavior.
2. `Dmn_DLock_Handle`
   - Represents one acquired lease.
   - Carries key, owner ID, lease ID, expiration, fencing token.
3. `Dmn_DLock_Backend` (interface)
   - Pluggable backend contract for compare-and-swap lock state.
4. `Dmn_DLock_Clock`
   - Monotonic time abstraction for deterministic testing.
5. `Dmn_DLock_Events`
   - Optional observability hooks for metrics/logging.

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
- Release is idempotent; releasing an already-expired or missing lease is safe.

## 5. Public API Requirements

## 5.1 Types

- `Dmn_DLock_Key = std::string`
- `Dmn_DLock_Token = uint64_t`
- `Dmn_DLock_Duration = std::chrono::milliseconds`

`Dmn_DLock_Result`:

- `bool ok`
- `enum Code { Acquired, Busy, Timeout, Cancelled, BackendError, InvalidArg, NotOwner, Expired }`
- `std::string message`

`Dmn_DLock_Handle`:

- `key()`
- `ownerId()`
- `leaseId()`
- `fencingToken()`
- `expiresAt()`
- `isValid()`

### 5.2 Manager API

- `tryAcquire(key, lease_ttl, opts) -> AcquireResult`
  - Returns immediately.
  - If busy, returns `Busy`.
- `acquire(key, lease_ttl, wait_timeout, opts) -> AcquireResult`
  - Retries until acquired, timeout, or cancellation.
- `renew(handle, lease_ttl) -> Result`
  - Extends lease only if current owner + lease ID match.
- `release(handle) -> Result`
  - Releases if owner matches; otherwise `NotOwner`.
- `isHeldByCaller(key) -> bool`
- `shutdown() -> void`
  - Cancels pending acquire waiters and prevents new acquisitions.

### 5.3 Behavioral Requirements

- Any API requiring positive duration must reject zero/negative durations.
- `acquire` timeout must be monotonic-clock based.
- `acquire` wait loop must support cancellation token.
- After `shutdown`, all acquire attempts fail with `Cancelled`.
- After `shutdown`, all renew attempts fail with `Cancelled`.
- Renewing after lease expiration returns `Expired`.

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
- manager shutdown

### FR-6: Idempotent Release

`release()` is idempotent only when the lease is already missing/expired and no
active owner exists for that lease. If the key is actively held by a different
owner, `release()` must return `NotOwner`.

### FR-7: Process Crash Tolerance

No explicit release is required for eventual progress; lease expiry enables
future acquisition by other clients.

### FR-8: Observability

Emit structured events/counters for acquire attempt, acquire success/failure,
renew success/failure, release, timeout, cancellation, backend error.

## 7. Error Model

- Validation errors: `InvalidArg`
- Contention: `Busy` or `Timeout`
- Ownership mismatch: `NotOwner`
- Lease stale: `Expired`
- Backend operation failure: `BackendError`
- Shutdown/cancellation: `Cancelled`

All operational errors must be surfaced through result codes and must not be
reported by exceptions.

## 8. Concurrency and Threading Model

- Manager methods are thread-safe.
- Backend interaction is serialized per key by CAS/version semantics, not by a
  global process lock.
- Local process optimization may use a striped mutex map per key to reduce
  duplicate backend load.
- No unbounded busy loops; all retries sleep/yield with jittered backoff.

## 9. Security and Abuse Considerations

- Owner IDs must be unguessable (UUID v4 or cryptographically random ID).
- API must not trust client wall clock for lease validity decisions.
- Fencing token must be propagated by lock users to guarded side effects.
- Logging must not leak secrets in lock key metadata.

## 10. Test-Driven Development (TDD) Strategy

## 10.1 TDD Rules

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
14. backend error propagation maps to `BackendError`.
15. invalid durations return `InvalidArg`.

### 10.4 Integration/Stress Test Matrix

- 2, 8, 32 contenders on one key with bounded fairness checks.
- Owner crash simulation (no release) followed by lease expiry takeover.
- Rapid renew loop under intermittent backend failures.
- High churn across many keys (hot/cold distribution).

## 11. Step-by-Step Implementation Plan

### Phase 0: Scaffolding and Contracts

1. Add public headers: `dmn-dlock.hpp`, `dmn-dlock-backend.hpp`.
2. Define enums/result structs and handle type.
3. Add backend abstract interface + fake backend for tests.
4. Add fake monotonic clock interface for deterministic tests.
5. Add test target `dmn-test-dlock` to build system.

### Phase 1: Non-blocking Acquire

1. Write failing tests for `tryAcquire` success + busy.
2. Implement manager construction and dependency wiring.
3. Implement backend CAS acquire path.
4. Return lock handle with fencing token and expiration.
5. Refactor result mapping and error helpers.

### Phase 2: Release and Ownership Validation

1. Write failing tests for owner release + idempotent release.
2. Implement release path with owner/lease validation.
3. Add stale-owner negative tests.
4. Refactor shared key validation paths.

### Phase 3: Renew Semantics

1. Write failing tests for renew success, not-owner, expired.
2. Implement renew CAS update path.
3. Validate expiration edge boundaries.
4. Refactor expiration utility helpers.

### Phase 4: Blocking Acquire + Cancellation

1. Write failing tests for acquire timeout and cancellation.
2. Implement retry loop with exponential backoff + jitter.
3. Add cancellation token plumbing and shutdown checks.
4. Add deterministic timing tests via fake clock.

### Phase 5: Shutdown and Lifecycle

1. Write failing tests for shutdown behavior.
2. Implement shutdown state flag and waiter cancellation.
3. Ensure no new acquire/renew operations proceed post-shutdown (policy-defined exceptions may keep release allowed).
4. Add race tests between shutdown and acquire.

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

Naming examples:

- `TryAcquire_FreeKey_ReturnsAcquired`
- `Acquire_Contention_TimesOut`
- `Renew_StaleLease_ReturnsNotOwner`
- `Release_AlreadyExpired_IsIdempotent`
- `Acquire_AfterShutdown_ReturnsCancelled`
- `ConcurrentTryAcquire_OnlyOneSucceeds`

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
