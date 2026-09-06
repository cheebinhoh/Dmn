# Feature Spec: Runtime State Manager

Status: Implemented.

## Implementation Status

The current implementation provides the singleton manager, managed state
handles, completion futures, runtime scheduling, manager-held lifetime,
one-shot submission, cooperative cancellation, failure capture, runtime
error callback forwarding, post-submission external-mutation rejection, and
the runtime-aware `setRuntimeStateFnc()` callback API. `run()` supports
priority and an initial delay; later user-state callbacks are reposted
immediately at the submitted priority.

`run()`, `wait()`, and `wait_for()` reject calls from the runtime async thread.
Focused tests cover external-mutation rejection after submission,
runtime-aware callback cancellation observation, queued cancellation,
manager-retained lifetime, priority ordering, timed initial submission,
runtime-thread rejection, drain-and-cancel shutdown, multi-state
serialization, failure isolation, and concurrent lifecycle operations. The
manager exposes one shutdown mode and no configurable concurrency. The
current runtime architecture serializes all user-state callbacks through the
process-wide runtime async thread.

## 1. Summary

This feature introduces a new runtime-owned state execution manager that
combines the existing `dmn-runtime` scheduler with the `dmn-state` finite-state
helper. The result is a singleton runtime service that creates state objects
for clients, serializes their execution through the runtime, and lets callers
wait for completion without forcing the client thread to execute each
user-state callback directly.

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
5. the runtime manager continues to repost tasks while user callbacks remain;
   errors occurring in async execution invoke a client-provided onError
   callback (if supplied) and are captured on the state handle;
6. callbacks from submitted state objects execute serially through
   `Dmn_Runtime_Manager`;
7. client may call `statehandle->wait()` or use the returned shared_future to block or asynchronously observe completion.

## 2. Design Objective

The existing `dmn-state` component is synchronous and client-driven. It calls
`runNext()` directly in the caller thread; each call executes at most one user
callback and folds in pending initialization or finalization. That is useful
for local control flow, but not for runtime-managed workflows. The new runtime
state manager changes the ownership model:

- the state object remains a state machine definition and execution state,
- the runtime manager owns when user-state callbacks are executed,
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
- retain submitted state handles while routing their work through the
  process-wide runtime scheduler
- ensure all state execution is scheduled through `Dmn_Runtime_Manager`

The manager must not declare a same-named `createInstance()` with a different
return type. A forwarding convenience function is permitted only when it has a
distinct name and preserves the singleton's shared ownership semantics.

The implemented manager and state handle live in
`include/dmn-runtime-state.hpp` and `src/dmn-runtime-state.cpp`; the
`dmn-test-runtime-state` target exercises creation, scheduling, cancellation,
mutation guards, runtime-aware callback behavior, shutdown, and concurrent
integration coverage.

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
- be configured by calling `setStateFnc()`, `setRuntimeStateFnc()`, or an
  equivalent state registration method

### FR-3: State configuration

A client must be able to define one or more states on the returned runtime state object via the compatible `Dmn_State` interface.

`Dmn_Runtime_State` MUST inherit the `Dmn_State` configuration surface so
state functions retain the base callback signature,
`std::function<void(Dmn_State &)>`.

`Dmn_Runtime_State` SHOULD also provide a runtime-aware convenience callback
API that accepts `std::function<void(Dmn_Runtime_State &)>` and adapts it into
the underlying `Dmn_State` callback storage. This is the preferred API when a
callback needs runtime-only methods such as `isCancelled()`.

Clients configure state functions before calling `run()`. They do not directly
advance or terminate the machine from outside a state function: the runtime
manager controls when `runNext()` executes and whether another job is posted.
A state function uses either its `Dmn_State &` parameter or its
`Dmn_Runtime_State &` parameter, depending on which registration API the
client chose, to call `setNext()` or `setEnd()` when it needs to select the
next transition or terminate the machine.

`Dmn_State::hasStateFncs()` is a public query that returns true when at least
one client-defined callback exists. It excludes the reserved internal slot and
is used by `Dmn_Runtime_State::run()` to reject an unconfigured state without
terminalizing it.

For synchronous `Dmn_State`, internal lifecycle work is not exposed as
separate steps. `runNext()` performs initialization before the first
user-provided callback and performs finalization after a callback selects a
terminal transition, all in the same invocation. It executes at most one user
callback per call. An empty state converts to false; an explicit `runNext()`
still initializes and finalizes that empty state before returning false.
State index 0 is reserved for internal initialization and is not a valid
`setNext()` target.

Configuration must be complete before a successful `run()` call. After a
successful submission, external calls to inherited `setStateFnc()`,
`setNext()`, and `setEnd()` MUST throw `std::logic_error`. This freeze applies
through any `Dmn_Runtime_State` or `Dmn_State` view of the object. The
currently executing runtime-managed callback remains allowed to call
`setNext()` and `setEnd()` to choose transitions. Authorization is tied to the
runtime execution thread, so a client thread cannot mutate transitions while a
callback is active.

External `runNext()` is never part of the runtime-managed contract. Calling
`runNext()` on a `Dmn_Runtime_State` through any `Dmn_State` view MUST throw
`std::logic_error`; only the manager may drive state advancement. This
restriction also applies inside a user callback, which must use `setNext()` or
`setEnd()` and return control to the manager instead of recursively advancing
the machine.

### FR-4: `run()` dispatches work to runtime and error callback forwarding

The runtime state handle must expose a public `run()` method. In addition, `run()` MUST accept optional parameters for priority, delay (timed variant), and an onError callback that matches the runtime's `Dmn_Runtime_Job::OnErrorFncType` signature.

Behavior:

- `run()` may be called from any client thread EXCEPT the runtime async thread (calls from the runtime thread are disallowed — see `run()` thread policy below).
- `run()` must schedule asynchronous work on the singleton `Dmn_Runtime_Manager` async thread by calling `addJob()` or `addTimedJob()` as appropriate.
- `run()` returns `true` if the state was successfully queued and `false` when
  the state is unconfigured, cancelled, terminal, already active, or the
  manager has shut down. Runtime submission exceptions propagate after the
  state clears its queued marker, allowing a later submission attempt.
- `run()` is a one-shot operation for each state handle: subsequent `run()` calls after the first successful enqueue MUST be no-ops and MUST return `false`.
- The optional `onError` callback provided to `run()` MUST be forwarded to the underlying `dmn-runtime` job so that asynchronous runtime failures invoke the client callback when the runtime job reports an error. Use `Dmn_Runtime_Job::OnErrorFncType` as the canonical type.
- Each runtime dispatch executes at most one user-provided callback.
  Initialization runs before the first callback, and finalization runs after a
  terminal callback in the same dispatch. Cancellation or an end selected
  before submission may cause a dispatch to execute no user callback. After a
  non-terminal callback executes, the manager reposts another job.
- `run()` MUST NOT execute state logic synchronously in the caller thread.

Priority and timed variants:

- `run()` must accept an optional `Dmn_Runtime_Job::Priority` parameter and an
  optional delay so callers can control priority and schedule a timed initial
  run. The manager maps these directly to `Dmn_Runtime_Manager::addJob()`
  (immediate) or `addTimedJob()` (timed).

Thread policy for `run()`, `wait()`, and `wait_for()`:

- If `run()`, `wait()`, or `wait_for()` is called from inside the runtime async
  thread (detected via `Dmn_Runtime_Manager::isRunInAsyncThread()`), the
  implementation MUST throw `std::runtime_error`. This prevents deadlocks and
  enforces the rule that runtime-internal callbacks should not block the
  runtime.
- This policy applies in every build configuration, permitting direct,
  deterministic unit testing without a debug-only death test.

### FR-5: Serialized execution across submitted state objects

All state objects created from the runtime state manager must execute in serialized form through the shared runtime scheduler by default.

Requirements:

- no two state objects may run user-state callbacks concurrently in the same runtime manager (default global serialization)
- state object execution order must follow runtime job ordering/priority semantics
- user-state callbacks must be single-threaded relative to manager execution
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
- cancelled state

A runtime state object is terminal when it has either:

- completed a `runNext()` call that observed an end selected by `setEnd()` or
  another terminal transition
- failed due to an uncaught exception during a user-state callback
- been cancelled

The inherited `Dmn_State::isInitialized()`, `isFinalized()`, and boolean
conversion describe the base state-machine lifecycle. They are not substitutes
for runtime terminal-status queries: failure or cancellation before a runtime
dispatch can publish a terminal runtime outcome without running base
finalization. Clients must use `isCompleted()`, `isFailed()`, `isCancelled()`,
or `getFuture()` for runtime completion.

Lifecycle hooks have these execution and ordering rules:

- `onStarted()` normally runs in the runtime thread before `runNext()` and
  before the first user callback. If it throws, the state fails.
- `onCompleted()` and `onFailed()` run in the runtime thread after the terminal
  result has been published.
- `onCancelled()` runs synchronously in the caller's thread for
  pre-submission cancellation and normally in the runtime thread for submitted
  work.
- A terminal future may wake a waiter before its terminal hook returns.
- Terminal hook overrides must not throw because an exception can interrupt
  manager cleanup without changing the already-published outcome.

### FR-8: Cancellation and shutdown

The runtime state object MUST provide a `cancel()` method that is cooperative in nature.

Semantics:

- `cancel()` is idempotent and sets the object's cancelled flag and prevents further non-cooperative steps from being scheduled
- `cancel()` does NOT asynchronously preempt a currently executing state
  functor. A callback that requires cooperative early exit should prefer the
  runtime-aware callback API so it can inspect `isCancelled()` directly on its
  `Dmn_Runtime_State &` parameter. Callers that use the inherited
  `Dmn_State &` callback form may still capture their runtime-state handle and
  query `isCancelled()` that way.
- when the runtime task executes and detects the cancel flag is set, it MUST call `setEnd()` before invoking `runNext()` so that the state finalizes deterministically rather than executing further steps
- `isCancelled()` provides the cancellation query for callbacks that capture
  their runtime-state handle and need to early-exit or perform cleanup
- calling `cancel()` before `run()` transitions the object to cancelled
  terminal state immediately, completes its shared future, and causes any later
  `run()` call to return false. Its `onCancelled()` hook runs synchronously in
  the thread that called `cancel()`.
- `Dmn_Runtime_State_Manager::shutdown()` provides the current
  drain-and-cancel shutdown mode. It rejects new submissions, requests
  cancellation for all submitted states, and waits for their terminal futures.
  A currently executing callback may finish, but publishes cancellation when
  shutdown requested it; queued states finalize without another user callback.
  Calling shutdown from the runtime async thread throws `std::runtime_error`.

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

The runtime state manager must preserve the runtime's execution model.
Runtime-specific lifecycle operations and status queries are synchronized and
may be called from client threads, subject to the runtime-thread restrictions
on `run()`, `wait()`, `wait_for()`, and `shutdown()`. Inherited `Dmn_State`
configuration is single-threaded and must finish before submission. User-state
callbacks execute on the runtime thread.

### NFR-2: Compatibility

The runtime manager API remains additive and does not change
`Dmn_Runtime_Manager`. `Dmn_State` intentionally changes execution semantics
so initialization and finalization are no longer user-visible steps, empty
states convert to false, and index 0 is reserved for internal initialization.

### NFR-3: Determinism

State execution within one runtime manager must be deterministic with respect to queue ordering and posting order when preservation of ordering is requested by the client.

### NFR-4: Controlled memory lifetime (explicit)

The manager MUST hold a `std::shared_ptr` to any queued/running state object until terminal state is reached. This mirrors existing dmn ownership patterns (see `dmn-dmesg` for a similar handle + manager-owned shared_ptr pattern).

### NFR-5: Thread safety for wait semantics

`wait()` must be implemented using synchronization primitives appropriate for cross-thread signaling (`std::condition_variable`, flag, or equivalent) and must not busy-spin. Use of a `std::shared_future<void>` simplifies multi-waiter semantics.

## 7. Current API Shape

The following summarizes the currently implemented public API. It reflects the
library naming, ownership, error/cancellation, priority/timed, and thread-safety
contracts described in this specification.

```cpp
namespace dmn {

class Dmn_Runtime_State;
using DmnRuntimeStatePtr = std::shared_ptr<Dmn_Runtime_State>;

class Dmn_Runtime_State_Manager
    : public Dmn_Singleton<Dmn_Runtime_State_Manager> {
public:
  // Inherited factory:
  // std::shared_ptr<Dmn_Runtime_State_Manager> createInstance();

  // createState returns a shared_ptr handle. Manager will also keep a shared_ptr while the
  // state is queued or running to guarantee lifetime.
  DmnRuntimeStatePtr createState(std::string_view name);

  // Cancel retained states and wait for terminal completion. New run() calls
  // are rejected after shutdown begins.
  void shutdown();
};

class Dmn_Runtime_State
    : public Dmn_State,
      public std::enable_shared_from_this<Dmn_Runtime_State> {
  friend class Dmn_Runtime_State_Manager;

public:
  using OnErrorFnc = Dmn_Runtime_Job::OnErrorFncType; // std::function<void(std::exception_ptr &)>
  using RuntimeStateFnc = std::function<void(Dmn_Runtime_State &)>;

  explicit Dmn_Runtime_State(std::string_view name);

  // State configuration methods are inherited from Dmn_State for pre-run
  // setup. After submission or a terminal outcome, external
  // setStateFnc()/setNext()/setEnd() calls throw std::logic_error.
  // External runNext() always throws
  // std::logic_error; only the manager may advance the machine.
  void setRuntimeStateFnc(RuntimeStateFnc fnc, int index = 0);

public:

  // Lifecycle APIs. A successful run retains the state in the manager until
  // it reaches a completed, failed, or cancelled terminal outcome.
  bool run(Dmn_Runtime_Job::Priority priority = Dmn_Runtime_Job::Priority::kMedium,
           const std::chrono::steady_clock::duration &delay = std::chrono::steady_clock::duration::zero(),
           OnErrorFnc onError = {});

  // Cancellation is cooperative and idempotent. A runtime dispatch observes
  // the request, selects the end, and terminates without invoking another
  // user callback. A callback whose dispatch already started may finish.
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
  enum class Terminal_State { kCompleted, kFailed, kCancelled };

  // Synchronization and completion state.
  std::mutex m_mutex;
  std::promise<void> m_completionPromise;
  std::shared_future<void> m_completionSharedFuture;

  std::exception_ptr m_failure;
  bool m_queued{};
  bool m_running{};
  bool m_started{};
  bool m_completed{};
  bool m_failed{};
  bool m_cancelled{};
  bool m_terminal{};
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
- `Dmn_Runtime_State::run(priority, delay, onError)` returns true on successful
  enqueue and false when the state is unconfigured, cancelled, terminal,
  already active, or its manager has shut down. A runtime submission exception
  propagates after clearing the queued marker. If `delay` is non-zero, the
  first runtime dispatch uses `addTimedJob()`; later dispatches use `addJob()`.
- `run()` accepts an optional onError callback that uses the runtime's `Dmn_Runtime_Job::OnErrorFncType` signature and is forwarded to the runtime job.
- `run()` is one-shot: a successful `run()` prevents subsequent `run()` calls from enqueueing again; such subsequent calls return `false` (no-op). This avoids duplicate enqueues across threads.
- If runtime enqueueing throws, the queued marker is cleared before the
  exception propagates and the caller may retry.
- `cancel()` is cooperative: it sets a cancelled flag. The runtime job, before
  calling `runNext()`, must check `isCancelled()` and call `setEnd()` if the
  state has been cancelled so that the state finalizes without invoking
  another user callback.
- `setRuntimeStateFnc()` is the preferred callback API when a user-state
  callback needs runtime-only methods such as `isCancelled()`. It adapts a
  `Dmn_Runtime_State &` callback into the same underlying state-machine
  storage.
- The inherited `setStateFnc()` remains available for compatibility. A
  callback that stays on the `Dmn_State &` form and wants to observe
  cancellation must capture its runtime-state handle and call `isCancelled()`
  on that handle.
- After successful `run()`, external configuration or transition mutation is
  frozen. Calls to inherited `setStateFnc()`, `setNext()`, and `setEnd()`
  throw `std::logic_error` unless they occur from inside the active
  runtime-managed callback on the runtime thread. External `runNext()` also throws
  `std::logic_error`, including when attempted through a `Dmn_State &` view.
- `wait()` supports a timeout variant and `getFuture()` returns a `std::shared_future<void>` that can be used by multiple waiters. The shared_future is valid immediately after createState() is called and resolves when the state reaches a terminal condition.
- Calls to `run()`, `wait()`, or `wait_for()` from inside the runtime async
  thread must throw `std::runtime_error`. Use the public
  `Dmn_Runtime_Manager::isRunInAsyncThread()` query to detect and enforce
  this.

## 8. Execution Model

### 8.1 State object lifecycle

A runtime state object has the following lifecycle:

1. Created by `Dmn_Runtime_State_Manager::createState()` and returned as a shared_ptr handle
2. Configured by setting state functors (`setStateFnc`, etc.)
3. Idle before `run()` is called
4. Queued for runtime execution after `run()` (manager retains shared_ptr and
   sets `m_queued` while holding the state mutex)
5. Running inside runtime async thread
6. Terminal outcome published and manager-held ownership released
7. Client may call `wait()` at any time after submission or use the shared_future returned by `getFuture()`

### 8.2 `run()` exact semantics and mutex-protected queued flag

When `statehandle->run(priority, delay, onError)` is called:

1. the state object must be validated for legal execution
2. the implementation MUST set `m_queued` while holding the state mutex to
   avoid races where multiple threads try to enqueue simultaneously; only the
   thread that observes it unset proceeds to request the manager to retain an
   internal shared_ptr and submit the runtime job
3. if `m_queued` was already true or the object is terminal, `run()` returns false (no-op)
4. the manager stores a shared_ptr to the object (ensuring lifetime) and schedules a runtime job via `addJob()` or `addTimedJob()` depending on `delay`
5. the scheduled job invokes `runNext()`, which executes at most one
   user-provided state callback and folds in internal initialization or
   finalization when needed
6. before calling `runNext()`, the runtime job checks cancellation; when
   cancelled, it selects the end so `runNext()` finalizes without invoking
   another user callback
7. after the dispatch finishes, the runtime manager reposts a new job only
   when more user work remains
8. if no state remains, the object transitions to completed terminal state, the manager notifies waiters (set promise) and releases its internal shared_ptr
9. if a state callback throws, the exception is captured, the object transitions to failed, waiters are notified, and the optional onError callback is invoked

This loop continues until no more states to run. The runtime manager is responsible for re-posting tasks while the object remains active.

### 8.3 Serialization requirement and optional config

All state object tasks execute serially through the process-wide runtime queue.
The current manager does not provide a relaxed-concurrency mode.

## 9. State Object Contract

### 9.1 Subclassing `Dmn_State`

The runtime state object must subclass `Dmn_State` and preserve its
user-visible stepping semantics while adding runtime lifecycle tracking.

It must retain:

- `setStateFnc()`, `setNext()`, and `setEnd()` semantics for pre-submission
  configuration and in-callback transition control
- `setRuntimeStateFnc()` as a runtime-aware convenience wrapper for callbacks
  that need `Dmn_Runtime_State` methods directly
- internal initialization before the first user callback and finalization
  after terminal selection, inherited from the base
- default state sequencing model

The runtime layer keeps the base configuration methods visible, but adds
dynamic enforcement on top of them: post-submission external mutation throws
`std::logic_error`, while the active runtime-managed callback may still use the
inherited `Dmn_State &` API to select the next transition or terminate.
`runNext()` is reserved for the manager even if a caller obtains a
`Dmn_State &` view of the runtime-managed object.

### 9.2 Cancellation contract

- `cancel()` sets the cancellation flag and is safe to call from any thread.
- The runtime job MUST observe `isCancelled()` before executing `runNext()`
  and call `setEnd()` to force deterministic finalization.
- `setRuntimeStateFnc()` is the preferred way to write a cancellation-aware
  state functor because its callback receives `Dmn_Runtime_State &` directly.
- A callback registered through the inherited `setStateFnc()` may still
  cooperate with cancellation by capturing its `Dmn_Runtime_State` handle and
  querying `isCancelled()` explicitly.

### 9.3 `wait()` behavior and deadlock avoidance

- `wait()` blocks until the object is terminal.
- Implementations MUST detect calls from the runtime async thread and throw
  `std::runtime_error` as described.
- `wait_for(timeout)` returns a bool indicating whether the wait observed terminal completion before the timeout expired.
- `getFuture()` returns a `std::shared_future<void>` available immediately after creation and resolves on terminal state.
- `wait()` and `wait_for()` do not rethrow execution failures;
  `getFuture().get()` rethrows the captured exception.

## 10. Detailed Behavior and Edge Cases

### 10.1 No state configured

If no state is defined before `run()`, the manager must not enqueue an invalid
task. `run()` returns false, leaves the state unsubmitted and non-terminal, and
does not invoke onError. Its inherited `Dmn_State` boolean conversion is also
false because no user-state callback is configured; this does not mean the
runtime state has reached a terminal outcome.

### 10.2 Repeated `run()` calls

A runtime state object must not be run multiple times simultaneously.

Behavior:

- The first successful `run()` enqueues the object and returns true.
- Subsequent `run()` calls (concurrent or later) MUST return false (no-op).
  This avoids duplicate enqueues and is thread-safe due to the mutex-protected
  `m_queued` guard.

### 10.3 Finalized or cancelled states

Once finalized, failed, or cancelled, no further user-state callback may be
scheduled.

### 10.4 Exception propagation and onError

If a state callback throws while inside the runtime manager:

- capture the exception in `std::exception_ptr` stored on the object
- transition object to failed terminal state
- set the completion promise and notify any shared_future waiters
- invoke the optional onError callback provided to `run()` with the captured exception (using `Dmn_Runtime_Job::OnErrorFncType`)
- ensure runtime queue remains healthy

### 10.5 Shutdown race and modes

The manager provides one drain-and-cancel shutdown mode:

- shutdown rejects new `run()` submissions but preserves `createState()` as a
  non-null handle factory;
- it requests cancellation for all manager-retained states and waits for their
  completion futures without holding the manager mutex;
- queued states finalize as cancelled without another user callback;
- a running callback may return normally, but the runtime publishes
  cancellation rather than completion once shutdown has requested it.

The runtime main loop must remain active while shutdown drains queued work.

## 11. Serialization and Scheduling Contract

The manager serializes submitted state instances through the runtime queue.
Each queued job triggers one `runNext()` invocation, which executes at most one
user callback, and reposts only if more user work remains.

Provide clear priority mapping between manager jobs and other runtime jobs. The manager must not starve other runtime jobs; use the runtime's priority scheme and document how manager tasks are enqueued.

## 12. Test Plan (expanded)

### Implemented Unit Tests

`test/dmn-test-runtime-state.cpp` contains focused Google Test cases:

- `CreatesSingletonManagerAndStateHandle`
- `RejectsExternalMutationAfterSubmission`
- `RejectsRecursiveRunNextFromRuntimeCallback`
- `RuntimeCallbackCanObserveCancellationDirectly`
- `RejectsUnconfiguredAndPreRunCancelledStates`
- `ExecutesStatesAndReportsStateFailures`
- `CancelsQueuedStateWithoutRunningUserStep`
- `RetainsStateUntilCompletionAfterClientHandleReleased`
- `HonorsPriorityOrdering`
- `DelaysInitialSubmission`
- `RejectsRunAndWaitOperationsFromRuntimeThread`
- `SerializesMultipleStateExecutions`
- `IsolatesStateFailureFromOtherQueuedStates`
- `HandlesConcurrentStateLifecycleOperations`
- `ShutdownCancelsPendingStatesAndRejectsNewSubmissions`

Together, these cover creation, normal execution, future completion, failure
propagation and onError forwarding, pre-run and queued cancellation,
manager-retained lifetime, priority ordering, timed initial submission,
runtime-thread restrictions, multi-state serialization, failure isolation,
concurrent lifecycle operations, and shutdown draining.

### Integration and Stress Coverage

The implemented tests submit multiple state objects through the same manager,
verify that user callbacks do not overlap and run in the runtime async
context, and verify that a failure does not block an unrelated queued state.
The concurrent lifecycle test exercises 24 client threads, while the shutdown
stress test drains 32 queued states behind an executing callback.

## 13. Acceptance Criteria

The feature is accepted when all of the following are true:

- clients can create runtime-managed state objects from a singleton manager using a shared_ptr handle
- state objects subclass `Dmn_State` and retain its documented user-visible
  stepping semantics
- `statehandle->run(priority, delay, onError)` schedules work into the runtime manager and returns true/false to indicate success
- state execution is serialized through the runtime manager by default
- `statehandle->wait()` and `wait_for()` block until runtime completion or terminal failure and `getFuture()` is available for async waiting (shared_future)
- `cancel()` cooperatively terminates runtime execution and prevents future
  user callbacks; cancellation semantics are documented and tested
- exceptions and cancellation leave the runtime in a valid state and optional onError callbacks are invoked
- documentation, examples and tests exist for typical runtime state workflow usage and lifetime edge cases

## 14. Risks and Mitigations (updated)

### Risk: re-entrant scheduling loop
Mitigation: `run()` must schedule a single runtime task per user-state callback
and stop when the terminal condition is reached. Internal initialization and
finalization do not require extra tasks. No unbounded recursive scheduling
loop is allowed. A mutex-protected queued flag prevents duplicate enqueues.

### Risk: wait deadlock
Mitigation: do not call `wait()` from inside the runtime async thread. Throw
`std::runtime_error` in every build configuration. Prefer future-based waiting
from runtime thread contexts.

### Risk: queued state object lifetime issues
Mitigation: manager must hold a `std::shared_ptr` while queued. Returning a shared_ptr to clients and holding an internal shared_ptr mirrors the existing dmn pattern used in other subsystems (see `dmn-dmesg`).

### Risk: serializer starvation
Mitigation: the runtime manager must keep job postings small, deterministic, and bounded; the runtime's priority and scheduling mechanisms should be used to avoid starvation. Consider adding a configurable fairness mechanism for long-running state functors.

## 15. Implementation Notes

The runtime-state feature is implemented as an API layered on the existing
runtime and state components. It also includes the intentional `Dmn_State`
stepping changes described in FR-3 and NFR-2.

Implementation should reuse:

- `Dmn_Runtime_Manager` for the process-wide scheduler
- `Dmn_State` for the state machine mechanics
- `Dmn_Runtime_Job` and `Dmn_Runtime_Task` for runtime dispatch

The runtime state manager should primarily add:

- state object lifecycle tracking using shared_ptr handles
- queueing and serialization logic
- `wait()` synchronization through the promise/shared_future pair
- terminal-outcome publication and onError callback forwarding
- cooperative cancel() semantics

Sample usage (illustrative):

```cpp
#include <chrono>
#include <thread>

using StatePtr = dmn::DmnRuntimeStatePtr;

// Initialize the process-wide runtime from the main thread before workers are
// created. The state manager can then create and submit managed state handles.
auto runtime = dmn::Dmn_Runtime_Manager<>::createInstance();
auto manager = dmn::Dmn_Runtime_State_Manager::createInstance();
StatePtr s = manager->createState("example");

// Prefer the runtime-aware callback API when the callback may need
// Dmn_Runtime_State methods such as isCancelled().
s->setRuntimeStateFnc([](dmn::Dmn_Runtime_State &st) {
  if (st.isCancelled()) {
    st.setEnd();

    return;
  }

  /* step work */
  st.setEnd();
});

const bool ok = s->run(
    dmn::Dmn_Runtime_Job::Priority::kMedium,
    std::chrono::steady_clock::duration::zero(),
    [](std::exception_ptr &ep) { /* log or inspect error */ });
if (!ok) { /* handle enqueue failure */ }

// enterMainLoop() keeps the runtime active while scheduled state work drains.
std::thread runtimeMainLoop{[runtime] { runtime->enterMainLoop(); }};

// Wait for normal completion, failure, or cancellation.
s->wait();

// Explicitly drain/cancel outstanding runtime states before stopping runtime.
manager->shutdown();
runtime->exitMainLoop();
runtimeMainLoop.join();
```

## 16. Definition of Done

The feature is complete when:

- the singleton runtime state manager is designed and documented with
  `Dmn_Singleton` shared-pointer ownership
- the runtime state object class is specified and matches the required async semantics (run returning bool, onError forwarding, cancel, wait/timeout/shared_future)
- `run()`, `wait()`, and `wait_for()` behavior are documented and tested
- serialized execution through `Dmn_Runtime_Manager` is verified
- shutdown, failure, and cancellation semantics are validated
- example usage patterns are included in the documentation

## 17. Recommended Milestones

1. Complete the manager singleton, state-handle, lifecycle, and scheduling
   implementation (complete).
2. Add focused coverage for cancellation, ownership, priority/timing,
   runtime-thread safety, and runtime-aware callback behavior (complete).
3. Define and implement drain-and-cancel manager shutdown (complete).
4. Add multi-state integration and concurrency stress coverage (complete).

---

This specification is derived from current runtime and state abstractions in:

- `include/dmn-runtime.hpp`
- `include/dmn-runtime-task.hpp`
- `include/dmn-state.hpp`
