# Feature Spec: Runtime State Engine

Status: Implementation-ready
Canonical implementation plan: [`../IMPLEMENTATION_PLAN_runtime-state.md`](../IMPLEMENTATION_PLAN_runtime-state.md)

## 1. Goal and scope

`Dmn_Runtime_State_Engine` is a process-wide factory and lifecycle manager for
one-shot `Dmn_State` machines. A client configures a state handle, submits it
once, and the existing `Dmn_Runtime_Manager` executes one state-machine
invocation per runtime job until the handle reaches a terminal status.

The feature is additive. `Dmn_State` remains synchronous and unchanged. The
runtime state API is deliberately stricter because its state-machine data is
owned by one runtime thread after submission.

Out of scope:

- persistent or distributed state machines;
- parallel state execution;
- automatic retries;
- fairness guarantees against arbitrary higher-priority runtime jobs; and
- restarting an engine after shutdown.

## 2. Public API contract

The public API is declared in `include/dmn-runtime-state.hpp`.

### 2.1 Singleton and state ownership

`Dmn_Runtime_State_Engine` inherits the existing singleton factory:

```cpp
auto engine = dmn::Dmn_Runtime_State_Engine::createInstance();
```

`createInstance()` returns `std::shared_ptr<Dmn_Runtime_State_Engine>`, as
defined by `Dmn_Singleton`. The engine constructor is private and the singleton
base is its friend. Its destructor is public because the singleton factory uses
the standard `shared_ptr` deleter.

`engine->createState(name)` returns
`std::shared_ptr<Dmn_Runtime_State>`. `Dmn_Runtime_State` construction is
private, so every runnable handle is engine-created. The state derives from
`std::enable_shared_from_this`; on a successful submission the engine stores
that exact shared pointer until the handle reaches a terminal status. This
permits fire-and-forget use after the client releases its handle.

Because the constructor is private, `createState()` must construct the handle
with `DmnRuntimeStatePtr{new Dmn_Runtime_State{name}}`, not `make_shared`.
The factory binds the handle to its creating engine in the state implementation;
`run()` uses that binding and `shared_from_this()` to perform pending-map
insertion. A state must never retain a `shared_ptr` back to the engine.

The engine owns a mutex-protected pending-handle map keyed by a monotonically
allocated engine ID. A raw-pointer key is prohibited. A terminal transition
fulfills completion before removing the map entry.

### 2.2 Configuration and transitions

All calls to `setStateFnc()` must complete while the state status is `kIdle`.
Calling it after `run()` has been attempted throws `std::logic_error`.

`Dmn_Runtime_State::FncType` accepts `Dmn_Runtime_State&`, while `Dmn_State`
stores `std::function<void(Dmn_State&)>`. `setStateFnc()` must install an
adapter that downcasts the base reference to `Dmn_Runtime_State&`; callers
must not configure a runtime state through a `Dmn_State&`.

Each configured callback must call exactly one of `setNext(index)`,
`setNext()`, or `setEnd()` before it returns. The runtime wrapper records this
transition. Returning without a transition, or making more than one
transition, throws `std::logic_error` and fails the state. This rule is
necessary because `Dmn_State::runNext()` does not advance automatically.

Transition methods may only be called by the currently executing state
callback on the runtime async thread. Calling them while idle or from another
thread throws `std::logic_error`. `cancel()` is the cross-thread lifecycle
operation.

An empty configuration is invalid. `run()` then atomically transitions the
state to `kFailed`, records `std::logic_error`, fulfills the completion future
with that exception, and returns `false`. It does not invoke `onError`,
because no runtime job was accepted.

### 2.3 Lifecycle

The authoritative statuses are:

| Status | Meaning |
|---|---|
| `kIdle` | Configurable and not submitted. |
| `kQueued` | Accepted and waiting for its next runtime job. |
| `kRunning` | A state callback is executing on the runtime async thread. |
| `kCompleted` | `Dmn_State` finalized normally. |
| `kFailed` | Configuration or callback execution failed. |
| `kCancelled` | Cancellation became terminal before another callback began. |

The three latter statuses are terminal. `isRunning()` is true for `kQueued`
and `kRunning`; `isCancelled()` is true only for `kCancelled`, not merely for
a cancellation request. `failure()` returns the stored exception for
`kFailed`, otherwise `nullptr`.

`run()` is one-shot. It atomically changes a valid idle state to `kQueued`;
every later call returns `false`. A call from the runtime async thread throws
`std::runtime_error`. Runtime context is checked with the now-public,
thread-safe `Dmn_Runtime_Manager::isRunInAsyncThread() const`.

Only `kHigh`, `kMedium`, and `kLow` priorities are accepted. `kSched` and a
negative delay are invalid configuration: `run()` marks the state failed with
`std::invalid_argument`, returns `false`, and does not invoke `onError`.

`cancel()` is idempotent and may be called from any thread:

- In `kIdle`, it immediately becomes terminal `kCancelled` and fulfills the
  completion future successfully.
- In `kQueued`, it records a cancellation request. The queued runtime job
  observes it, invokes `Dmn_State::setEnd()` followed by `runNext()` to
  finalize the base state without executing a user callback, then becomes
  `kCancelled`.
- In `kRunning`, it records a cancellation request only. The active callback
  is never preempted. Once it returns, the engine does not repost another user
  callback; it performs the same base finalization and becomes `kCancelled`.
- It has no effect on a terminal state.

This guarantees that an idle state remains pending only until `run()` or
`cancel()` is called. A future from an ordinary never-run, never-cancelled
state is intentionally not ready.

### 2.4 Completion and errors

A `std::promise<void>` and `std::shared_future<void>` are created with each
state. `getFuture()` is valid immediately and is safe for multiple callers.

- Completed and cancelled states call `promise.set_value()`.
- Failed states call `promise.set_exception(failure())`.
- `wait()` and `wait_for()` only observe readiness; they do not rethrow.
- `getFuture().get()` rethrows the captured exception for `kFailed`.

`wait()` and `wait_for()` throw `std::runtime_error` on the runtime async
thread. They otherwise use the shared future and must not busy-spin.

For a callback exception, the runtime-state job first records `kFailed` and
fulfills the promise, then rethrows the same exception. The job supplied to
`Dmn_Runtime_Manager` carries the caller's `onError` unchanged. Thus the
runtime invokes it exactly once under its existing error handling. The engine
must not invoke it directly. If `onError` throws, the existing runtime
behavior applies; the state has already reached its failed terminal state.

## 3. Scheduling and ordering

`run(priority, delay, onError)` submits the initial job with `addJob()` when
`delay == 0`, otherwise with `addTimedJob()`. A delay applies only to the
initial job. Each continuation is submitted immediately at the same priority.

Each job performs at most one `Dmn_State::runNext()` call. A successful call
that leaves the base state active schedules the continuation. A call that
finalizes the base state transitions the runtime state to `kCompleted`.

The runtime has one async execution thread, so state callbacks cannot execute
concurrently. The engine adds no second worker queue. Ordering is exactly the
existing runtime policy: priority order is high, then medium, then low; the
underlying queue determines same-priority order. There is no posting-order
guarantee across priorities and no starvation bound when clients continuously
submit higher-priority runtime jobs. Tests must not assert either property.

## 4. Engine shutdown

`shutdown(ShutdownMode)` is thread-safe, non-blocking, and irreversible:

- `kGraceful` rejects new `run()` calls and allows accepted states to finish.
- `kImmediate` rejects new `run()` calls, terminally cancels queued states,
  fulfills their futures, removes their engine ownership, and records a
  cancellation request for a running callback. A queued job that runs after
  immediate shutdown is a no-op because its state is already terminal.

The engine does not stop or destroy `Dmn_Runtime_Manager`; process-level
runtime shutdown remains the runtime manager's responsibility. `isShutdown()`
reports whether either mode has been selected.

## 5. Required implementation boundaries

`Dmn_Runtime_State` and `Dmn_Runtime_State_Engine` each use a private `Impl`
defined in `src/dmn-runtime-state.cpp`. The state implementation owns its
status mutex, completion promise/future, failure exception, callback count,
transition guard, cancellation request, original submission metadata, engine
ID, and a non-owning binding to the creating singleton. The engine
implementation owns its mutex, shutdown state, next ID, and pending map.

All terminal transitions must be idempotent under the appropriate mutex.
Never hold an engine mutex while invoking a user state callback, fulfilling a
promise, or allowing engine ownership to be released.

The new source must be added to the `dmn` library target and the public header
must be listed with the target's interface headers. `include/dmn.hpp` must
include `dmn-runtime-state.hpp`.

## 6. Required tests

- engine singleton creation and factory-only state construction;
- successful multi-step execution with explicit transitions;
- missing and duplicate transitions fail the state;
- empty configuration fails before submission;
- repeated/concurrent `run()` accepts exactly once;
- priority and initial-delay forwarding without cross-priority order claims;
- future obtained before submission, multiple waiters, success, failure, and
  cancellation result semantics;
- cancellation while idle, queued, and running;
- client handle destruction while queued;
- `wait()` and `wait_for()` rejection from a runtime callback;
- callback failure reaches `failure()`, future `get()`, and `onError` exactly
  once; and
- graceful and immediate shutdown behavior.
