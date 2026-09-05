# Runtime State Engine Implementation Plan

This document is a step-by-step TDD-first implementation plan for the runtime state engine feature. It maps spec items to phased implementation tasks. Follow the phases sequentially and run unit tests after each phase.

Repository layout assumptions
- include/: public headers
- src/: library implementation
- test/: unit tests
- CMake macros like ADD_TEST_EXECUTABLE are available and used for registering test executables.

Phase 0: Already completed (spec & header)
- `docs/specs/runtime-state-machine-spec.md` updated with ownership, run(onError), cancel, wait(timeout)/shared_future, priority/timed variants and tests.
- Public header `include/dmn-runtime-state.hpp` added declaring the API.

Phase 1: Add test skeletons and minimal stubs to compile
- Add test: `test/dmn-test-runtime-state.cpp` (skeleton)
- Add CMake: include `dmn-test-runtime-state` in `test/CMakeLists.txt`
- Add Phase-1 stub: `src/dmn-runtime-state.cpp` implementing minimal methods (compile-friendly):
  - Dmn_Runtime_State ctor/dtor
  - run() returns false
  - cancel() is a no-op
  - wait() returns immediately
  - getFuture() returns ready shared_future

Verify:
- cmake -B build -DCMAKE_BUILD_TYPE=Debug
- cmake --build build
- ctest --test-dir build --output-on-failure

Phase 2: Basic runtime enqueue & single-step execution
- Implement run() to atomically set queued flag and enqueue a Dmn_Runtime_Job to `Dmn_Runtime_Manager::addJob()` (immediate) or `addTimedJob()` (delay). Use Dmn_Runtime_Job::Priority.
- Engine will retain an internal shared_ptr to the state while queued; store it in `std::unordered_map<void*, std::shared_ptr<Dmn_Runtime_State>> m_pendingStates;` keyed by pointer or generated id.
- The job's m_fnc must create a coroutine task (TaskFncType) that:
  - locks a weak_ptr to the state
  - checks isCancelled(); if set, call setEnd() and finalize
  - calls runNext() once (in try/catch)
  - if still active, repost by calling addJob() again
  - if terminal, set completion promise and erase engine internal shared_ptr
- Wire `m_completionPromise` and `m_completionSharedFuture` so getFuture() returns `m_completionSharedFuture`.

Tests expected to pass after this phase:
- RuntimeState_BasicFlow
- RuntimeState_GetFuture_PreRun_MultipleWaiters (shared_future works)

Phase 3: Exception capture and onError forwarding
- Wrap runNext() call in try/catch inside the runtime job.
- On exception:
  - store `std::current_exception()` in the state
  - set `m_failed` flag
  - set `m_completionPromise` (set_exception or set_value, but capture ep for diagnostics)
  - invoke onError callback forwarded via job.m_onErrorFnc
- Update run() to forward client-provided onError into the runtime job creation

Tests expected to pass:
- RuntimeState_RunOnErrorCallback
- state_exception_marks_failed

Phase 4: Cancel semantics & destructor-while-queued
- Implement cancel() to set atomic m_cancelled.
- Ensure runtime job checks m_cancelled before runNext() and calls setEnd() if true.
- Ensure engine internal shared_ptr map is created when run() enqueues; it must hold the shared_ptr until terminal.
- Implement destructor_while_queued test to validate engine holds state alive.

Phase 5: Priority/timed behavior and fairness
- Implement run(priority, delay) mapping to addJob/addTimedJob. If delay > 0 use addTimedJob.
- Add tests verifying that priority ordering affects execution order.
- Consider fairness: ensure engine uses runtime priority queues and doesn't monopolize the runtime.

Phase 6: Runtime-thread detection & runtime safety
- Detect runtime context using `Dmn_Runtime_Manager::isRunInAsyncThread()`.
- In run() and wait(), if called on runtime thread: assert in debug builds and throw `std::runtime_error` in release builds.
- Implement safe unit/integration tests for detection (special harness that posts a runtime job which attempts to call wait() and expects an exception).

Phase 7: Polish, stress tests, documentation
- Add stress tests, runtime integration tests, and code comments.
- Document known limitations and example usage.

Developer checklist for each commit
- Keep commits small and focused.
- Run `cmake -B build -DCMAKE_BUILD_TYPE=Debug` and `cmake --build build` locally before pushing.
- Run `ctest --test-dir build --output-on-failure` after each phase and fix failing tests or update the Phase implementation accordingly.

Notes and gotchas
- Use weak_ptr in runtime job to avoid reference cycles; the engine's internal shared_ptr keeps the object alive while queued.
- Use atomic compare_exchange to set queued flag and avoid races for multiple-concurrent run() calls.
- Use std::shared_future to support multiple waiters.
- Be careful to release engine internal shared_ptr only after the completion promise is fulfilled and after finalization is complete.
- Use runtime's addJob/addTimedJob APIs and forward onError callback using Dmn_Runtime_Job::OnErrorFncType.

Example commands
- Configure & build: cmake -B build -DCMAKE_BUILD_TYPE=Debug
- Build: cmake --build build -j$(nproc)
- Run tests: ctest --test-dir build --output-on-failure

