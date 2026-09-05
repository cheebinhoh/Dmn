# Implementation Plan: Runtime State Manager

## 1. Goal

Implement the runtime state manager feature as a singleton runtime-owned state machine manager built on the existing `dmn-runtime` and `dmn-state` components.

## 2. Core Design Decisions

### Decision 1: manager is singleton and runtime-owned
The manager follows the same singleton model as `Dmn_Runtime_Manager`. It owns the execution policy for state objects and routes state execution through the runtime scheduler.

### Decision 2: state objects subclass `Dmn_State`
Each runtime state object remains a state machine, but adds async lifecycle metadata and `wait()` support. This keeps the base state semantics while adding runtime execution ownership.

### Decision 3: all state execution is serialized
The manager does not allow independent parallel execution of state steps across runtime state objects. It posts work to the runtime scheduler in serialized form.

### Decision 4: `run()` is async-only
The client never directly executes state logic in its own thread. `run()` only queues runtime tasks. The runtime executes each state step and then re-posts the next task until the machine is complete.

## 3. Phase 1: Construct the manager singleton (complete)

### Tasks

- define `Dmn_Runtime_State_Manager` singleton API
- use the inherited `Dmn_Singleton` shared-pointer `createInstance()` factory
- grant `Dmn_Singleton<Dmn_Runtime_State_Manager>` access to the protected
  manager constructor through a friend declaration
- add a focused unit test that constructs the singleton, verifies the returned
  shared pointer is non-null, and verifies repeated calls return the same
  manager instance
- defer lifecycle flags, waiting, scheduling, and ownership retention to later
  phases

### Deliverables

- public manager class declaration
- manager constructor/destructor implementation
- registered `dmn-test-runtime-state` target
- singleton construction test

### Completed follow-on increment: Create state handles

- define `DmnRuntimeStatePtr` as `std::shared_ptr<Dmn_Runtime_State>`
- implement `Dmn_Runtime_State_Manager::createState()` to return a newly
  constructed concrete runtime state
- define the runtime-state constructor, destructor, and default no-op
  lifecycle hooks
- extend the runtime-state test to verify non-null state creation and
  `Dmn_State` inheritance
- do not retain a manager-owned state handle until successful runtime queueing

## 4. Phase 2: Implement the completion model

### Tasks

- add runtime lifecycle flags (`queued`, `running`, `completed`, `failed`,
  `cancelled`)
- add `wait()` synchronization primitives and the shared future
- implement pre-run rejection and cancel-before-run terminal semantics

### Deliverables

- lifecycle state model
- completion waiting design

## 5. Phase 3: Integrate with runtime scheduler

### Tasks

- schedule state steps as runtime jobs using `Dmn_Runtime_Manager::addJob()`
- ensure `run()` posts work to runtime rather than executing directly
- serialize state object execution through the runtime queue
- implement continuation loop so each step schedules the next one until terminal state

### Deliverables

- runtime job adapter for state objects
- serialized manager dispatcher
- sequential execution loop

## 6. Phase 4: State lifecycle and completion

### Tasks

- implement transitioned completion behavior
- implement failed state handling for thrown exceptions
- implement cancellation and shutdown handling
- notify waiters exactly once
- avoid re-enqueuing after terminal state

### Deliverables

- terminal-state semantics
- exception-safe cleanup
- completion synchronization contract

## 7. Phase 5: API ergonomics and compatibility

### Tasks

- keep `Dmn_State` unchanged for synchronous scenarios
- provide a runtime-specific object for async execution
- preserve `setStateFnc()`, `setNext()`, `setEnd()`, and `runNext()` semantics
- document `wait()` semantics and restrictions

### Deliverables

- developer-facing API contract
- usage examples for initialization, run, and wait
- compatibility note for existing runtime and state users

## 8. Phase 6: Validation

### Tests to add

- state object created from manager
- multiple state objects serialized in order
- `run()` enqueues runtime work and completes successfully
- waiting on completion returns after all steps finish
- exception sets failed terminal state
- repeated run is rejected or ignored as designed
- cancellation during queued or running state does not corrupt runtime

### Validation commands

- `cmake -B build -DCMAKE_BUILD_TYPE=Debug`
- `cmake --build build`
- `ctest --test-dir build --output-on-failure`

## 9. Risks and Checkpoints

### Risk: one state object re-enters itself
Checkpoint: ensure `run()` only posts a single pending job and does not recursively run a state before the previous task finishes.

### Risk: wait deadlock
Checkpoint: `wait()` must never run inside the runtime async thread; it must block on a condition variable or equivalent external completion signal.

### Risk: queue corruption during failure/cancel
Checkpoint: all failed/cancelled states must terminate the loop cleanly and never re-post further runtime tasks.

## 10. Definition of Ready

Implementation can begin once:

- the manager singleton contract is approved
- the state object subclass behavior is approved
- the serialization rules are agreed
- `wait()` and failure semantics are documented

## 11. Definition of Done

The feature is done when:

- the runtime state manager is implemented as a singleton
- runtime state objects subclass `Dmn_State`
- `run()` schedules async execution through `Dmn_Runtime_Manager`
- all state objects are serialized through the runtime manager
- client `wait()` supports async completion tracking
- failure and cancellation paths are verified by tests
- the library remains backward compatible with existing runtime/state APIs
