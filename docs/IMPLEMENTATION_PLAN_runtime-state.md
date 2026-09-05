# Runtime State Engine Implementation Plan

Status: Canonical plan
Specification: [`specs/runtime-state-machine-spec.md`](specs/runtime-state-machine-spec.md)

Implement the phases in order. The API contract in the specification takes
precedence over this plan.

## Phase 1: Wire the public surface

1. Add `src/dmn-runtime-state.cpp` to `src/CMakeLists.txt`.
2. Add `include/dmn-runtime-state.hpp` to the `dmn` interface headers and
   include it from `include/dmn.hpp`.
3. Add `dmn-test-runtime-state` to `test/CMakeLists.txt`.
4. Implement defaulted/destructed PIMPL owners and the engine singleton
   constructor. Do not add compile-only behavior stubs.
5. Use the public, const `Dmn_Runtime_Manager::isRunInAsyncThread()` for
   runtime-thread checks.

## Phase 2: Implement synchronization and configuration

1. Define `Dmn_Runtime_State::Impl` with a mutex, `Status`, cancellation
   request, configured callback count, transition guard, failure exception,
   promise, and shared future.
2. Define `Dmn_Runtime_State_Engine::Impl` with a mutex, irreversible
   shutdown mode, monotonically increasing ID, and
   `unordered_map<uint64_t, shared_ptr<Dmn_Runtime_State>>` pending map.
3. Make `createState()` the only construction path. Since the state constructor
   is private, use `shared_ptr<Dmn_Runtime_State>{new Dmn_Runtime_State{name}}`
   rather than `make_shared`, and bind the new state to its creating engine
   without creating an ownership cycle.
4. Adapt each `FncType(Dmn_Runtime_State&)` into the base
   `Dmn_State::FncType(Dmn_State&)`, validate one transition per callback, and
   reject `setStateFnc()` after submission.
5. Restrict `setNext()` and `setEnd()` to the current runtime callback.
6. Implement one idempotent terminal-transition helper. It must store the
   status and exception, fulfill the promise, then arrange release of engine
   ownership without holding either mutex during external calls.

## Phase 3: Submit and continue jobs

1. Reject runtime-thread `run()` calls with `std::runtime_error`.
2. Atomically accept only an idle, non-empty, non-shutdown state. Empty
   configuration becomes failed with `logic_error`; `kSched` priority and a
   negative delay become failed with `invalid_argument`; duplicate, cancelled,
   or shutdown submissions return `false`.
3. Retain `shared_from_this()` in the engine pending map before submitting the
   initial runtime job. If submission throws, remove ownership and fail the
   handle.
4. Submit the initial job with `addJob()` or `addTimedJob()` as specified.
   Store the supplied priority and onError for each continuation.
5. In a job, process at most one `runNext()`. Check cancellation before a
   user step and after a running step. Cancellation finalizes the base state
   without running another user callback.
6. On a callback exception, terminally fail the state before rethrowing the
   same exception. Pass `onError` only as the runtime job error callback; do
   not call it in state-engine code.
7. Repost only an active, non-cancelled state at its original priority.

## Phase 4: Completion and shutdown

1. Implement `wait()`, `wait_for()`, `getFuture()`, status queries, and
   `failure()` from the shared-future contract.
2. Implement graceful shutdown: reject future runs and retain accepted work
   until it reaches a terminal state.
3. Implement immediate shutdown: terminally cancel queued entries, remove
   their ownership after fulfillment, and request cancellation of a running
   entry. Never block waiting for a callback.

## Phase 5: Tests

Add the required tests listed in the specification. Tests that require runtime
execution must start and stop `Dmn_Runtime_Manager` according to the existing
runtime-test pattern. Include a callback that deliberately omits a transition,
and verify that it fails instead of entering an infinite repost loop.

## Validation

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build -R dmn-test-runtime-state -VV --output-on-failure
ctest --test-dir build --output-on-failure
```
