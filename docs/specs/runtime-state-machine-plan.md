# Implementation Plan: Runtime State Engine

## 1. Goal

Implement the runtime state engine feature as a singleton runtime-owned state machine manager built on the existing `dmn-runtime` and `dmn-state` components.

## 2. Core Design Decisions

### Decision 1: engine is singleton and runtime-owned
The engine follows the same singleton model as `Dmn_Runtime_Manager`. It owns the execution policy for state objects and routes state execution through the runtime scheduler.

### Decision 2: state objects subclass `Dmn_State`
Each runtime state object remains a state machine, but adds async lifecycle metadata and `wait()` support. This keeps the base state semantics while adding runtime execution ownership.

### Decision 3: all state execution is serialized
The engine does not allow independent parallel execution of state steps across runtime state objects. It posts work to the runtime scheduler in serialized form.

### Decision 4: `run()` is async-only
The client never directly executes state logic in its own thread. `run()` only queues runtime tasks. The runtime executes each state step and then re-posts the next task until the machine is complete.

## 3. Phase 1: Define the engine and object model

### Tasks

- define `Dmn_Runtime_State_Engine` singleton API
- define `Dmn_Runtime_State` subclass of `Dmn_State`
- add runtime lifecycle flags (`queued`, `running`, `completed`, `failed`, `cancelled`)
- add `wait()` synchronization primitive
- confirm object ownership and destroy semantics

### Deliverables

- public engine class declaration
- public state class declaration
- lifecycle state model
- wait mechanism design

## 4. Phase 2: Integrate with runtime scheduler

### Tasks

- schedule state steps as runtime jobs using `Dmn_Runtime_Manager::addJob()`
- ensure `run()` posts work to runtime rather than executing directly
- serialize state object execution through the runtime queue
- implement continuation loop so each step schedules the next one until terminal state

### Deliverables

- runtime job adapter for state objects
- serialized engine dispatcher
- sequential execution loop

## 5. Phase 3: State lifecycle and completion

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

## 6. Phase 4: API ergonomics and compatibility

### Tasks

- keep `Dmn_State` unchanged for synchronous scenarios
- provide a runtime-specific object for async execution
- preserve `setStateFnc()`, `setNext()`, `setEnd()`, and `runNext()` semantics
- document `wait()` semantics and restrictions

### Deliverables

- developer-facing API contract
- usage examples for initialization, run, and wait
- compatibility note for existing runtime and state users

## 7. Phase 5: Validation

### Tests to add

- state object created from engine
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

## 8. Risks and Checkpoints

### Risk: one state object re-enters itself
Checkpoint: ensure `run()` only posts a single pending job and does not recursively run a state before the previous task finishes.

### Risk: wait deadlock
Checkpoint: `wait()` must never run inside the runtime async thread; it must block on a condition variable or equivalent external completion signal.

### Risk: queue corruption during failure/cancel
Checkpoint: all failed/cancelled states must terminate the loop cleanly and never re-post further runtime tasks.

## 9. Definition of Ready

Implementation can begin once:

- the engine singleton contract is approved
- the state object subclass behavior is approved
- the serialization rules are agreed
- `wait()` and failure semantics are documented

## 10. Definition of Done

The feature is done when:

- the runtime state engine is implemented as a singleton
- runtime state objects subclass `Dmn_State`
- `run()` schedules async execution through `Dmn_Runtime_Manager`
- all state objects are serialized through the runtime engine
- client `wait()` supports async completion tracking
- failure and cancellation paths are verified by tests
- the library remains backward compatible with existing runtime/state APIs
