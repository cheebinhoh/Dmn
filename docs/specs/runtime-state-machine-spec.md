# Feature Spec: Runtime State Engine

Status: Draft

## 1. Summary

This feature introduces a new runtime-owned state execution engine that combines the existing `dmn-runtime` scheduler with the `dmn-state` finite-state helper. The result is a singleton runtime service that creates state objects for clients, serializes their execution through the runtime, and lets callers wait for completion without forcing the client thread to execute each state step directly.

The new feature preserves the library’s current design philosophy:

- `Dmn_Runtime_Manager` remains the process-wide scheduler and signal manager.
- `Dmn_State` remains the lightweight state-machine primitive.
- the new runtime state engine owns the scheduling and serialized execution policy.
- each state object is a runtime-managed, asynchronously executed state machine instance.

The primary behavior is:

1. client obtains a state handle from the `Dmn_Runtime_State_Engine` singleton;
2. client configures the state(s) on that object;
3. client calls `statehandle->run()`;
4. `run()` enqueues a runtime task into the runtime engine and returns a boolean indicating whether the enqueue succeeded;
5. the runtime engine continues to repost tasks until the state object reaches its terminal condition; errors occurring in async execution invoke a client-provided onError callback (if supplied) and are captured on the state handle;
6. all state objects created by the engine are executed in serialized order through `Dmn_Runtime_Manager` (subject to the engine's serialization policy);
7. client may call `statehandle->wait()` or use the returned future to block or asynchronously observe completion.

## 2. Design Objective

The existing `dmn-state` component is synchronous and client-driven. It calls `runNext()` directly in the caller thread. That is useful for local control flow, but not for runtime-managed workflows. The new runtime state engine changes the ownership model:

- the state object remains a state machine definition and execution state,
- the runtime engine owns when the state steps are executed,
- state execution is serialized within the runtime’s async context,
- the client receives an async completion signal via `wait()` or a future rather than manually stepping the machine.

This makes the feature a natural fit for handshake flows, retries, startup/teardown workflows, protocol states, network state transitions, and any runtime pipeline that must share the same scheduling semantics as other runtime jobs.

## 3. Scope

### In Scope

- singleton runtime state engine class modeled after `Dmn_Runtime_Manager`
- state objects returned from the engine as managed handles (shared ownership) and that subclass `Dmn_State`
- state registration API for user-defined states
- `run()` scheduling through the runtime engine with onError callback forwarding to `dmn-runtime` semantics
- serialized execution of all runtime state objects (engine-global by default)
- `wait()` completion API for async execution with optional timeout and a future-based async alternative
- shutdown, error, cancellation, and terminal-state handling
- tests for startup, normal completion, errors, cancellation, and lifetime edge cases

### Out of Scope

- distributed state replication
- persistence or recovery of state machines
- automatic consensus protocol orchestration
- general-purpose actor model features
- changes to existing `Dmn_State` sync API behavior

## 4. Architectural Context

### Existing Runtime Architecture

The current runtime API is singleton-based and uses:

- `Dmn_Runtime_Manager<QueueType>::createInstance()` for process-wide lifecycle
- `addJob()` / `addTimedJob()` for queued work
- `enterMainLoop()` / `exitMainLoop()` for the runtime loop
- `Dmn_Runtime_Job` with priority and coroutine execution support
- `Dmn_Runtime_Task` as the coroutine wrapper for job execution

The runtime currently serializes work via its async execution model, and this is the correct execution context for the new state engine.

### Existing State Machine Architecture

`Dmn_State` has the following current semantics:

- stores a vector of state functors
- uses `m_next` to drive execution
- supports `setStateFnc()`, `setNext()`, `setEnd()`, and `runNext()`
- defines init/finalize hooks
- uses `runNext()` directly in the caller context
- has no async lifecycle, no wait, and no runtime ownership

### Feature Intent

The runtime state engine is not a replacement for `Dmn_State`; it is a runtime-managed execution shell around it. Each state object created by the engine is still a `Dmn_State`, but with additional runtime lifetime controls and completion synchronization.

## 5. Functional Requirements

### FR-1: Runtime state engine existence

A singleton class named `Dmn_Runtime_State_Engine` must exist and follow the same singleton creation conventions as `Dmn_Runtime_Manager`.

The engine must:

- provide `createInstance()` or equivalent singleton factory consistent with `Dmn_Singleton`
- own a runtime-managed execution queue for state objects
- ensure all state execution is scheduled through `Dmn_Runtime_Manager`

### FR-2: Client-managed state object creation and ownership

Clients must be able to obtain a managed handle to a state object from the runtime state engine.

Ownership model (required):

- `createState()` MUST return a managed handle type: `std::shared_ptr<Dmn_Runtime_State>` (or an alias) following the existing dmn pattern used by other components (for example, `dmn-dmesg` and other resource holders in the codebase).
- The engine must retain a `std::shared_ptr` to the state object while it is queued or running. This guarantees the object remains alive until it reaches a terminal state even if the client drops its handle.
- When the object becomes terminal (completed/failed/cancelled), the engine releases its internal shared_ptr; any remaining client-held shared_ptr keeps the object alive until all references are dropped.
- The spec must include sample usage showing how clients keep a handle, but clients may also intentionally let the engine own a state for fire-and-forget semantics.

The resulting object must:

- be a concrete state object type derived from `Dmn_State`
- be created from the runtime state engine singleton and returned as a shared_ptr handle
- carry runtime-managed lifecycle metadata
- be configured by calling `setStateFnc()` or equivalent state registration methods

### FR-3: State configuration

A client must be able to define one or more states on the returned runtime state object via the compatible `Dmn_State` interface.

The engine must support:

- sequential state definition by appending state functors
- explicit transition to next state via `setNext()` and `setEnd()` semantics
- state functors that are valid in the same pattern as `Dmn_State`

### FR-4: `run()` dispatches work to runtime and error callback forwarding

The runtime state handle must expose a public `run()` method. In addition, `run()` MUST accept an optional onError callback that matches the `dmn-runtime` onError callback signature so callers can receive asynchronous error notifications.

Behavior:

- `run()` may be called from any client thread
- `run()` must schedule asynchronous work on the singleton `Dmn_Runtime_Manager` async thread
- `run()` returns `true` if the state was successfully queued (or started) and `false` if the enqueue failed (invalid state, already terminal, or internal error)
- an optional `onError` callback provided to `run()` is forwarded to the underlying `dmn-runtime` job so that asynchronous runtime failures invoke the client callback when the runtime job reports an error
- the runtime work must execute exactly one next state step of the state object per posted job
- after the step executes, the engine must post another runtime task to run the next state, continuing until no more states remain
- `run()` must not directly execute state logic in the caller thread

### FR-5: Serialized execution across all state objects

All state objects created from the runtime state engine must execute in serialized form through the shared runtime scheduler by default.

Requirements:

- no two state objects may run their next step concurrently in the same runtime engine (default global serialization)
- state object execution order must follow runtime job ordering/priority semantics
- state step tasks must be single-threaded relative to engine execution
- the engine MAY provide configuration for relaxed concurrency (optional extension) but default behavior must be serialized to match the spec

### FR-6: Completion waiting via `wait()` and async alternatives

Each runtime state object must support a `wait()` method and provide safer alternatives to avoid deadlocks.

Behavior:

- `wait()` blocks until the state object reaches its terminal state
- `wait()` MUST support an overload `wait(std::chrono::milliseconds timeout)` (or templated duration) that returns a boolean indicating whether the wait observed terminal completion before the timeout
- `wait()` MUST NOT be called from the runtime async thread; calling from the runtime thread is undefined — implementations SHOULD assert or return immediately with an error when detected
- in addition to blocking wait(), the object MUST provide a `std::future<void> getFuture()` (or equivalent) so callers can use non-blocking or async wait patterns on other threads or coroutines
- `wait()` and future completion must be signalled using a condition variable / promise and must handle spurious wakeups correctly

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
- the engine must provide a way for state functors to query the cancel flag (e.g. `isCancelled()` or a cancellation token) so they can early-exit and perform graceful cleanup
- on shutdown, the engine must support at least two modes: graceful (finish current step and queued states) and immediate (mark queued as cancelled and notify waiters). The exact mode shall be selectable via engine shutdown API.

### FR-9: Error propagation and onError callback

If a state callback throws while running inside the runtime-managed async thread, the runtime state engine must:

- capture the exception
- set the state object to failed terminal state
- notify any waiting client via `wait()` or future
- invoke the onError callback supplied by the client (if any) with the runtime's error details
- avoid corrupting the runtime scheduler internal state

## 6. Non-Functional Requirements

### NFR-1: Singleton and runtime-thread ownership

The runtime state engine must preserve the runtime’s model: client code may call APIs from any thread, but the actual state execution must be marshaled to the runtime async thread.

### NFR-2: Backward compatibility

This feature must not break the existing `Dmn_Runtime_Manager` or `Dmn_State` APIs. It is additive only.

### NFR-3: Determinism

State execution within one runtime engine must be deterministic with respect to queue ordering and posting order when preservation of ordering is requested by the client.

### NFR-4: Controlled memory lifetime (explicit)

The engine MUST hold a `std::shared_ptr` to any queued/running state object until terminal state is reached. This mirrors existing dmn ownership patterns (see `dmn-dmesg` for a similar handle + engine-owned shared_ptr pattern).

### NFR-5: Thread safety for wait semantics

`wait()` must be implemented using synchronization primitives appropriate for cross-thread signaling (`std::condition_variable`, flag, or equivalent) and must not busy-spin.

## 7. Proposed API Shape

This API is proposed to match the existing library naming and runtime conventions while reflecting the ownership and error/cancel semantics requested.

```cpp
namespace dmn {

class Dmn_Runtime_State_Engine
    : public Dmn_Singleton<Dmn_Runtime_State_Engine> {
public:
  static auto createInstance() -> Dmn_Runtime_State_Engine &;

  class Dmn_Runtime_State;

  // handle type returned to clients. Follows existing dmn shared ownership pattern.
  using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

  // createState returns a shared_ptr handle. Engine will also keep a shared_ptr while the
  // state is queued or running to guarantee lifetime.
  DmnRuntimeStatePtr createState(std::string_view name);

  // optional engine-level shutdown / configuration APIs omitted for brevity
};

class Dmn_Runtime_State : public Dmn_State {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;
  using OnErrorFnc = std::function<void(std::exception_ptr)>; // forwarded to runtime's onError

  explicit Dmn_Runtime_State(std::string_view name);

  // state configuration (same as Dmn_State)
  void setStateFnc(FncType fnc, int index = 0);
  void setNext(int index);
  void setNext();
  void setEnd();

  // lifecycle APIs
  // run: returns true when the enqueue succeeded; false on failure (already terminal, invalid, runtime busy)
  // optional onError is forwarded to the runtime job so callers get notified of async job failure.
  bool run(OnErrorFnc onError = nullptr);

  // cancel is cooperative and idempotent. If called before a step runs, the running task will
  // observe the cancelled flag, call setEnd() and transition to terminal state instead of executing further steps.
  void cancel();

  // wait blocks until terminal state. wait(timeout) returns true if it observed terminal state before timeout.
  void wait();
  template <typename Rep, typename Period>
  bool wait_for(const std::chrono::duration<Rep, Period> &timeout);

  // future-based async alternative
  std::future<void> getFuture();

  // introspection
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
  std::promise<void> m_completionPromise; // getFuture() returns m_completionPromise.get_future()

  std::atomic_bool m_running{false};
  std::atomic_bool m_completed{false};
  std::atomic_bool m_failed{false};
  std::atomic_bool m_cancelled{false};
  std::atomic_bool m_queued{false};
  std::atomic_bool m_waiting{false};

  // captured failure for diagnostics
  std::mutex m_failureMutex;
  std::exception_ptr m_failureEp{nullptr};
};

} // namespace dmn
```

### API Notes

- `Dmn_Runtime_State_Engine::createState()` returns a `std::shared_ptr<Dmn_Runtime_State>` handle. The engine retains a shared_ptr while the state is queued/running to ensure safe lifetime.
- `Dmn_Runtime_State::run(OnErrorFnc)` returns a boolean: true on successful enqueue, false if enqueue failed (e.g., already terminal or invalid state).
- `run()` accepts an optional onError callback that is forwarded to the `dmn-runtime` job plumbing; if the runtime reports an error, the callback will be invoked with the exception_ptr.
- `cancel()` is cooperative: it sets a cancelled flag. The runtime job, before calling `runNext()`, must check isCancelled() and call `setEnd()` if the state has been cancelled so that the state finalizes without executing further steps.
- `wait()` supports a timeout variant; a future is also available for non-blocking waits.

## 8. Execution Model

### 8.1 State object lifecycle

A runtime state object has the following lifecycle:

1. Created by `Dmn_Runtime_State_Engine::createState()` and returned as a shared_ptr handle
2. Configured by setting state functors (`setStateFnc`, etc.)
3. Idle before `run()` is called
4. Queued for runtime execution after `run()` (engine retains shared_ptr)
5. Running inside runtime async thread
6. Finalized once terminal condition reached (engine releases internal shared_ptr)
7. Client may call `wait()` at any time after submission or use the future returned by `getFuture()`

### 8.2 `run()` exact semantics

When `statehandle->run(onError)` is called:

1. the state object must be validated for legal execution
2. if not already running/completed, it is marked queued and the engine stores an internal shared_ptr
3. a runtime job is scheduled in the runtime engine; the job is created with the supplied onError callback forwarded to the runtime
4. the scheduled job executes exactly one next state step using the state object
5. before calling `runNext()`, the runtime job checks the cancel flag; if cancelled, it must call `setEnd()` and finalize instead of running the step
6. after the state step finishes, the runtime engine checks whether another state remains
7. if another state exists and not cancelled, the engine posts a new runtime job for the next step
8. if no state remains, the object transitions to completed terminal state and the engine notifies waiters (condition variable and promise)
9. if a state callback throws, the exception is captured, the object transitions to failed, waiters are notified, and the optional onError callback is invoked

This loop continues until no more states to run. The runtime engine is responsible for re-posting tasks while the object remains active.

### 8.3 Serialization requirement and optional config

All state object tasks must be executed in serialized form through the runtime queue by default. This strict global serialization simplifies reasoning and matches the project's stated intent. Implementations SHOULD provide configuration for relaxed concurrency (e.g., per-engine worker count) as an optional extension, but that must be explicitly chosen and documented by the caller.

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
- The runtime job must observe `isCancelled()` before executing `runNext()` and call `setEnd()` to force deterministic finalization.
- State functors may query `isCancelled()` if they want to implement cooperative cancellation.

### 9.3 `wait()` behavior and deadlock avoidance

- `wait()` blocks until the object is terminal.
- Implementations must detect (or document) that `wait()` MUST NOT be called from the runtime async thread. Preferably, `wait()` asserts or returns an error when invoked from runtime thread context.
- `wait_for(timeout)` returns a bool indicating whether the wait observed terminal completion before the timeout expired.
- `getFuture()` provides an async, non-blocking alternative which is safe to use from the runtime thread if the runtime supports async continuation semantics (otherwise the caller should still avoid awaiting it on the runtime thread).

## 10. Detailed Behavior and Edge Cases

### 10.1 No state configured

If no state is defined before `run()`, the engine must not enqueue an invalid task. The object should either immediately transition to failed with a clear exception captured (and optional onError invoked), or the run() call should return false indicating invalid configuration.

### 10.2 Repeated `run()` calls

A runtime state object must not be run multiple times simultaneously.

Behavior:

- if `run()` is called while already queued or running, it MUST return false
- repeated `run()` calls after completion MUST return false

This explicit boolean return covers the recommended behavior and aligns with the request.

### 10.3 Finalized or canceled states

Once finalized, failed, or canceled, no further state step may be scheduled.

### 10.4 Exception propagation and onError

If a state callback throws while inside the runtime engine:

- capture the exception in `std::exception_ptr` stored on the object
- transition object to failed terminal state
- set the promise / notify the condition variable so waiters/future observers are notified
- invoke the optional onError callback provided to `run()` with the captured exception
- ensure runtime queue remains healthy

### 10.5 Shutdown race and modes

Engine shutdown must support at least two modes (configurable when shutting down):

- graceful: do not accept new state runs; let currently running state steps complete and drain queued states before finalizing them (clients may call wait() to observe completion)
- immediate: cancel queued (but not yet running) states and mark them cancelled; running steps proceed to completion or observe cancellation cooperatively

The spec requires tests for both modes.

## 11. Serialization and Scheduling Contract

The engine is responsible for ensuring serialized processing across all state instances it creates (by default). Each queued job should trigger exactly one `runNext()` invocation and then repost if required.

Provide clear priority mapping between engine jobs and other runtime jobs. The engine must not starve other runtime jobs; use the runtime's priority scheme and document how engine tasks are enqueued.

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

- `destructor_while_queued`: verify engine-held shared_ptr keeps object alive while queued and that finalization occurs correctly when the client drops its handle
- `wait_from_runtime_thread_detected`: ensure wait() from runtime thread either asserts or returns error
- `cancel_before_run_or_queued`: calling cancel() before a queued step prevents runNext() and finalizes the state
- `cancel_during_queued_execution`: cancellation while queued leads the runtime job to call setEnd() rather than executing further steps
- `run_onerror_callback_invoked`: ensure onError passed to run() is invoked when the runtime job fails
- `shutdown_graceful_vs_immediate`: test both shutdown modes and their effects on queued and running states

### Integration Tests

- run multiple runtime state objects in the same runtime engine
- verify that state steps are serialized in posting order
- verify wait() and future complete after sequence completion
- verify runtime jobs remain valid when the state object reaches terminal condition
- stress test many queued states to measure the effect of global serialization

### Stress / Regression Tests

- repeated create/run/wait cycles
- many state objects queued sequentially
- failure followed by cleanup and reuse
- shutdown while state objects are queued

## 13. Acceptance Criteria

The feature is accepted when all of the following are true:

- clients can create runtime-managed state objects from a singleton engine using a shared_ptr handle
- state objects subclass `Dmn_State` and retain base state semantics
- `statehandle->run(onError)` schedules work into the runtime engine and returns true/false to indicate success
- state execution is serialized through the runtime engine by default
- `statehandle->wait()` and `wait_for()` block until runtime completion or terminal failure and `getFuture()` is available for async waiting
- `cancel()` cooperatively finalizes execution and prevents future steps; cancel semantics are documented and tested
- exceptions and cancellation leave the runtime in a valid state and optional onError callbacks are invoked
- documentation, examples and tests exist for typical runtime state workflow usage and lifetime edge cases

## 14. Risks and Mitigations (updated)

### Risk: re-entrant scheduling loop
Mitigation: `run()` must schedule a single runtime task per state step and stop when the terminal condition is reached. No unbounded recursive scheduling loop is allowed. Atomic queued flag prevents duplicate enqueues.

### Risk: wait deadlock
Mitigation: do not call `wait()` from inside the runtime async thread. Document and assert this in debug builds; prefer future-based wait from runtime thread contexts.

### Risk: queued state object lifetime issues
Mitigation: engine must hold a `std::shared_ptr` while queued. Returning a shared_ptr to clients and holding an internal shared_ptr mirrors the existing dmn pattern used in other subsystems (see `dmn-dmesg`).

### Risk: serializer starvation
Mitigation: the runtime engine must keep job postings small, deterministic, and bounded; the runtime's priority and scheduling mechanisms should be used to avoid starvation. Consider adding a configurable fairness mechanism for long-running state functors.

## 15. Implementation Notes

This feature should be implemented as an additive API layered on top of the existing runtime and state components.

Implementation should reuse:

- `Dmn_Runtime_Manager` for the process-wide scheduler
- `Dmn_State` for the state machine mechanics
- `Dmn_Runtime_Job` and `Dmn_Runtime_Task` for runtime dispatch

The runtime state engine should primarily add:

- state object lifecycle tracking using shared_ptr handles
- queueing and serialization logic
- `wait()` synchronization (condition_variable + promise/future)
- terminal-state finalization and onError callback forwarding
- cooperative cancel() semantics

Sample usage (illustrative):

```cpp
using StatePtr = dmn::Dmn_Runtime_State_Engine::DmnRuntimeStatePtr;

// create
StatePtr s = dmn::Dmn_Runtime_State_Engine::createInstance().createState("example");

// configure
s->setStateFnc([](dmn::Dmn_Runtime_State &st){ /* step 0 */ }, 0);
s->setNext();
s->setStateFnc([](dmn::Dmn_Runtime_State &st){ /* step 1 */ }, 1);
s->setEnd();

// run with onError callback
bool ok = s->run([](std::exception_ptr ep){ /* log or inspect error */ });
if (!ok) { /* handle enqueue failure */ }

// wait (blocking)
s->wait();

// or async
auto fut = s->getFuture();
fut.wait();
```

## 16. Definition of Done

The feature is complete when:

- the singleton runtime state engine is designed and documented with shared_ptr handle ownership
- the runtime state object class is specified and matches the required async semantics (run returning bool, onError forwarding, cancel, wait/timeout/future)
- `run()` and `wait()` behavior are documented and tested
- serialized execution through `Dmn_Runtime_Manager` is verified
- shutdown, failure, and cancellation semantics are validated
- example usage patterns are included in the documentation

## 17. Recommended Milestones

1. Create the engine singleton and state object base model (shared_ptr handle)
2. Add runtime scheduling and serialized execution loop with onError forwarding
3. Add completion/failure/cancel tracking, wait(timeout), and future support
4. Add tests for execution order, completion, failures, destructor-while-queued, and shutdown modes
5. Validate the runtime integration with `Dmn_Runtime_Manager`

---

This specification is derived from current runtime and state abstractions in:

- `include/dmn-runtime.hpp`
- `include/dmn-runtime-task.hpp`
- `include/dmn-state.hpp`
