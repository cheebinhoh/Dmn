# Asynchronous execution and lifetime

**Status:** Code-derived contract for the existing thread, async-queue,
singleton, and timer helpers. Recommendations are listed separately below.

## Modules and responsibilities

- `Dmn_Proc` (`include/dmn-proc.hpp`, `src/dmn-proc.cpp`) owns one pthread and
  invokes a stored `std::function<void()>`.
- `Dmn_Async<QueueType>` (`include/dmn-async.hpp`) serializes submitted
  callables through a `Dmn_Pipe` backed by the selected queue template.
- `Dmn_Singleton<T>` (`include/dmn-singleton.hpp`) lazily constructs and shares
  one instance per `T`.
- `Dmn_Timer<Duration>` (`include/dmn-timer.hpp`) repeats a callback after each
  relative interval using a scheduled-mode `Dmn_Pipe`.

## Contracts

### `Dmn_Proc`

Construction optionally supplies the task. `exec()` may replace it and starts
a new thread only from the ready state. `wait()` joins a running thread;
`stopExec()` requests deferred pthread cancellation and joins. A caller can
implement cooperative exit in its task by capturing synchronized caller-owned
state, checking it, and arranging to wake any blocking operation; `wait()`
then joins after the task returns. This does not change `Dmn_Proc`'s API.
`yield()` checks for cancellation and yields the scheduler; `testcancel()` is
a cancellation point. Destruction cancels a running thread and sets the
internal state to unknown. Copy and move are disabled. Lifecycle operations
and destruction are not internally
synchronized. Callers must ensure `exec()`, `wait()`,
`stopExec()`, task replacement, and destruction do not overlap and that each
operation happens-before the next. A caller may hand off lifecycle control to
another thread using external synchronization; all calls need not originate on
one OS thread. Do not replace the task or destroy the object before the worker
has been joined. State used by both the worker task and callers requires its
own synchronization.

Worker exceptions terminate the process by default, preserving the legacy
behavior. The optional `ExceptionPolicy::kCaptureAndRethrowFromWait` policy
captures task/setup exceptions and rethrows them from `wait()` after the join;
`stopExec()` also propagates a captured failure through its call to `wait()`.
The destructor remains non-throwing and suppresses failures while cleaning up.

Pthread cancellation is deferred. A task blocked outside a cancellation point
may delay `stopExec()` and destruction. Client-owned cooperative stopping
requires the task to check synchronized caller-owned state and the owner to
wake blocking operations. Cleanup macros are thin wrappers around pthread
cleanup registration and are intended to release resources such as mutexes or
in-flight tickets.

### `Dmn_Async`

Submitted callables execute serially on one worker. Immediate tasks are
processed in FIFO submission order. `addExecTask()` does not wait;
`addExecTaskWithWait()` returns a single-use handle whose `wait()` blocks and
rethrows task exceptions. Delayed forms accept a chrono duration and must not
execute before their computed steady-clock due time; negative durations are
rejected synchronously with `std::invalid_argument` before enqueueing, while a
zero duration is accepted and immediately eligible. They are submitted to the
pipe's deadline-scheduled queue only after converting the
delay and checking the due-time addition. An unrepresentable delay or deadline
throws `std::overflow_error` before enqueueing; positive fractional clock ticks
are rounded up so a task is not made eligible early. The queue blocks the worker
until new work arrives or the earliest deadline; an overdue delayed task cannot
cause a dequeue/re-enqueue polling loop. Immediate tasks use the ordinary queue
and retain priority over delayed tasks, which can therefore be overtaken while
waiting for their deadlines. `waitForEmpty()` waits for the accepted-work
snapshot to pass through the pipe. Destruction resets the pipe and stops its
worker; because the scheduled pipe drains accepted work when callbacks return
normally, destruction may wait until pending delayed tasks become due. Delayed
tasks with equal deadlines retain submission order.

Task exceptions are captured by the task handle and do not stop the worker.
The generic pipe worker itself still has no failure-reporting channel; a
failure outside the task invocation/handle completion path can stop the worker
silently and leave pending work unprocessed.

The implementation binds the callable and arguments by move-capturing them,
then invokes them as rvalues. A callable should therefore be invocable once.
Reference capture macros require the captured objects to outlive execution.

### `Dmn_Singleton`

`createInstance(args...)` runs an optional static
`T::runPriorToCreateInstance()` hook once and constructs the shared instance
once via `std::call_once`. Only the arguments supplied by the successful
initialization are used. Later calls return the same `shared_ptr`.

### `Dmn_Timer`

`Dmn_Timer<T>` owns a scheduled-mode `Dmn_Pipe` and starts its worker during
construction. The pipe invokes a timer wrapper for each scheduled tick. The
wrapper invokes the client callback and schedules the next tick one interval
after callback completion, preserving fixed-delay cadence. A strictly positive
interval is required; conversion into the steady-clock duration rounds up to
the next clock tick and throws on an unrepresentable interval or deadline.

Every scheduled tick carries a shared generation token. `start()` and
`resume()` schedule a new generation; stale ticks are discarded even if they
wake after a resume. `stop()` pauses by marking the timer inactive under its
state mutex; it does not cancel a queued pipe item or wait for a callback
already admitted. The wrapper copies the callback under the mutex, invokes it
without holding that mutex, then checks the generation and active state before
scheduling another tick. This prevents stop/resume or restart from producing
duplicate recurring chains.

`start(reltime, fn)` validates the new interval before modifying existing timer
state. An empty callback retains the previous callback. `resume()` is a no-op
when already active and rethrows a previously stored asynchronous failure;
`start()` can recover from such a failure by scheduling a new generation.
`start()`, `stop()`, and `resume()` are serialized internally by the timer
mutex, but destruction must not race public calls or occur from the timer's
callback.

Destroying the timer marks it inactive, then shuts down and joins the pipe.
Since scheduled pipe shutdown drains accepted work, destruction can wait until
stale ticks reach their deadlines. The pipe worker is started during pipe
construction; a worker-start failure is reported by an exception propagated
from the timer constructor. The timer no longer uses `Dmn_Proc::exec()`.

Standard client callback exceptions are logged through `DMN_DEBUG_PRINT` and
the timer continues at the next interval, matching prior behavior. A
non-standard callback exception pauses the timer and is saved; an asynchronous
failure to schedule a later tick does the same. `rethrowFailure()` surfaces
either stored failure. Failures to schedule the first tick from construction,
`start()`, or `resume()` are reported synchronously and do not publish a
partially active generation.

Tests cover configured-delay delivery, restart, rejecting non-positive and
unrepresentable intervals, preserving a running timer after invalid restart,
no callback from a pending tick after pause, pause/resume generation
invalidation, pausing during an active callback, continuing after standard
callback exceptions, reporting non-standard callback exceptions, and
propagating a deterministic worker-thread creation failure through timer
construction. With `ENABLE_FAULT_INJECTION=ON`, the
`fault-injection`-labelled `dmn-test-fi-timer-thread-start-failure` test
activates `dmn/timer/pipe/proc/pthread_create` and verifies that the failure
propagates from construction. The `dmn-test-fi-proc-thread-start-failure`
test activates the same point directly against `Dmn_Proc::exec()` and verifies
that the task does not run. The `dmn-test-fi-timer-reschedule-failure` test
activates `dmn/timer/reschedule/write_at`, lets the first callback run, then
verifies that failure to enqueue its next tick pauses the timer, is reported by
`rethrowFailure()`, and is rethrown by `resume()`. These tests close the
deterministic-coverage gap for direct process startup, timer worker startup,
and recurring tick rescheduling failures; they do not inject failures into the
underlying scheduled queue or cover other `Dmn_Proc` callers.

## Thread-safety and lifetime requirements

The async queue provides serialized callback execution, but does not make
captured application state safe to access from other threads. A caller must
manage captured-object lifetime and synchronization. `Dmn_Proc` lifecycle calls
are not internally synchronized; callers must serialize them externally.
`Dmn_Timer` serializes `start()`, `stop()`, and `resume()` with its state mutex,
but destruction must not race public calls or run from its callback. For a
`Dmn_Pipe` with a background worker, callers must serialize `shutdown()` calls;
its shutdown flag does not make concurrent shutdown invocations safe. A worker
thread must not outlive the object whose members its task accesses.
