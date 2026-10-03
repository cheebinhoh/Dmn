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
  relative interval using a `Dmn_Proc` thread.

## Contracts

### `Dmn_Proc`

Construction optionally supplies the task. `exec()` may replace it and starts
a new thread only from the ready state. `wait()` joins a running thread;
`stopExec()` requests deferred pthread cancellation and joins. `yield()` checks
for cancellation and yields the scheduler; `testcancel()` is a cancellation
point. Destruction cancels a running thread and sets the internal state to
unknown. Copy and move are disabled. Lifecycle operations and destruction are
not internally synchronized. Callers must ensure `exec()`, `wait()`,
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

Cancellation is cooperative. A task blocked outside a cancellation point may
delay `stopExec()` and destruction. Cleanup macros are thin wrappers around
pthread cleanup registration and are intended to release resources such as
mutexes or in-flight tickets.

### `Dmn_Async`

Submitted callables execute serially in queue order. `addExecTask()` does not
wait; `addExecTaskWithWait()` returns a single-use handle whose `wait()` blocks
and rethrows task exceptions. Delayed forms accept a chrono duration and must
not execute before their computed steady-clock due time. `waitForEmpty()` waits
for work to pass through the pipe. Destruction resets the pipe and stops its
worker.

The implementation binds the callable and arguments by move-capturing them,
then invokes them as rvalues. A callable should therefore be invocable once.
Reference capture macros require the captured objects to outlive execution.

### `Dmn_Singleton`

`createInstance(args...)` runs an optional static
`T::runPriorToCreateInstance()` hook once and constructs the shared instance
once via `std::call_once`. Only the arguments supplied by the successful
initialization are used. Later calls return the same `shared_ptr`.

### `Dmn_Timer`

Construction starts the periodic thread. Each cycle sleeps for the configured
duration and invokes the callback; it is a relative recurring timer, not a
real-time deadline scheduler. `start()` stops the old thread and restarts with
the new duration and optional replacement callback, updating those settings
only after the old worker has been joined. `start()` propagates stop/join
errors but currently ignores the boolean result of thread creation, so a failed
restart is not reported. `stop()` requests cancellation and joins but
suppresses exceptions from those operations. Callers must serialize `start()`,
`stop()`, and destruction. Callback exceptions derived from `std::exception`
are printed only through the debug macro; other exception types escape the
pthread entry point and normally cause `std::terminate`.

## Thread-safety and lifetime requirements

The async queue provides serialized callback execution, but does not make
captured application state safe to access from other threads. A caller must
manage captured-object lifetime and synchronization. `Dmn_Proc` and
`Dmn_Timer` do not synchronize lifecycle calls; callers must serialize them
with external synchronization, including handoffs between controlling
threads. For a `Dmn_Pipe` with a background worker, callers must also
serialize `shutdown()` calls; its shutdown flag does not make concurrent
shutdown invocations safe. A worker thread must not outlive the object whose
members its task accesses.

## Gaps / improvements

1. A delayed `Dmn_Async` task is re-enqueued until due. If it is the only
   queued task, the worker repeatedly dequeues and re-enqueues it without
   blocking, consuming CPU while waiting. Use a timer/wait mechanism or
   otherwise avoid polling; improve the existing sleep-based due-time test and
   add coverage for idle CPU use.
2. Define and test behavior for negative durations in `Dmn_Async` and
   `Dmn_Timer`, and detect overflow in the async duration-to-nanoseconds
   conversion and due-time addition. `Dmn_Timer` startup ignores a `false`
   return from `Dmn_Proc::exec()`, while
   `stop()` suppresses cancellation/join exceptions; decide how those failures
   should be reported.
3. Specify and test the timer callback failure policy. `Dmn_Timer` catches
   `std::exception` and reports it only through `DMN_DEBUG_PRINT`; a
   non-standard exception reaches the `Dmn_Proc` default policy and terminates
   the process.
4. `Dmn_Timer::stop()` relies on cooperative pthread cancellation and may wait
   for the sleep or callback to reach a cancellation point; a callback that
   never reaches one can make stopping block indefinitely. Consider an
   interruptible wait if bounded stop latency is required.
