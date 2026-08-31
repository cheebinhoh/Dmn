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

1. client creates a state object from the `Dmn_Runtime_State_Engine` singleton;
2. client configures the state(s) on that object;
3. client calls `stateobject.run()`;
4. `run()` enqueues a runtime task into the runtime engine;
5. the runtime engine continues to repost tasks until the state object reaches its terminal condition;
6. all state objects created by the engine are executed in serialized order through `Dmn_Runtime_Manager`;
7. client may call `stateobject.wait()` to block until the runtime completes the state object.

## 2. Design Objective

The existing `dmn-state` component is synchronous and client-driven. It calls `runNext()` directly in the caller thread. That is useful for local control flow, but not for runtime-managed workflows. The new runtime state engine changes the ownership model:

- the state object remains a state machine definition and execution state,
- the runtime engine owns when the state steps are executed,
- state execution is serialized within the runtime’s async context,
- the client receives an async completion signal via `wait()` rather than manually stepping the machine.

This makes the feature a natural fit for handshake flows, retries, startup/teardown workflows, protocol states, network state transitions, and any runtime pipeline that must share the same scheduling semantics as other runtime jobs.

## 3. Scope

### In Scope

- singleton runtime state engine class modeled after `Dmn_Runtime_Manager`
- state objects returned from the engine that subclass `Dmn_State`
- state registration API for user-defined states
- `run()` scheduling through the runtime engine
- serialized execution of all runtime state objects
- `wait()` completion API for async execution
- shutdown, error, cancellation, and terminal-state handling
- tests for startup, normal completion, errors, and cancellation

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

### FR-2: Client-managed state object creation

Clients must be able to create a state object from the runtime state engine.

The resulting object must:

- be a concrete state object type derived from `Dmn_State`
- be created from the runtime state engine singleton
- carry runtime-managed lifecycle metadata
- be configured by calling `setStateFnc()` or equivalent state registration methods

### FR-3: State configuration

A client must be able to define one or more states on the returned runtime state object via the compatible `Dmn_State` interface.

The engine must support:

- sequential state definition by appending state functors
- explicit transition to next state via `setNext()` and `setEnd()` semantics
- state functors that are valid in the same pattern as `Dmn_State`

### FR-4: `run()` dispatches work to runtime

The runtime state object must expose a public `run()` method.

Behavior:

- `run()` may be called from any client thread
- `run()` must schedule asynchronous work on the singleton `Dmn_Runtime_Manager` async thread
- the runtime work must execute the next state step of the state object
- after the step executes, the engine must post another runtime task to run the next state, continuing until no more states remain
- `run()` must not directly execute state logic in the caller thread

### FR-5: Serialized execution across all state objects

All state objects created from the runtime state engine must execute in serialized form through the shared runtime scheduler.

Requirements:

- no two state objects may run their next step concurrently in the same runtime engine
- state object execution order must follow runtime job ordering/priority semantics
- state step tasks must be single-threaded relative to engine execution

### FR-6: Completion waiting via `wait()`

Each runtime state object must support a `wait()` method.

Behavior:

- `wait()` blocks until the state object reaches its terminal state
- `wait()` must be safe to call from arbitrary client threads
- `wait()` must not race with runtime completion
- `wait()` returns when the state object has completed, failed, or been canceled

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

If runtime shutdown or cancellation occurs while a state object is queued or running, the engine must ensure the runtime state object is stopped deterministically and may invoke final cleanup hooks.

The object must not leave the runtime in a partially queued state.

### FR-9: Error propagation

If a state callback throws while running inside the runtime-managed async thread, the runtime state engine must:

- capture the exception
- set the state object to failed terminal state
- notify any waiting client via `wait()`
- avoid corrupting the runtime scheduler internal state

## 6. Non-Functional Requirements

### NFR-1: Singleton and runtime-thread ownership

The runtime state engine must preserve the runtime’s model: client code may call APIs from any thread, but the actual state execution must be marshaled to the runtime async thread.

### NFR-2: Backward compatibility

This feature must not break the existing `Dmn_Runtime_Manager` or `Dmn_State` APIs. It is additive only.

### NFR-3: Determinism

State execution within one runtime engine must be deterministic with respect to queue ordering and posting order.

### NFR-4: Controlled memory lifetime

The runtime state engine must own or manage the lifecycle of runtime state objects so that they are not destroyed while their task is still pending.

### NFR-5: Thread safety for wait semantics

`wait()` must be implemented using synchronization primitives appropriate for cross-thread signaling (`std::condition_variable`, flag, or equivalent) and must not busy-spin.

## 7. Proposed API Shape

This API is proposed to match the existing library naming and runtime conventions.

```cpp
namespace dmn {

class Dmn_Runtime_State_Engine
    : public Dmn_Singleton<Dmn_Runtime_State_Engine> {
public:
  static auto createInstance() -> Dmn_Runtime_State_Engine &;

  class Dmn_Runtime_State;

  Dmn_Runtime_State createState(std::string_view name);
  void runState(Dmn_Runtime_State &state);
};

class Dmn_Runtime_State : public Dmn_State {
public:
  using FncType = std::function<void(Dmn_Runtime_State &)>;

  explicit Dmn_Runtime_State(std::string_view name);

  void setStateFnc(FncType fnc, int index = 0);
  void setNext(int index);
  void setNext();
  void setEnd();

  void run();
  void wait();

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
  std::mutex m_waitMutex;
  std::condition_variable m_waitCv;
  std::atomic_bool m_running{false};
  std::atomic_bool m_completed{false};
  std::atomic_bool m_failed{false};
  std::atomic_bool m_cancelled{false};
  std::atomic_bool m_queued{false};
  std::atomic_bool m_waiting{false};
};

} // namespace dmn
```

### API Notes

- `Dmn_Runtime_State_Engine::createState()` returns a state object associated with the singleton runtime engine.
- `Dmn_Runtime_State` inherits from `Dmn_State` and adds async runtime lifecycle behavior.
- `run()` does not execute the state in caller thread; it only schedules runtime execution.
- `wait()` blocks until the state object completes, fails, or cancels.
- The engine serializes all created state objects through a common runtime queue.

## 8. Execution Model

### 8.1 State object lifecycle

A runtime state object has the following lifecycle:

1. Created by `Dmn_Runtime_State_Engine::createState()`
2. Configured by setting state functors (`setStateFnc`, etc.)
3. Idle before `run()` is called
4. Queued for runtime execution after `run()`
5. Running inside runtime async thread
6. Finalized once terminal condition reached
7. Client may call `wait()` at any time after submission

### 8.2 `run()` exact semantics

When `stateobject.run()` is called:

1. the state object must be validated for legal execution
2. if not already running/completed, it is marked queued
3. a runtime job is scheduled in the runtime engine
4. the scheduled job executes the next state step using the state object
5. after the state step finishes, the runtime engine checks whether another state remains
6. if another state exists, the engine posts a new runtime job for the next step
7. if no state remains, the object transitions to completed terminal state
8. all waiting consumers are notified

This loop continues until no more states to run. The runtime engine is responsible for re-posting tasks while the object remains active.

### 8.3 Serialization requirement

All state object tasks must be executed in serialized form through the runtime queue. That means:

- no direct parallel execution of multiple runtime state objects
- step execution order is driven by runtime job posting order
- the runtime scheduler remains the single authority on dispatch

This requirement is intentionally stricter than simply “each object runs in its own task”; it guarantees a predictable runtime execution model that matches the rest of the library.

## 9. State Object Contract

### 9.1 Subclassing `Dmn_State`

The new runtime state object must subclass `Dmn_State` and preserve `Dmn_State` semantics while adding runtime lifecycle tracking.

It must retain:

- `setStateFnc()`, `setNext()`, `setEnd()`, `runNext()` semantics
- init/finalize behavior inherited from the base
- default state sequencing model

The runtime layer adds async ownership and completion signaling on top of the base semantics.

### 9.2 State execution in runtime context

A runtime state object must execute its step logic inside the runtime async thread, not in the caller thread. This is required to guarantee serialized execution and consistent signal behavior.

### 9.3 `wait()` behavior

`wait()` should be implemented as follows:

- if the state is already completed, return immediately
- if the state is still queued or running, block until the terminal condition is reached
- if the state fails or cancels, the wait returns after the failure/cancel status is finalized

## 10. Detailed Behavior and Edge Cases

### 10.1 No state configured

If no state is defined before `run()`, the engine must not enqueue an invalid task. The object should transition to failed or finalized with a clear error condition.

### 10.2 Repeated `run()` calls

A runtime state object must not be run multiple times simultaneously.

Behavior:

- if `run()` is called while already queued or running, it should be ignored or return false
- repeated `run()` calls after completion should be rejected or treated as no-op depending on implementation policy

Recommended behavior: reject with an error/exception in the engine if the state is already active.

### 10.3 Finalized or canceled states

Once finalized, failed, or canceled, no further state step may be scheduled.

### 10.4 Exception propagation

If a state callback throws while inside the runtime engine:

- the current state machine transitions to failed
- the exception is captured in `std::exception_ptr`
- any waiters are notified
- the runtime queue remains valid

### 10.5 Shutdown race

If `exitMainLoop()` is called while a runtime state object is being processed, the state object should be finished or cancelled in a deterministic way without corrupting runtime scheduler state.

## 11. Serialization and Scheduling Contract

The engine is responsible for ensuring serialized processing across all state instances it creates.

Core contract:

- every runtime state object instance is queued as a runtime job
- runtime jobs are executed through `Dmn_Runtime_Manager`
- the runtime engine maintains a serialized dispatcher for all queued state jobs
- each runtime state job triggers exactly one state step, then posts another job if needed

This design matches the rest of the runtime library while introducing a higher-level state lifecycle.

## 12. Test Plan

### Unit Tests

- `create_state_from_runtime_engine`
- `state_run_posts_runtime_job`
- `state_wait_completes_after_all_steps`
- `state_serialization_maintains_order`
- `state_exception_marks_failed`
- `state_cancel_marks_cancelled`
- `state_no_state_defined_fails_cleanly`
- `run_is_rejected_when_state_already_active`

### Integration Tests

- run multiple runtime state objects in the same runtime engine
- verify that state steps are serialized in posting order
- verify `wait()` returns after sequence completion
- verify runtime jobs remain valid when the state object reaches terminal condition

### Stress / Regression Tests

- repeated create/run/wait cycles
- many state objects queued sequentially
- failure followed by cleanup and reuse
- shutdown while state objects are queued

## 13. Acceptance Criteria

The feature is accepted when all of the following are true:

- clients can create runtime-managed state objects from a singleton engine
- state objects subclass `Dmn_State` and retain base state semantics
- `stateobject.run()` schedules work into the runtime engine instead of running directly in client thread
- state execution is serialized through the runtime engine
- `stateobject.wait()` blocks until runtime completion or terminal failure
- exceptions and cancellation leave the runtime in a valid state
- documentation and examples exist for typical runtime state workflow usage

## 14. Risks and Mitigations

### Risk: re-entrant scheduling loop
Mitigation: `run()` must schedule a single runtime task per state step and stop when the terminal condition is reached. No unbounded recursive scheduling loop is allowed.

### Risk: wait deadlock
Mitigation: do not call `wait()` from inside the runtime async thread. The runtime state engine must document that `wait()` is a client-side synchronization method.

### Risk: queued state object lifetime issues
Mitigation: the runtime engine must hold ownership or lifecycle references to queued state objects until they are completed or canceled.

### Risk: serializer starvation
Mitigation: the runtime engine must keep job posting small, deterministic, and bounded; no state object should be allowed to monopolize the queue indefinitely.

## 15. Implementation Notes

This feature should be implemented as additive API layered on top of the existing runtime and state components, not as a rewrite of them.

Implementation should reuse:

- `Dmn_Runtime_Manager` for the process-wide scheduler
- `Dmn_State` for the state machine mechanics
- `Dmn_Runtime_Job` and `Dmn_Runtime_Task` for runtime dispatch

The runtime state engine should primarily add:

- state object lifecycle tracking
- queueing and serialization logic
- `wait()` synchronization
- terminal-state finalization

## 16. Definition of Done

The feature is complete when:

- the singleton runtime state engine is designed and documented
- the runtime state object class is specified and matches the required async semantics
- `run()` and `wait()` behavior are documented and tested
- serialized execution through `Dmn_Runtime_Manager` is verified
- shutdown, failure, and cancellation semantics are validated
- example usage patterns are included in the documentation

## 17. Recommended Milestones

1. Create the engine singleton and state object base model
2. Add runtime scheduling and serialized execution loop
3. Add completion/failure/cancel tracking and `wait()` synchronization
4. Add tests for execution order, completion, and failures
5. Validate the runtime integration with `Dmn_Runtime_Manager`

---

This specification is derived from current runtime and state abstractions in:

- `include/dmn-runtime.hpp`
- `include/dmn-runtime-task.hpp`
- `include/dmn-state.hpp`
