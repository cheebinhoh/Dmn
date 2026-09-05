# Runtime State Manager Implementation Plan

This document is a step-by-step TDD-first implementation plan for the runtime state manager feature. It maps spec items to phased implementation tasks. Follow the phases sequentially and run unit tests after each phase.

Repository layout assumptions
- include/: public headers
- src/: library implementation
- test/: unit tests
- CMake macros like ADD_TEST_EXECUTABLE are available and used for registering test executables.

Phase 0: API contract and header preparation
- `docs/specs/runtime-state-machine-spec.md` defines ownership, run(onError),
  cancel, wait(timeout)/shared_future, priority/timed variants, and tests.
- `Dmn_Runtime_State_Manager` uses the inherited
  `Dmn_Singleton<Dmn_Runtime_State_Manager>::createInstance()` factory and
  therefore returns `std::shared_ptr<Dmn_Runtime_State_Manager>`. It must not
  declare a conflicting reference-returning factory.
- `Dmn_Runtime_Manager::isRunInAsyncThread()` is part of the public runtime
  API so runtime-state can reject run()/wait() calls from the async thread.
- Update `include/dmn-runtime-state.hpp` to reflect these contracts before
  adding its implementation.
- The selected lifecycle contract is: a pre-run shared future remains pending;
  no configured state makes run() return false without terminalizing; cancel()
  before run() terminalizes as cancelled; failed futures rethrow the captured
  exception from get(); and runtime-thread run()/wait() calls throw in all
  build configurations.

Phase 1: Construct the singleton manager (complete)
- Correct `include/dmn-runtime-state.hpp` so
  `Dmn_Runtime_State_Manager` has a protected constructor, public destructor,
  and friends `Dmn_Singleton<Dmn_Runtime_State_Manager>`. Do not declare a
  conflicting `createInstance()` method; use the inherited shared-pointer
  factory. The public destructor is required by the singleton's default
  `std::shared_ptr` deleter.
- Add `src/dmn-runtime-state.cpp` containing the manager constructor and
  destructor definitions.
- Add `test/dmn-test-runtime-state.cpp`, with a focused unit test that
  calls `Dmn_Runtime_State_Manager::createInstance()`, verifies the returned
  shared pointer is non-null, and verifies repeated calls return the same
  manager address.
- Add `dmn-test-runtime-state` to `test/CMakeLists.txt`.
- Add `src/dmn-runtime-state.cpp` and `include/dmn-runtime-state.hpp` to the
  `dmn` target in
  `src/CMakeLists.txt`. Add the header to `include/dmn.hpp`.

Verify:
- cmake -B build -DCMAKE_BUILD_TYPE=Debug
- cmake --build build
- ctest --test-dir build -R dmn-test-runtime-state --output-on-failure

Completed follow-on increment: State-handle creation
- Add the `DmnRuntimeStatePtr` alias for
  `std::shared_ptr<Dmn_Runtime_State>`.
- Implement `Dmn_Runtime_State_Manager::createState(std::string_view)` to
  construct and return a new `Dmn_Runtime_State`.
- Define the runtime-state constructor, destructor, and default no-op
  lifecycle hooks required to link the concrete polymorphic type.
- Extend `dmn-test-runtime-state` to verify `createState()` returns a
  non-null handle, that `Dmn_Runtime_State` derives from `Dmn_State`, and
  that inherited state configuration and stepping remain accessible for
  compatibility testing.
- Remove the shadowing `Dmn_Runtime_State` declarations of `setStateFnc()`,
  `setNext()`, and `setEnd()`. `runNext()` is re-exposed as protected and is
  available to `Dmn_Runtime_State_Manager` through friendship.
- Add `Dmn_State::hasStateFncs()` as a public query for whether the client
  configured at least one state function, excluding the internal
  initialization function.
- Do not retain created states in the manager yet. Retention begins only when
  a later `run()` implementation queues a state.

Phase 2: Terminal-state primitive and lifecycle unit tests (next)
- Do not make the manager advance state transitions implicitly: it controls
  when `runNext()` executes, while a state function uses its `Dmn_State &`
  parameter to call `setNext()` or `setEnd()`.
- Implement the completion promise/shared_future pair, terminal flags, and a
  single idempotent terminal transition helper.
- Implement the selected no-state and cancel-before-run behavior.
- Add targeted tests for a pending pre-run future, rejected unconfigured
  run(), and cancel-before-run terminalization.

Verify:
- cmake -B build -DCMAKE_BUILD_TYPE=Debug
- cmake --build build
- ctest --test-dir build -R dmn-test-runtime-state --output-on-failure

Phase 3: Basic runtime enqueue & single-step execution
- Implement run() to atomically set queued flag and enqueue a Dmn_Runtime_Job to `Dmn_Runtime_Manager::addJob()` (immediate) or `addTimedJob()` (delay). Use Dmn_Runtime_Job::Priority.
- Manager will retain an internal shared_ptr to the state while queued; store it in `std::unordered_map<void*, std::shared_ptr<Dmn_Runtime_State>> m_pendingStates;` keyed by pointer or generated id.
- The job's m_fnc must create a coroutine task (TaskFncType) that:
  - locks a weak_ptr to the state
  - checks isCancelled(); if set, call setEnd() and finalize
  - calls runNext() once (in try/catch)
  - if still active, repost by calling addJob() again
  - if terminal, set completion promise and erase manager internal shared_ptr
- Wire `m_completionPromise` and `m_completionSharedFuture` so getFuture() returns `m_completionSharedFuture`.

Tests expected to pass after this phase:
- RuntimeState_BasicFlow
- RuntimeState_GetFuture_PreRun_MultipleWaiters (shared_future works)

Phase 4: Exception capture and onError forwarding
- Wrap runNext() call in try/catch inside the runtime job.
- On exception:
  - store `std::current_exception()` in the state
  - set `m_failed` flag
  - set `m_completionPromise` with the captured exception so
    `getFuture().get()` rethrows it
  - invoke onError callback forwarded via job.m_onErrorFnc
- Update run() to forward client-provided onError into the runtime job creation

Tests expected to pass:
- RuntimeState_RunOnErrorCallback
- state_exception_marks_failed

Phase 5: Cancel semantics & destructor-while-queued
- Implement cancel() to set atomic m_cancelled.
- Ensure runtime job checks m_cancelled before runNext() and calls setEnd() if true.
- Ensure manager internal shared_ptr map is created when run() enqueues; it must hold the shared_ptr until terminal.
- Implement destructor_while_queued test to validate manager holds state alive.

Phase 6: Priority/timed behavior and fairness
- Implement run(priority, delay) mapping to addJob/addTimedJob. If delay > 0 use addTimedJob.
- Add tests verifying that priority ordering affects execution order.
- Consider fairness: ensure manager uses runtime priority queues and doesn't monopolize the runtime.

Phase 7: Runtime-thread detection & runtime safety
- Detect runtime context using the public
  `Dmn_Runtime_Manager::isRunInAsyncThread()`.
- In run() and wait(), if called on runtime thread, throw
  `std::runtime_error` in every build configuration.
- Implement safe unit/integration tests for detection (a harness that posts a
  runtime job which attempts to call wait() and expects an exception).

Phase 8: Shutdown, polish, stress tests, documentation
- Define and implement the selected graceful and immediate manager shutdown API
  before adding shutdown tests.
- Add stress tests, runtime integration tests, and code comments.
- Document known limitations and example usage.

Developer checklist for each commit
- Keep commits small and focused.
- Run `cmake -B build -DCMAKE_BUILD_TYPE=Debug` and `cmake --build build` locally before pushing.
- Run `ctest --test-dir build --output-on-failure` after each phase and fix failing tests or update the Phase implementation accordingly.

Notes and gotchas
- Use weak_ptr in runtime job to avoid reference cycles; the manager's internal shared_ptr keeps the object alive while queued.
- Use atomic compare_exchange to set queued flag and avoid races for multiple-concurrent run() calls.
- Use std::shared_future to support multiple waiters.
- Be careful to release manager internal shared_ptr only after the completion promise is fulfilled and after finalization is complete.
- Use runtime's addJob/addTimedJob APIs and forward onError callback using Dmn_Runtime_Job::OnErrorFncType.

Example commands
- Configure & build: cmake -B build -DCMAKE_BUILD_TYPE=Debug
- Build: cmake --build build -j$(nproc)
- Run tests: ctest --test-dir build --output-on-failure
