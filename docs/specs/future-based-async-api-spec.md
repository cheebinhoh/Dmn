# Future-Based Asynchronous API Extension

**Status:** The first two increments (`Dmn_Async` typed task futures and
`Dmn_Pub::publishAsync()`) are implemented. The remaining items are proposed
follow-on work. This spec identifies existing Dmn operations that can expose
completion through `std::future` without replacing their current callback,
queue, or synchronous contracts.

## 1. Goal

Add an opt-in completion-oriented API for work that Dmn already schedules or
for which it already receives a completion notification. A caller can submit
an operation without waiting for its completion and later observe its value or
exception with `std::future<T>`. Submission retains each API's existing queue
backpressure behavior and may itself wait for queue capacity.

This is a completion API, not by itself a callback/continuation API. A future
represents one result and has one consumer. It does not provide repeated event
delivery, cancellation, a scheduler, or automatic nonblocking composition.
Long-lived and repeated notifications continue to use callbacks/subscriptions
or an explicitly designed event-stream abstraction.

The existing `Dmn_DbusClient::callAsync()` and move-only `Dmn_DbusCall` are
the reference for submitting an operation to an existing worker and receiving
an owned result without blocking the submitter. This proposal reuses that
pattern where Dmn already has a queue or completion callback; it does not
prescribe copying the D-Bus transport/client implementation into other layers.

## 2. Code-derived candidates and recommendation

| Priority | Existing API and implementation | Future-compatible operation | Fit and constraints |
|---|---|---|---|
| 1 | `Dmn_Async` in `include/dmn-async.hpp` already queues serialized callables. `addExecTaskWithWait()` creates a `promise<void>` and returns a blocking, shared `Dmn_Async_Handle`. | **Implemented:** `addExecTaskFuture(callable, args...) -> std::future<R>` and delayed counterpart; existing methods are retained. | Foundational reuse: fulfillment happens on the worker that executes the callable. It preserves queue order, delayed-write rules, and exception propagation without creating a thread per submission. |
| 2 | `Dmn_Pub::publish()` in `include/dmn-pub-sub.hpp` queues `publishInternal()` on `Dmn_Async`; `block=true` waits and `block=false` returns before delivery. | **Implemented:** `publishAsync(item) -> std::future<void>`. Async registration variants remain a separate proposal. | Natural typed wrapper on the serialized publisher queue. `get()` observes callback/filter exceptions after delivery completes; failures remain recorded as today. Future completion does not imply that a subscriber has finished work it separately queued. |
| 3 | `Dmn_DMesgHandler::write()` and `writeAndCheckConflict()` in `include/dmn-dmesg.hpp` and `src/dmn-dmesg.cpp` serialize writes on the handler worker; conflict-aware writes already wait for publisher processing and return `bool`. | Add explicit `writeAsync()` and `writeAndCheckConflictAsync()` returning `std::future<void>` and `std::future<bool>` respectively, after the foundational async API is stable. | Useful where handlers are used on latency-sensitive paths. Define the future's completion boundary precisely: conflict-aware completion includes publisher conflict processing and the handler snapshot needed to produce the bool. Own/copy/move the message into queued work. The handler must remain alive until completion. |
| 4 | `Dmn_Kafka::write()` in `src/kafka/dmn-kafka.cpp` blocks while polling librdkafka until the delivery callback sets an atomic flag; `read()` blocks in consumer polling. | First consider `writeAsync()` that completes on delivery report. A truly nonblocking submitter requires a client-owned poll/event worker. A future-returning read requires a separately designed consumer loop and cancellation/shutdown contract. | Delivery callbacks already signal a one-shot completion, but today the writing thread performs the polling that drives callback delivery. Returning a future without transferring that poll responsibility would hang. Do not add an unowned helper thread per message. |
| 5 | `Dmn_Runtime_Manager::addJob()` and `addTimedJob()` in `include/dmn-runtime.hpp` schedule jobs; jobs may return `void` or `Dmn_Runtime_Task` and failures route through an optional error callback. | Consider a completion future for one-shot jobs after the runtime's coroutine and error-handler semantics are specified. | Needs a clear result for failures already consumed by `onErrorFnc`, and a completion point after coroutine final suspension. Do not block the singleton runtime worker on a future or use `std::future::get()` to sequence runtime jobs. |
| 6 | `Dmn_Proc::exec()` starts a thread and `wait()` joins it; an optional exception policy captures worker exceptions for rethrow from `wait()`. | A per-execution shared completion state/future could expose thread completion to multiple observers. | Lower priority: thread lifecycle and restart semantics need a generation model, and a future must not outlive or access a destroyed `Dmn_Proc`. Keep `wait()` and explicit cooperative stop semantics. |
| 7 | `Dmn_DMesgNet` currently receives inbound messages on a worker and gates outbound application messages on its `m_ready` state. | A one-shot `readyAsync()` can be considered for first-ready notification, paired with a separate state/event observer if readiness can regress. | A future alone cannot express repeated ready/pending transitions. Do not replace the current system-message/event path with a one-shot result. |

## 3. APIs that should not be changed into future-only interfaces

- `Dmn_Io<T>` in `include/dmn-io.hpp` is a transport-neutral synchronous
  interface. Its implementations have materially different blocking,
  threading, error, and cancellation behavior. Adding `std::future` virtual
  methods to the base would either force hidden worker creation for synchronous
  adapters or provide a misleading wrapper that still blocks at submission.
  Any future-based I/O operation must first specify its executor ownership,
  shutdown, cancellation, and typed error contract, preferably on a concrete
  adapter or a new explicit async interface.
- `Dmn_Socket::read()` is a blocking UDP read and its current
  `shutdown()` contract does not interrupt that read. A future would not fix
  the wakeup/lifetime problem; first use an explicit poller/reactor or
  cancellation mechanism with owned socket lifetime.
- `Dmn_Timer` is recurring. Its callback can be observed repeatedly; a single
  future does not model ticks. Keep its callback interface and failure
  reporting. A future may later represent one explicitly requested next tick,
  but not replace recurring callbacks.
- `Dmn_State::runNext()` is a caller-driven synchronous state machine. It has
  no existing background queue or asynchronous result to expose; futures would
  change its execution model rather than piggyback on it.
- `Dmn_DbusClient::subscribe()` and `Dmn_BluezClient` event handlers represent
  streams of signals/state changes. Preserve subscription/event callbacks.
  Futures remain suitable for one-shot method calls, which the generic D-Bus
  API already implements.

## 4. Foundation API contract

The implemented first increment extends `Dmn_Async` without replacing or
changing the existing API:

```cpp
template <typename Callable, typename... Args>
auto addExecTaskFuture(Callable &&callable, Args &&...args)
    -> std::future<std::invoke_result_t<std::decay_t<Callable>,
                                        std::decay_t<Args>...>>;

template <class Rep, class Period, typename Callable, typename... Args>
auto addExecTaskAfterFuture(
    const std::chrono::duration<Rep, Period> &delay,
    Callable &&callable, Args &&...args)
    -> std::future<std::invoke_result_t<std::decay_t<Callable>,
                                        std::decay_t<Args>...>>;
```

Both immediate and delayed forms use matching names so one-shot completion
does not have different behavior depending on scheduling mode.

Required semantics:

- The callable is enqueued on the existing `Dmn_Async` worker. No new thread is
  created per operation. Successful submissions execute exactly once and
  preserve the existing order for immediate work; delayed work retains the
  existing steady-clock deadline and immediate-work priority rules.
- The returned `std::future<R>` becomes ready only after the callable returns
  or throws. A non-void result is stored as an owned `R`; a `void` result
  fulfills a `future<void>`. A task exception is stored with
  `std::promise::set_exception()` and rethrown by `get()`.
- Reference-returning tasks are rejected; callers return `void` or an owned
  value so the future cannot outlive the result it observes.
- Callables/arguments are owned by the queued task using the same move-capture
  and one-shot invocation semantics as `addExecTaskWithWait()`. References
  captured explicitly by the caller remain the caller's lifetime
  responsibility.
- A submission/queue failure is reported synchronously from the submit call,
  consistent with existing `Dmn_Async` enqueue failures. Submission retains
  the existing bounded queue's backpressure behavior and may block until queue
  capacity is available. Once accepted, an operation does not disappear
  silently.
- Destroying or abandoning the future does not cancel accepted work. Standard
  `std::future` has no cancellation operation; cancellation is out of scope
  for this increment. Existing shutdown/drain semantics remain unchanged.
- Do not use `std::async`, detached threads, or a per-call thread pool as an
  implementation shortcut. Completion is signaled by the existing worker.
- Do not block the worker waiting for its own future. Do not call `get()` from
  the worker whose queue must execute that operation. Document this
  self-deadlock constraint on the new API.
- Futures are single-consumer. If an operation needs multiple independent
  observers, specify `std::shared_future` deliberately or provide a separate
  observer; do not silently copy a `std::future`.

Existing `addExecTask()`, `addExecTaskWithWait()`,
`addExecTaskAfter()`, `addExecTaskAfterWithWait()`, `Dmn_Async_Handle`,
`waitForEmpty()`, and destruction behavior remain source- and behavior-
compatible. The new API is additive.

## 5. Layer-specific API contracts after the foundation

### 5.1 Publisher completion

The additive `Dmn_Pub::publishAsync(const T &item)` enqueues exactly the
same publication operation as `publish(item, false)` and returns a future that
becomes ready after `publishInternal()` has completed all synchronous filter
and notification calls for that publication. Exceptions from those callbacks
are observable through `future.get()` and remain recorded by
`takeCallbackFailures()` according to its current capacity/order semantics.
Submission does not wait for publication completion but retains the existing
queue's backpressure behavior. The future does not wait for work that a
subscriber itself enqueues elsewhere.

Do not implement this by calling synchronous `publish(item, true)` on another
thread. The operation should be queued directly and completed on the existing
publisher worker.

### 5.2 DMesg handler completion

Only add DMesg future APIs after `Dmn_Async` and publisher completion semantics
are tested. Candidate semantics:

- `writeAsync(message, flags)` completes when the handler-context write has
  been stamped, conflict-checked, and accepted by the publisher queue. If
  flags require publisher processing (including forced-write conflict rules),
  completion includes that processing.
- `writeAndCheckConflictAsync(message, flags)` returns a
  `future<bool>` and completes only after publisher processing and ordered
  conflict-state application to the handler context. The bool has the same
  meaning as the existing blocking method. Implementation must not block the
  handler worker waiting for publisher work and then inspect handler state
  from that same worker; publisher completion should enqueue a handler-context
  finalizer behind the corresponding conflict event, which reads the state
  and fulfills the promise.
- Copy overloads own a copy before returning; rvalue overloads transfer or
  move into owned queued work. A `future` must never retain a reference to a
  caller's stack message.
- The handler must outlive completion. Closing/destroying a handler while an
  accepted future operation is outstanding is prohibited unless a later
  shared-lifetime design explicitly changes handler ownership.
- Waiting/getting from a handler or publisher callback that must execute the
  future's queued work is invalid and must be documented.

These signatures and boundaries require a focused follow-on design review
before implementation; this document does not authorize changing current
blocking write behavior.

### 5.3 Kafka producer completion

Do not expose `Dmn_Kafka::writeAsync()` until callback polling ownership is
resolved. A valid design must arrange for delivery callbacks to be serviced
independently of the submitting caller, likely through one producer-owned poll
worker shared by all in-flight messages. Each delivery report must fulfill or
fail exactly the matching message's promise. Enqueue errors complete/fail that
submission without waiting for a delivery report. Shutdown must stop admission,
drain or explicitly fail accepted deliveries according to a documented
deadline, join the poll worker, and only then destroy librdkafka state.

A first Kafka change may instead provide a blocking `writeAndWait()` alias or
an explicit completion handle without calling it asynchronous; that is not a
substitute for nonblocking submission.

## 6. Error, lifetime, and threading rules

- A future reports completion/result only; it does not imply cancellation,
  retry, timeout, or durable delivery unless the specific API contract says
  so.
- For accepted queued work, exceptions are stored in the future. Failures
  before acceptance remain synchronous exceptions/errors. Each operation must
  complete at most once.
- Objects referenced by accepted work must remain alive until that work has
  completed. Prefer owned captures and weak/shared internal state where
  ownership permits; never capture a borrowed user reference to make the
  returned future appear self-contained.
- Destruction and shutdown retain each module's existing admission and drain
  contract. An unresolved future must not silently turn into success if the
  underlying object shuts down; the relevant layer must either complete it
  with its established shutdown exception/error or document why accepted work
  is drained before destruction returns.
- A future wait can block. APIs should name their execution context and
  identify any `get()`/wait calls that would deadlock by waiting on the same
  serialized worker.
- Do not invoke client callbacks while holding a queue, transport, or promise
  registry mutex. Promise fulfillment must happen after state required by the
  documented completion boundary has been applied.
- Where the existing API uses typed `expected` results, preserve that error
  model inside the future rather than converting expected operational errors
  into undocumented exceptions. Use exception storage for exceptions already
  thrown by a callable; introduce no broad catch that converts failures into
  success-shaped defaults.

## 7. Test and acceptance matrix

All tests for queue-based future APIs must use the real Dmn worker/queue
implementation. Do not replace `Dmn_Async`, `Dmn_Pub`, or the target adapter
with mocks. Transport-specific tests may use an isolated fake broker/service
only when the actual transport adapter remains under test.

### Implemented API test coverage

The initial future APIs are covered in the existing test executables:

| API | Test | Required cases |
|---|---|---|
| `Dmn_Async::addExecTaskFuture()` | `test/dmn-test-async.cpp`: `FutureTasksReturnValuesAndPropagateExceptions` | Value and `void` results, owned argument capture, exception propagation, worker remains usable after failure, FIFO execution and result completion. |
| `Dmn_Async::addExecTaskAfterFuture()` | `test/dmn-test-async.cpp`: `DelayedFutureRespectsDeadlineAndValidatesDuration`, `DelayedFutureCannotRunBeforeDeadline` | Delayed value and exception, zero and negative delay, unrepresentable delay/deadline, and execution no earlier than the deadline. |
| Future abandonment and owner shutdown | `test/dmn-test-async.cpp`: `DiscardingFutureDoesNotCancelAcceptedTask`, `AcceptedFutureCompletesWhenAsyncIsDestroyed` | Accepted task still executes when its future is destroyed; accepted future completes while `Dmn_Async` drains during destruction. |
| `Dmn_Pub::publishAsync()` | `test/dmn-test-pub-sub-errors.cpp`: `PublishAsyncOwnsItemAndCompletesAfterDelivery`, `PublishAsyncPreservesOrderAndSurvivesAbandonedFuture`, `PublishAsyncDoesNotWaitForSubscriberQueuedWork`, `PublishAsyncReportsAndRecordsCallbackFailure`, `PublishAsyncReportsFilterFailure` | Item copied before return, delivery completion, queue order, abandonment does not cancel, notification/filter failure reaches `get()`, failure is retained, later subscribers still receive the item, and work separately queued by a subscriber is not included in publisher completion. |

### Foundation (`Dmn_Async`)

- Verify `future<void>` and `future<R>` readiness/result after queued tasks.
- Verify task exception is rethrown by `get()` and does not terminate or poison
  the worker; submit and complete a subsequent task.
- Verify FIFO ordering for multiple immediate tasks and exact-once invocation.
- Verify callable and argument ownership and verify discarding the future does
  not cancel accepted work.
- Verify delayed future result and exception behavior, not-ready-before-deadline
  and ready-afterward behavior; include zero delay, negative delay,
  unrepresentable delay, and deadline-overflow rejection while preserving
  current semantics.
- Verify accepted future work is completed/drained when the helper is
  destroyed according to existing queue behavior; future destruction does not
  cancel it.
- Verify synchronous queue-submission errors remain observable, and that
  waiting from the same worker is explicitly prohibited/documented. Queue
  saturation continues to exercise the existing backpressure contract.
- Retain every existing `Dmn_Async`, `Dmn_Pipe`, queue, and timer regression.

### Publisher (`Dmn_Pub`)

- Verify `publishAsync()` completion follows actual delivery to all eligible
  subscribers and preserves publication ordering and filtering.
- Verify callback/filter exceptions reach `future.get()`, are recorded once,
  and do not suppress delivery to later subscribers.
- Verify successive future-backed publications preserve publisher queue order.
- Verify future completion does not wait for downstream work enqueued by a
  subscriber on another worker.
- Verify abandoning the future does not cancel publication and publisher
  destruction drains accepted publication work per its current contract.
- Verify the old `publish(item, false)` and `publish(item, true)` behavior is
  unchanged.

### DMesg (later phase)

- Verify owned copy/move inputs and ordered handler writes.
- Verify `writeAsync()` exceptions match the blocking write exception
  contract.
- Verify `writeAndCheckConflictAsync()` resolves true/false only after the
  corresponding ordered conflict event is applied.
- Verify force, conflict, playback, shutdown, handler-close ordering, and
  callback reentrancy/deadlock restrictions.

### Kafka (later phase)

- With the actual Kafka adapter and a controlled broker/test harness, verify
  immediate produce-enqueue failure, successful delivery, delivery error,
  concurrent messages correlated to their own futures, bounded in-flight work,
  timeout/shutdown behavior, and callback worker drain before handle destroy.
- Verify submission returns without waiting for broker delivery and does not
  create a thread per message.
- Keep consumer future/stream behavior out of scope until a separate API and
  cancellation contract is approved.

### Runtime and process (optional later phases)

- Runtime jobs: verify future result/exception for ordinary and timed jobs,
  priority/deadline behavior, coroutine completion, and error-hook semantics
  without blocking the runtime worker.
- `Dmn_Proc`: verify completion state is per execution generation, exception
  policy remains consistent, and futures cannot access destroyed process state.

## 8. Implementation sequence

1. Implement and test additive typed future submission in `Dmn_Async`. Retain
   all existing task/handle APIs.
2. Add `Dmn_Pub::publishAsync()` atop that worker and test callback completion,
   exceptions, ordering, and unchanged legacy publication modes.
3. Review and specify DMesg async write/conflict completion after confirming
   handler/publisher context ordering and handler lifetime behavior.
4. Separately design Kafka producer polling ownership before adding a
   future-returning producer call. Keep consumer and generic `Dmn_Io` future
   proposals out of this change until their executor/cancellation contracts
   are defined.
5. Consider runtime job and `Dmn_Proc` futures only if a concrete caller needs
   completion values; preserve the coroutine scheduler and restart semantics.

## 9. Related specifications

- [`async-execution-spec.md`](async-execution-spec.md) describes current
  `Dmn_Async`, `Dmn_Proc`, and timer execution/lifetime behavior.
- [`pub-sub-spec.md`](pub-sub-spec.md) describes publisher ordering, callback
  failure collection, and subscriber lifetime.
- [`dmesg-spec.md`](dmesg-spec.md) describes DMesg handler serialization and
  conflict processing.
- [`kafka-spec.md`](kafka-spec.md) describes current blocking producer writes
  and consumer polling.
- [`dbus-client-spec.md`](dbus-client-spec.md) describes the existing
  one-shot future-backed D-Bus call pattern and its distinct signal-subscription
  API.
