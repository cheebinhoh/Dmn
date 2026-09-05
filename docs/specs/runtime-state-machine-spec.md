# Feature Spec: Runtime State Manager

Status: Draft

## 1. Summary

This feature introduces a new runtime-owned state execution manager that combines the existing `dmn-runtime` scheduler with the `dmn-state` finite-state helper. The result is a singleton runtime service that creates state objects for clients, serializes their execution through the runtime, and lets callers wait for completion without forcing the client thread to execute each state step directly.

The new feature preserves the library’s current design philosophy:

- `Dmn_Runtime_Manager` remains the process-wide scheduler and signal manager.
- `Dmn_State` remains the lightweight state-machine primitive.
- the new runtime state manager owns the scheduling and serialized execution policy.
- each state object is a runtime-managed, asynchronously executed state machine instance.

The primary behavior is:

1. client obtains a state handle from the `Dmn_Runtime_State_Manager` singleton;
2. client configures the state(s) on that object;
3. client calls `statehandle->run()` (optionally with priority / delay / onError handler);
4. `run()` enqueues a runtime task into the runtime manager and returns a boolean indicating whether the enqueue succeeded;
5. the runtime manager continues to repost tasks until the state object reaches its terminal condition; errors occurring in async execution invoke a client-provided onError callback (if supplied) and are captured on the state handle;
6. all state objects created by the manager are executed in serialized order through `Dmn_Runtime_Manager` (subject to the manager's serialization policy);
7. client may call `statehandle->wait()` or use the returned shared_future to block or asynchronously observe completion.

## 2. Design Objective

The existing `dmn-state` component is synchronous and client-driven. It calls `runNext()` directly in the caller thread. That is useful for local control flow, but not for runtime-managed workflows. The new runtime state manager changes the ownership model:

- the state object remains a state machine definition and execution state,
- the runtime manager owns when the state steps are executed,
- state execution is serialized within the runtime’s async context,
- the client receives an async completion signal via `wait()` or a shared_future rather than manually stepping the machine.

This makes the feature a natural fit for handshake flows, retries, startup/teardown workflows, protocol states, network state transitions, and any runtime pipeline that must share the same scheduling semantics as other runtime jobs.

## 3. Scope

### In Scope

- singleton runtime state manager class modeled after `Dmn_Runtime_Manager`
- state objects returned from the manager as managed handles (shared ownership) and that subclass `Dmn_State`
- state registration API for user-defined states
- `run()` scheduling through the runtime manager with onError callback forwarding to `dmn-runtime` semantics
- serialized execution of all runtime state objects (manager-global by default)
- `wait()` completion API for async execution with optional timeout and a shared_future-based async alternative
- shutdown, error, cancellation, and terminal-state handling
- tests for startup, normal completion, errors, cancellation, and lifetime edge cases

### Out of Scope

- distributed state replication
- persistence or recovery of state machines
- automatic consensus protocol orchestration
- general-purpose actor model features
- changes to existing `Dmn_State` sync API behavior

## 4. Architectural Context

Refer to `include/dmn-runtime.hpp`, `include/dmn-runtime-task.hpp`, and `include/dmn-state.hpp` for the runtime and state primitives that will be reused.

Key runtime types and semantics reused:

- `Dmn_Runtime_Job::Priority` and `Dmn_Runtime_Manager::addJob()` / `addTimedJob()`
- `Dmn_Runtime_Job::OnErrorFncType` (signature: `std::function<void(std::exception_ptr &)>`) — the onError callback type used by runtime jobs
- `Dmn_Runtime_Manager` owns the singleton async thread and exposes the public
  `isRunInAsyncThread()` query to detect runtime-thread context. This query
  does not alter scheduling state and is required by runtime-managed clients
  to reject operations that would deadlock the runtime thread.

## 5. Functional Requirements

### FR-1: Runtime state manager existence

A singleton class named `Dmn_Runtime_State_Manager` must exist and follow the same singleton creation conventions as `Dmn_Runtime_Manager`.

The manager must:

- use the inherited
  `Dmn_Singleton<Dmn_Runtime_State_Manager>::createInstance()` factory, which
  returns `std::shared_ptr<Dmn_Runtime_State_Manager>`
- own a runtime-managed execution queue for state objects
- ensure all state execution is scheduled through `Dmn_Runtime_Manager`

The manager must not declare a same-named `createInstance()` with a different
return type. A forwarding convenience function is permitted only when it has a
distinct name and preserves the singleton's shared ownership semantics.

The first implementation increment is limited to manager construction. It must
provide a focused unit test that calls
`Dmn_Runtime_State_Manager::createInstance()`, verifies the returned
`std::shared_ptr` is non-null, and verifies repeated calls return the same
manager instance. This increment uses
`include/dmn-runtime-state.hpp`, `src/dmn-runtime-state.cpp`, and
`test/dmn-test-runtime-state.cpp`, registered as `dmn-test-runtime-state`.
State creation, runtime scheduling, and lifecycle behavior are out of scope
for that increment.

### FR-2: Client-managed state object creation and ownership

Clients must be able to obtain a managed handle to a state object from the runtime state manager.

Ownership model (required):

- `createState()` MUST return a managed handle type: `std::shared_ptr<Dmn_Runtime_State>` (alias `DmnRuntimeStatePtr`) following the existing dmn pattern used by other components (for example, `dmn-dmesg`).
- The manager MUST retain a `std::shared_ptr` to the state object while it is queued or running. This guarantees the object remains alive until it reaches a terminal state even if the client drops its handle.
- When the object becomes terminal (completed/failed/cancelled), the manager releases its internal shared_ptr; any remaining client-held shared_ptr keeps the object alive until all references are dropped.
- Clients may intentionally drop their handle to rely on manager ownership for fire-and-forget semantics.

The resulting object must:

- be a concrete state object type derived from `Dmn_State`
- be created from the runtime state manager singleton and returned as a shared_ptr handle
- carry runtime-managed lifecycle metadata
- be configured by calling `setStateFnc()` or equivalent state registration methods

### FR-3: State configuration

A client must be able to define one or more states on the returned runtime state object via the compatible `Dmn_State` interface.

The manager must support:

- sequential state definition by appending state functors
- explicit transition to next state via `setNext()` and `setEnd()` semantics
- state functors that are valid in the same pattern as `Dmn_State`

### FR-4: `run()` dispatches work to runtime and error callback forwarding

The runtime state handle must expose a public `run()` method. In addition, `run()` MUST accept optional parameters for priority, delay (timed variant), and an onError callback that matches the runtime's `Dmn_Runtime_Job::OnErrorFncType` signature.

Behavior:

- `run()` may be called from any client thread EXCEPT the runtime async thread (calls from the runtime thread are disallowed — see `run()` thread policy below).
- `run()` must schedule asynchronous work on the singleton `Dmn_Runtime_Manager` async thread by calling `addJob()` or `addTimedJob()` as appropriate.
- `run()` returns `true` if the state was successfully queued (or started) and `false` if the enqueue failed (invalid state, already terminal, or internal error).
- `run()` is a one-shot operation for each state handle: subsequent `run()` calls after the first successful enqueue MUST be no-ops and MUST return `false`.
- The optional `onError` callback provided to `run()` MUST be forwarded to the underlying `dmn-runtime` job so that asynchronous runtime failures invoke the client callback when the runtime job reports an error. Use `Dmn_Runtime_Job::OnErrorFncType` as the canonical type.
- The runtime work must execute exactly one next state step of the state object per posted job; after the step executes, the manager reposts another job (or finalizes) until terminal.
- `run()` MUST NOT execute state logic synchronously in the caller thread.

Priority and timed variants:

- `run()` must accept an optional `Dmn_Runtime_Job::Priority` parameter and an optional timeout/delay parameter so callers can control priority and optionally schedule timed runs. The manager will map these directly to `Dmn_Runtime_Manager::addJob()` (immediate) or `addTimedJob()` (timed).

Thread policy for `run()` and `wait()`:

- If `run()` or `wait()` is called from inside the runtime async thread
  (detected via `Dmn_Runtime_Manager::isRunInAsyncThread()`), the
  implementation MUST throw `std::runtime_error`. This prevents deadlocks and
  enforces the rule that runtime-internal callbacks should not block the
  runtime.
- This policy applies in every build configuration, permitting direct,
  deterministic unit testing without a debug-only death test.

### FR-5: Serialized execution across all state objects

All state objects created from the runtime state manager must execute in serialized form through the shared runtime scheduler by default.

Requirements:

- no two state objects may run their next step concurrently in the same runtime manager (default global serialization)
- state object execution order must follow runtime job ordering/priority semantics
- state step tasks must be single-threaded relative to manager execution
- the manager MAY provide configuration for relaxed concurrency (optional extension) but default behavior must be serialized to match the spec

### FR-6: Completion waiting via `wait()` and async alternatives

Each runtime state object must support a `wait()` method and provide safer alternatives to avoid deadlocks.

Behavior:

- `wait()` blocks until the state object reaches its terminal state
- `wait()` MUST support an overload `wait_for(std::chrono::duration<...> timeout)` that returns a boolean indicating whether the wait observed terminal completion before the timeout
- `wait()` MUST NOT be called from the runtime async thread; implementations
  MUST throw `std::runtime_error` in every build configuration when detected
- The object MUST provide a `std::shared_future<void> getFuture()` so callers
  can use non-blocking or async wait patterns. The shared_future MUST be
  available immediately after state creation (before run()) but remains pending
  until the object reaches a terminal state. It becomes ready when execution
  completes, fails, or is cancelled before or after submission.
- Multiple threads/callers MAY wait concurrently on the same shared_future or call `wait()` concurrently; the implementation must support multiple waiters.

### FR-7: Terminal state and completion semantics

A runtime state object must expose completion behavior consistent with `Dmn_State` but with async runtime ownership.

The object must track:

- initialized state
- finalized state
- running state
- completed state
- failed state
- canceled state

A state object is terminal when it has either:

- reached the end via `setEnd()` or a terminal transition
- failed due to uncaught exception during a state step
- been canceled

### FR-8: Cancellation and shutdown

The runtime state object MUST provide a `cancel()` method that is cooperative in nature.

Semantics:

- `cancel()` is idempotent and sets the object's canceled flag and prevents further non-cooperative steps from being scheduled
- `cancel()` does NOT asynchronously preempt a currently executing state functor unless that functor explicitly checks a cancellation token exposed by the state and co-operates
- when the runtime task executes and detects the cancel flag is set, it MUST call `setEnd()` before invoking `runNext()` so that the state finalizes deterministically rather than executing further steps
- the manager must provide a way for state functors to query the cancel flag (e.g. `isCancelled()` or a cancellation token) so they can early-exit and perform graceful cleanup
- calling `cancel()` before `run()` transitions the object to cancelled
  terminal state immediately, completes its shared future, and causes any later
  `run()` call to return false
- on shutdown, the manager must support at least two modes: graceful (finish current step and queued states) and immediate (mark queued as cancelled and notify waiters). The exact mode shall be selectable via manager shutdown API.

### FR-9: Error propagation and onError callback

If a state callback throws while running inside the runtime-managed async thread, the runtime state manager must:

- capture the exception
- set the state object to failed terminal state
- notify any waiting client via `wait()` or shared_future
- invoke the onError callback supplied to `run()` (if any) with the runtime's error details using `Dmn_Runtime_Job::OnErrorFncType`
- avoid corrupting the runtime scheduler internal state
- complete the shared future with the captured exception so `getFuture().get()`
  rethrows it

## 6. Non-Functional Requirements

### NFR-1: Singleton and runtime-thread ownership

The runtime state manager must preserve the runtime’s model: client code may call APIs from any thread, but the actual state execution must be marshaled to the runtime async thread.

### NFR-2: Backward compatibility

This feature must not break the existing `Dmn_Runtime_Manager` or `Dmn_State` APIs. It is additive only.

### NFR-3: Determinism

State execution within one runtime manager must be deterministic with respect to queue ordering and posting order when preservation of ordering is requested by the client.

### NFR-4: Controlled memory lifetime (explicit)

The manager MUST hold a `std::shared_ptr` to any queued/running state object until terminal state is reached. This mirrors existing dmn ownership patterns (see `dmn-dmesg` for a similar handle + manager-owned shared_ptr pattern).

### NFR-5: Thread safety for wait semantics

`wait()` must be implemented using synchronization primitives appropriate for cross-thread signaling (`std::condition_variable`, flag, or equivalent) and must not busy-spin. Use of a `std::shared_future<void>` simplifies multi-waiter semantics.

## 7. Proposed API Shape

This API is proposed to match the existing library naming and runtime conventions while reflecting the ownership, error/cancel, priority/timed semantics and thread-safety rules requested.

```cpp
namespace dmn {

class Dmn_Runtime_State_Manager
    : public Dmn_Singleton<Dmn_Runtime_State_Manager> {
public:
  class Dmn_Runtime_State;

  // handle type returned to clients. Follows existing dmn shared ownership pattern.
  using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

  // Inherited factory:
  // std::shared_ptr<Dmn_Runtime_State_Manager> createInstance();

  // createState returns a shared_ptr handle. Manager will also keep a shared_ptr while the
  // state is queued or running to guarantee lifetime.
  DmnRuntimeStatePtr createState(std::string_view name);

  // optional manager-level shutdown / configuration APIs omitted for brevity
};

class Dmn_Runtime_State : public Dmn_State {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;
  using OnErrorFnc = Dmn_Runtime_Job::OnErrorFncType; // std::function<void(std::exception_ptr &)>

  explicit Dmn_Runtime_State(std::string_view name);

  // state configuration (same as Dmn_State)
  void setStateFnc(FncType fnc, int index = 0);
  void setNext(int index);
  void setNext();
  void setEnd();

  // lifecycle APIs
  // run: returns true when the enqueue succeeded; false on failure (already terminal, invalid, runtime busy)
  // optional onError is forwarded to the runtime job so callers get notified of async job failure.
  // priority and timed overloads are supported and map to addJob()/addTimedJob().
  bool run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
           const std::chrono::steady_clock::duration &delay = std::chrono::steady_clock::duration::zero(),
           OnErrorFnc onError = {});

  // cancel is cooperative and idempotent. If called before a step runs, the running task will
  // observe the cancelled flag, call setEnd() and transition to terminal state instead of executing further steps.
  void cancel();

  // wait blocks until terminal state. wait_for(timeout) returns true if it observed terminal state before timeout.
  void wait();
  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout);

  // future-based async alternative (shared_future supports multiple waiters and callers).
  std::shared_future<void> getFuture();

  // introspection
  // isRunning() indicates the handle has been queued or is actively running inside the manager
  bool isRunning() const;
  bool isCompleted() const;
  bool isFailed() const;
  bool isCancelled() const;

protected:
  void onStarted();
  void onCompleted();
  void onFailed(std::exception_ptr ep);
  void onCancelled();

private:
  // synchronization / state
  std::mutex m_waitMutex;
  std::condition_variable m_waitCv;

  // promise/future pair: keep a shared_future so multiple waiters are supported
  std::promise<void> m_completionPromise;
  std::shared_future<void> m_completionSharedFuture; // initialized from m_completionPromise.get_future().share()

  std::atomic_bool m_running{false};   // set when a runtime task is active
  std::atomic_bool m_completed{false}; // terminal
  std::atomic_bool m_failed{false};    // terminal
  std::atomic_bool m_cancelled{false}; // set by cancel()
  std::atomic_bool m_queued{false};    // set before enqueue to avoid duplicates

  // captured failure for diagnostics
  std::mutex m_failureMutex;
  std::exception_ptr m_failureEp{nullptr};
};

} // namespace dmn
```

### API Notes

- `Dmn_Runtime_State_Manager::createInstance()` is inherited from
  `Dmn_Singleton` and returns `std::shared_ptr<Dmn_Runtime_State_Manager>`.
  Clients retain that manager handle while using it.
- `Dmn_Runtime_State_Manager::createState()` returns a
  `std::shared_ptr<Dmn_Runtime_State>` handle. The manager retains a shared_ptr
  while the state is queued/running to ensure safe lifetime.
- `Dmn_Runtime_State::run(priority, delay, onError)` returns a boolean: true on successful enqueue, false if enqueue failed (e.g., already terminal or invalid state). If `delay` is non-zero, the manager must use `addTimedJob()`; otherwise `addJob()` is used.
- `run()` accepts an optional onError callback that uses the runtime's `Dmn_Runtime_Job::OnErrorFncType` signature and is forwarded to the runtime job.
- `run()` is one-shot: a successful `run()` prevents subsequent `run()` calls from enqueueing again; such subsequent calls return `false` (no-op). This avoids duplicate enqueues across threads.
- `cancel()` is cooperative: it sets a cancelled flag. The runtime job, before calling `runNext()`, must check `isCancelled()` and call `setEnd()` if the state has been cancelled so that the state finalizes without executing further steps.
- `wait()` supports a timeout variant and `getFuture()` returns a `std::shared_future<void>` that can be used by multiple waiters. The shared_future is valid immediately after createState() is called and resolves when the state reaches a terminal condition.
- Calls to `run()` or `wait()` from inside the runtime async thread must throw
  `std::runtime_error`. Use the public
  `Dmn_Runtime_Manager::isRunInAsyncThread()` query to detect and enforce
  this.

## 8. Execution Model

### 8.1 State object lifecycle

A runtime state object has the following lifecycle:

1. Created by `Dmn_Runtime_State_Manager::createState()` and returned as a shared_ptr handle
2. Configured by setting state functors (`setStateFnc`, etc.)
3. Idle before `run()` is called
4. Queued for runtime execution after `run()` (manager retains shared_ptr and sets `m_queued` atomically)
5. Running inside runtime async thread
6. Finalized once terminal condition reached (manager releases internal shared_ptr)
7. Client may call `wait()` at any time after submission or use the shared_future returned by `getFuture()`

### 8.2 `run()` exact semantics and atomic queued flag

When `statehandle->run(priority, delay, onError)` is called:

1. the state object must be validated for legal execution
2. the implementation MUST atomically set the `m_queued` flag (e.g., using compare_exchange) to avoid races where multiple threads try to enqueue simultaneously; only the thread that successfully sets `m_queued` proceeds to request the manager to retain an internal shared_ptr and submit the runtime job
3. if `m_queued` was already true or the object is terminal, `run()` returns false (no-op)
4. the manager stores a shared_ptr to the object (ensuring lifetime) and schedules a runtime job via `addJob()` or `addTimedJob()` depending on `delay`
5. the scheduled job executes exactly one next state step using the state object
6. before calling `runNext()`, the runtime job MUST check the cancel flag; if cancelled, it MUST call `setEnd()` and finalize instead of running the step
7. after the state step finishes, the runtime manager checks whether another state remains and reposts a new runtime job if appropriate
8. if no state remains, the object transitions to completed terminal state, the manager notifies waiters (set promise) and releases its internal shared_ptr
9. if a state callback throws, the exception is captured, the object transitions to failed, waiters are notified, and the optional onError callback is invoked

This loop continues until no more states to run. The runtime manager is responsible for re-posting tasks while the object remains active.

### 8.3 Serialization requirement and optional config

All state object tasks must be executed in serialized form through the runtime queue by default. This strict global serialization simplifies reasoning and matches the project's stated intent. Implementations SHOULD provide configuration for relaxed concurrency (e.g., per-manager worker count) as an optional extension, but that must be explicitly chosen and documented by the caller.

## 9. State Object Contract

### 9.1 Subclassing `Dmn_State`

The new runtime state object must subclass `Dmn_State` and preserve `Dmn_State` semantics while adding runtime lifecycle tracking.

It must retain:

- `setStateFnc()`, `setNext()`, `setEnd()`, `runNext()` semantics
- init/finalize behavior inherited from the base
- default state sequencing model

The runtime layer adds async ownership, completion signaling, cancellation token, and error forwarding on top of the base semantics.

### 9.2 Cancellation contract

- `cancel()` sets the cancellation flag and is safe to call from any thread.
- The runtime job MUST observe `isCancelled()` before executing `runNext()` and call `setEnd()` to force deterministic finalization.
- State functors MAY query `isCancelled()` if they want to implement cooperative cancellation.

### 9.3 `wait()` behavior and deadlock avoidance

- `wait()` blocks until the object is terminal.
- Implementations MUST detect calls from the runtime async thread and throw
  `std::runtime_error` as described.
- `wait_for(timeout)` returns a bool indicating whether the wait observed terminal completion before the timeout expired.
- `getFuture()` returns a `std::shared_future<void>` available immediately after creation and resolves on terminal state.

## 10. Detailed Behavior and Edge Cases

### 10.1 No state configured

If no state is defined before `run()`, the manager must not enqueue an invalid
task. `run()` returns false, leaves the state unsubmitted and non-terminal, and
does not invoke onError.

### 10.2 Repeated `run()` calls

A runtime state object must not be run multiple times simultaneously.

Behavior:

- The first successful `run()` enqueues the object and returns true.
- Subsequent `run()` calls (concurrent or later) MUST return false (no-op). This avoids duplicate enqueues and is thread-safe due to the atomic `m_queued` guard.

### 10.3 Finalized or canceled states

Once finalized, failed, or canceled, no further state step may be scheduled.

### 10.4 Exception propagation and onError

If a state callback throws while inside the runtime manager:

- capture the exception in `std::exception_ptr` stored on the object
- transition object to failed terminal state
- set the completion promise and notify any shared_future waiters
- invoke the optional onError callback provided to `run()` with the captured exception (using `Dmn_Runtime_Job::OnErrorFncType`)
- ensure runtime queue remains healthy

### 10.5 Shutdown race and modes

Manager shutdown must support at least two modes (configurable when shutting down):

- graceful: do not accept new state runs; let currently running state steps complete and drain queued states before finalizing them (clients may call wait() to observe completion)
- immediate: cancel queued (but not yet running) states and mark them cancelled; running steps proceed to completion or observe cancellation cooperatively

The spec requires tests for both modes.

## 11. Serialization and Scheduling Contract

The manager is responsible for ensuring serialized processing across all state instances it creates (by default). Each queued job should trigger exactly one `runNext()` invocation and then repost if required.

Provide clear priority mapping between manager jobs and other runtime jobs. The manager must not starve other runtime jobs; use the runtime's priority scheme and document how manager tasks are enqueued.

## 12. Test Plan (expanded)

### Unit Tests (additions focusing on the requested gaps)

- `create_state_from_runtime_engine`
- `state_run_posts_runtime_job`
- `state_wait_completes_after_all_steps`
- `state_serialization_maintains_order`
- `state_exception_marks_failed`
- `state_cancel_marks_cancelled`
- `state_no_state_defined_fails_cleanly`
- `run_is_rejected_when_state_already_active`

Additional edge-case tests (required):

- `destructor_while_queued`: verify manager-held shared_ptr keeps object alive while queued and that finalization occurs correctly when the client drops its handle
- `wait_from_runtime_thread_detected`: ensure wait() from the runtime thread
  throws `std::runtime_error`
- `cancel_before_run_or_queued`: calling cancel() before a queued step prevents runNext() and finalizes the state
- `cancel_during_queued_execution`: cancellation while queued leads the runtime job to call setEnd() rather than executing further steps
- `run_onerror_callback_invoked`: ensure onError passed to run() is invoked when the runtime job fails
- `shutdown_graceful_vs_immediate`: test both shutdown modes and their effects on queued and running states
- `getFuture_shared_waiters_before_run`: verify multiple waiters using getFuture() before run() all get signaled on terminal condition
- `run_priority_and_timed_variant`: verify run(priority, delay) maps to addJob/addTimedJob and honors priority ordering

### Integration Tests

- run multiple runtime state objects in the same runtime manager
- verify that state steps are serialized in posting order
- verify wait() and shared_future complete after sequence completion
- verify runtime jobs remain valid when the state object reaches terminal condition
- stress test many queued states to measure the effect of global serialization

### Stress / Regression Tests

- repeated create/run/wait cycles
- many state objects queued sequentially
- failure followed by cleanup and reuse
- shutdown while state objects are queued

## 13. Acceptance Criteria

The feature is accepted when all of the following are true:

- clients can create runtime-managed state objects from a singleton manager using a shared_ptr handle
- state objects subclass `Dmn_State` and retain base state semantics
- `statehandle->run(priority, delay, onError)` schedules work into the runtime manager and returns true/false to indicate success
- state execution is serialized through the runtime manager by default
- `statehandle->wait()` and `wait_for()` block until runtime completion or terminal failure and `getFuture()` is available for async waiting (shared_future)
- `cancel()` cooperatively finalizes execution and prevents future steps; cancel semantics are documented and tested
- exceptions and cancellation leave the runtime in a valid state and optional onError callbacks are invoked
- documentation, examples and tests exist for typical runtime state workflow usage and lifetime edge cases

## 14. Risks and Mitigations (updated)

### Risk: re-entrant scheduling loop
Mitigation: `run()` must schedule a single runtime task per state step and stop when the terminal condition is reached. No unbounded recursive scheduling loop is allowed. Atomic queued flag prevents duplicate enqueues.

### Risk: wait deadlock
Mitigation: do not call `wait()` from inside the runtime async thread. Throw
`std::runtime_error` in every build configuration. Prefer future-based waiting
from runtime thread contexts.

### Risk: queued state object lifetime issues
Mitigation: manager must hold a `std::shared_ptr` while queued. Returning a shared_ptr to clients and holding an internal shared_ptr mirrors the existing dmn pattern used in other subsystems (see `dmn-dmesg`).

### Risk: serializer starvation
Mitigation: the runtime manager must keep job postings small, deterministic, and bounded; the runtime's priority and scheduling mechanisms should be used to avoid starvation. Consider adding a configurable fairness mechanism for long-running state functors.

## 15. Implementation Notes

This feature should be implemented as an additive API layered on top of the existing runtime and state components.

Implementation should reuse:

- `Dmn_Runtime_Manager` for the process-wide scheduler
- `Dmn_State` for the state machine mechanics
- `Dmn_Runtime_Job` and `Dmn_Runtime_Task` for runtime dispatch

The runtime state manager should primarily add:

- state object lifecycle tracking using shared_ptr handles
- queueing and serialization logic
- `wait()` synchronization (condition_variable + promise/shared_future)
- terminal-state finalization and onError callback forwarding
- cooperative cancel() semantics

Sample usage (illustrative):

```cpp
using StatePtr = dmn::Dmn_Runtime_State_Manager::DmnRuntimeStatePtr;

// create
auto manager = dmn::Dmn_Runtime_State_Manager::createInstance();
StatePtr s = manager->createState("example");

// configure
s->setStateFnc([](dmn::Dmn_Runtime_State &st){ /* step 0 */ }, 0);
s->setNext();
s->setStateFnc([](dmn::Dmn_Runtime_State &st){ /* step 1 */ }, 1);
s->setEnd();

// run with onError callback, medium priority, immediate
bool ok = s->run(Dmn_Runtime_Job::Priority::kMedium, std::chrono::steady_clock::duration::zero(),
                 [](std::exception_ptr &ep){ /* log or inspect error */ });
if (!ok) { /* handle enqueue failure */ }

// wait (blocking)
s->wait();

// or async
auto fut = s->getFuture();
fut.wait();
```

## 16. Definition of Done

The feature is complete when:

- the singleton runtime state manager is designed and documented with
  `Dmn_Singleton` shared-pointer ownership
- the runtime state object class is specified and matches the required async semantics (run returning bool, onError forwarding, cancel, wait/timeout/shared_future)
- `run()` and `wait()` behavior are documented and tested
- serialized execution through `Dmn_Runtime_Manager` is verified
- shutdown, failure, and cancellation semantics are validated
- example usage patterns are included in the documentation

## 17. Recommended Milestones

1. Create the manager singleton and state object base model (shared_ptr handle)
2. Add runtime scheduling and serialized execution loop with onError forwarding, priority and timed variants
3. Add completion/failure/cancel tracking, wait(timeout), and shared_future support
4. Add tests for execution order, completion, failures, destructor-while-queued, and shutdown modes
5. Validate the runtime integration with `Dmn_Runtime_Manager`

---

This specification is derived from current runtime and state abstractions in:

- `include/dmn-runtime.hpp`
- `include/dmn-runtime-task.hpp`
- `include/dmn-state.hpp`
