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
  API so runtime-state can reject run()/wait()/wait_for() calls from the async
  thread.
- Update `include/dmn-runtime-state.hpp` to reflect these contracts before
  adding its implementation.
- The selected lifecycle contract is: a pre-run shared future remains pending;
  no configured state makes run() return false without terminalizing; cancel()
  before run() terminalizes as cancelled; failed futures rethrow the captured
  exception from get(); and runtime-thread run()/wait()/wait_for() calls throw
  in all build configurations.

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
  that inherited state configuration remains available for pre-submission
  setup.
- Add `Dmn_Runtime_State::setRuntimeStateFnc()` as a convenience wrapper that
  adapts `std::function<void(Dmn_Runtime_State &)>` into the underlying
  `Dmn_State` callback storage for runtime-aware logic such as cancellation
  checks.
- Keep the inherited `Dmn_State` declarations visible, but add dynamic guard
  hooks so external `setStateFnc()`, `setNext()`, and `setEnd()` throw
  `std::logic_error` after a successful `run()`, even through a
  `Dmn_State &` view.
- Reserve `runNext()` for manager-only execution by rejecting all external
  calls and routing manager-driven stepping through internal runtime-state
  helpers.
- Add `Dmn_State::hasStateFncs()` as a public query for whether the client
  configured at least one state function, excluding the internal
  initialization function.
- Do not retain created states in the manager yet. Retention begins only when
  a later `run()` implementation queues a state.

Phase 2: Terminal-state primitive and lifecycle unit tests (complete)
- Do not make the manager advance state transitions implicitly: it controls
  when `runNext()` executes, while a state function uses either its
  `Dmn_State &` parameter or its `Dmn_Runtime_State &` parameter to call
  `setNext()` or `setEnd()`, depending on which registration API was used.
- Require clients to finish configuring state functions before successful
  submission, because configuration is not synchronized with runtime execution.
- Implement the completion promise/shared_future pair, terminal flags, and a
  single idempotent terminal transition helper.
- Implement the selected no-state and cancel-before-run behavior.
- Add targeted tests for a pending pre-run future, rejected unconfigured
  run(), and cancel-before-run terminalization.

Verify:
- cmake -B build -DCMAKE_BUILD_TYPE=Debug
- cmake --build build
- ctest --test-dir build -R dmn-test-runtime-state --output-on-failure

Phase 3: Basic runtime enqueue & single-step execution (complete)
- Implement run() to set the mutex-protected queued flag and enqueue a
  Dmn_Runtime_Job to `Dmn_Runtime_Manager::addJob()` (immediate) or
  `addTimedJob()` (initial delay). Use Dmn_Runtime_Job::Priority.
- The manager retains an internal shared_ptr to the state while queued or
  running in `std::unordered_map<const Dmn_Runtime_State *,
  DmnRuntimeStatePtr> m_pendingStates`.
- The job's m_fnc creates a coroutine task (TaskFncType) that:
  - locks a weak_ptr to the state
  - checks isCancelled(); if set, call setEnd() and finalize
  - calls runNext() once (in try/catch)
  - if still active, repost immediately by calling addJob() again
  - if terminal, set completion promise and erase manager internal shared_ptr
- Wire `m_completionPromise` and `m_completionSharedFuture` so getFuture() returns `m_completionSharedFuture`.
- Document `setRuntimeStateFnc()` as the preferred callback registration API
  for runtime-aware logic such as `isCancelled()`, while keeping the
  inherited `setStateFnc()` path documented for base-API compatibility.

Tests expected to pass after this phase:
- RuntimeState_BasicFlow
- RuntimeState_GetFuture_PreRun_MultipleWaiters (shared_future works)

Phase 4: Exception capture and onError forwarding (complete)
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

Phase 5: Complete lifecycle and scheduling coverage (complete)
- Added focused named Google Test cases for singleton/state creation,
  external mutation rejection after submission, runtime-aware callback
  cancellation observation, unconfigured and pre-run cancellation behavior,
  normal execution, failure propagation, queued cancellation,
  manager-retained lifetime, priority ordering, delayed initial submission,
  and runtime-thread rejection.
- The queued-cancellation test verifies no user-defined step executes after
  cancellation and that the inherited `Dmn_State` is finalized.
- The retained-lifetime test verifies a client can release its handle after
  submission and that the manager releases its final ownership after terminal
  completion.
- The priority and delay tests verify `run(priority, delay, onError)` maps
  correctly to runtime scheduling behavior.
- The runtime-thread test verifies `run()`, `wait()`, and `wait_for()` throw
  `std::runtime_error` from the runtime async thread.

Phase 6: Drain-and-cancel manager shutdown (complete)
- Added `Dmn_Runtime_State_Manager::shutdown()`, which permanently rejects new
  state submissions while allowing callers to create non-runnable handles.
- Shutdown snapshots the manager-retained handles, requests cooperative
  cancellation outside the manager mutex, and waits for all captured states to
  reach terminal cancellation before returning.
- A state step already executing may finish its callback, but its terminal
  outcome is cancellation when shutdown requested it. Queued states finalize
  without running another user-defined callback.
- Shutdown is idempotent and rejects calls from the runtime async thread to
  avoid deadlock.
- Added coverage for drain waiting, running and queued state cancellation, and
  rejection of a post-shutdown submission.

Phase 7: Integration, stress, and documentation (complete)
- Added multi-state integration coverage for serialized execution and runtime
  async-thread affinity, plus failure-isolation coverage for independent
  queued states.
- Added concurrent client coverage for create/run/cancel/getFuture/wait
  operations across 24 states, and shutdown stress coverage for 32 queued
  states behind a running callback.
- Added a public usage example that documents runtime initialization from the
  main thread, explicit state-manager shutdown while the runtime loop is
  active, and runtime shutdown only after state draining completes.
- Added API-boundary coverage that verifies external mutation is rejected
  after submission and that external `runNext()` cannot bypass the
  runtime-managed execution path.
- Added runtime-aware callback coverage that verifies a running step can
  observe `isCancelled()` directly from its `Dmn_Runtime_State &` parameter.

Developer checklist for each commit
- Keep commits small and focused.
- Run `cmake -B build -DCMAKE_BUILD_TYPE=Debug` and `cmake --build build` locally before pushing.
- Run `ctest --test-dir build --output-on-failure` after each phase and fix failing tests or update the Phase implementation accordingly.

Notes and gotchas
- Use weak_ptr in runtime job to avoid reference cycles; the manager's internal shared_ptr keeps the object alive while queued.
- Use the state mutex to set the queued flag and avoid races for multiple
  concurrent run() calls.
- Use std::shared_future to support multiple waiters.
- Be careful to release manager internal shared_ptr only after the completion promise is fulfilled and after finalization is complete.
- Use runtime's addJob/addTimedJob APIs and forward onError callback using Dmn_Runtime_Job::OnErrorFncType.
- The manager exposes one drain-and-cancel shutdown mode and no
  concurrency-configuration API; state steps execute in the process-wide
  runtime async context.

Example commands
- Configure & build: cmake -B build -DCMAKE_BUILD_TYPE=Debug
- Build: cmake --build build -j$(nproc)
- Run tests: ctest --test-dir build --output-on-failure
