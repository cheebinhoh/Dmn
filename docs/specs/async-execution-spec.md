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
unknown. Copy and move are disabled.

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
the new duration and optional replacement callback. `stop()` requests
cancellation and joins. Callback exceptions derived from `std::exception` are
printed only through the debug macro; other exception types are not handled.

## Thread-safety and lifetime requirements

The async queue provides serialized callback execution, but does not make
captured application state safe to access from other threads. A caller must
manage captured-object lifetime and synchronization. `Dmn_Proc` does not
currently provide synchronized concurrent lifecycle calls. A thread must not
outlive the object whose members its task accesses.

## Gaps / improvements

1. Synchronize `Dmn_Proc` state transitions and task/handle access or explicitly
   restrict lifecycle methods to one controlling thread. Ensure exceptions
   cannot escape the pthread entry point; expose a failure channel to joiners.
2. Delayed `Dmn_Async` tasks are re-enqueued while waiting for their due time,
   so the serialized worker can poll continuously and defer unrelated work.
   Replace this with a timer queue or a wait that permits ready work to run.
3. Define behavior for negative/overflowing durations and check the return from
   `Dmn_Proc::exec()` in timer startup.
4. Catch and report all timer callback exceptions, or state a strict
   non-throwing callback requirement. Current handling catches only
   `std::exception`.
5. `Dmn_Timer::stop()` may wait for the current sleep/callback cancellation
   point; document maximum stop latency or make it interruptible.
6. Add tests for concurrent `exec`/`wait`/`stopExec`, task exceptions,
   non-standard timer exceptions, and delayed-work CPU usage. Avoid asserting
   race-sensitive timing without generous deadline tolerances.
