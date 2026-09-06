# Implementation Plan: Runtime State Manager

## 1. Goal

Implement the runtime state manager feature as a singleton runtime-owned state machine manager built on the existing `dmn-runtime` and `dmn-state` components.

## 2. Core Design Decisions

### Decision 1: manager is singleton and runtime-owned
The manager follows the same singleton model as `Dmn_Runtime_Manager`. It owns the execution policy for state objects and routes state execution through the runtime scheduler.

### Decision 2: state objects subclass `Dmn_State`
Each runtime state object remains a state machine, but adds async lifecycle metadata and `wait()` support. This keeps the base state semantics while adding runtime execution ownership.

`Dmn_State::runNext()` exposes only user-provided states: it performs
initialization before the first user callback and finalization after a terminal
callback in the same invocation. An empty `Dmn_State` converts to false, while
an explicit `runNext()` initializes and finalizes it before returning false.

### Decision 3: all state execution is serialized
The manager does not allow independent parallel execution of user-state
callbacks across submitted runtime state objects. It posts work to the
process-wide runtime scheduler, which executes them serially.

### Decision 4: `run()` is async-only
The client never directly executes state logic in its own thread. `run()` only
queues runtime tasks. Each task invokes `runNext()`, executes at most one
user-state callback, folds in pending initialization or finalization, and
reposts when more user work remains.

## 3. Phase 1: Construct the manager singleton (complete)

### Tasks

- define `Dmn_Runtime_State_Manager` singleton API
- use the inherited `Dmn_Singleton` shared-pointer `createInstance()` factory
- grant `Dmn_Singleton<Dmn_Runtime_State_Manager>` access to the protected
  manager constructor through a friend declaration
- add a focused unit test that constructs the singleton, verifies the returned
  shared pointer is non-null, and verifies repeated calls return the same
  manager instance
- this phase deliberately deferred lifecycle flags, waiting, scheduling, and
  ownership retention to later phases

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
  `Dmn_State` inheritance and pre-submission configuration compatibility
- add a runtime-aware callback registration helper so state functors can take
  `Dmn_Runtime_State &` directly when they need runtime-only APIs such as
  cancellation inspection
- keep the inherited `Dmn_State` configuration API visible, but dynamically
  reject external `setStateFnc()`, `setNext()`, and `setEnd()` calls after a
  successful `run()`
- authorize in-callback transitions by runtime thread identity, preventing a
  client thread from mutating transitions while a callback is active
- reject external `runNext()` through any `Dmn_Runtime_State` or `Dmn_State`
  view, including from inside a user callback, so only the manager may advance
  the machine
- add public `Dmn_State::hasStateFncs()` to identify whether the client
  configured at least one state function
- retain a manager-owned state handle after successful runtime queueing and
  release it after terminal completion

## 4. Phase 2: Implement the completion model (complete)

### Tasks

- keep transition selection in state functions through either their
  `Dmn_State &` or `Dmn_Runtime_State &` parameter; the runtime manager
  controls when `runNext()` executes and reposts work, not which transition
  is selected
- add runtime lifecycle flags (`queued`, `running`, `completed`, `failed`,
  `cancelled`)
- add promise/shared-future completion synchronization for `wait()` and
  `wait_for()`
- implement pre-run rejection and cancel-before-run terminal semantics

### Deliverables

- lifecycle state model
- completion waiting design

## 5. Phase 3: Integrate with runtime scheduler (complete)

### Tasks

- schedule user-state callbacks as runtime jobs using
  `Dmn_Runtime_Manager::addJob()`
- ensure `run()` posts work to runtime rather than executing directly
- serialize state object execution through the runtime queue
- implement a continuation loop so each non-terminal user step schedules the
  next one

### Deliverables

- runtime job adapter for state objects
- serialized manager dispatcher
- sequential execution loop

## 6. Phase 4: State lifecycle and completion (complete)

### Tasks

- implement terminal completion, failure, and cancellation behavior
- notify waiters exactly once through the stored shared future
- avoid re-enqueuing after terminal state
- defer shutdown-mode API and tests to the shutdown phase

### Deliverables

- terminal-state semantics
- exception-safe cleanup
- completion synchronization contract

## 7. Phase 5: Complete lifecycle and scheduling test coverage (complete)

### Tasks

- Added named Google Test cases for external mutation rejection after
  submission, runtime-aware callback cancellation observation, cancellation
  while queued, manager-retained lifetime after the client drops its handle,
  priority ordering, timed initial submission, and runtime-thread rejection.
- The runtime-thread harness verifies `run()`, `wait()`, and `wait_for()`
  throw `std::runtime_error` from the runtime async thread.
- The test suite verifies runtime-managed states retain `Dmn_State`
  configuration and transition semantics.

### Deliverables

- independently reported lifecycle and scheduling tests
- regression coverage for state lifetime and runtime-thread safety

## 8. Phase 6: Drain-and-cancel manager shutdown (complete)

### Tasks

- Added `Dmn_Runtime_State_Manager::shutdown()`.
- Reject new runs after shutdown begins while preserving the existing
  non-null `createState()` factory behavior.
- Cancel retained states cooperatively and wait for terminal cancellation
  outside the manager mutex.
- Reject shutdown from the runtime async thread and release retained manager
  handles as states become terminal.

### Required tests

- shutdown waits for a running state callback to complete
- queued states are cancelled without running their user callback
- post-shutdown submission is rejected
- completion-future notification and manager-retention cleanup

## 9. Phase 7: Integration and documentation (complete)

### Tasks

- Added multi-state serialization and failure-isolation integration tests.
- Added concurrent lifecycle coverage for state creation, submission,
  cancellation, future retrieval, and waiting.
- Added shutdown stress coverage for a running state and 32 queued states.
- Added user documentation for runtime initialization, configuration,
  submission, completion waiting, state-manager shutdown, and runtime exit.
- Added API-boundary enforcement so runtime submission freezes external
  configuration/mutation and reserves `runNext()` for manager-driven
  execution only.
- Added runtime-aware callback support so cancellation-aware user-state
  callbacks can use `Dmn_Runtime_State &` directly when needed.

## 10. Risks and Checkpoints

### Risk: one state object re-enters itself
Checkpoint: ensure `run()` only posts a single pending job and does not recursively run a state before the previous task finishes.

### Risk: wait deadlock
Checkpoint: `wait()` must never run inside the runtime async thread; it must block on a condition variable or equivalent external completion signal.

### Risk: queue corruption during failure/cancel
Checkpoint: all failed/cancelled states must terminate the loop cleanly and never re-post further runtime tasks.

## 11. Definition of Ready

Implementation can begin once:

- the manager singleton contract is approved
- the state object subclass behavior is approved
- the serialization rules are agreed
- `wait()` and failure semantics are documented

## 12. Definition of Done

The feature is done when:

- the runtime state manager is implemented as a singleton
- runtime state objects subclass `Dmn_State`
- `run()` schedules async execution through `Dmn_Runtime_Manager`
- callbacks from submitted state objects are serialized through the runtime
  manager
- client `wait()` supports async completion tracking
- failure and cancellation paths are verified by tests
- the library remains backward compatible with existing runtime/state APIs
