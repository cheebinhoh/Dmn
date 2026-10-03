# Runtime scheduler and coroutine task

**Status:** Code-derived specification of `Dmn_Runtime_Manager`,
`Dmn_Runtime_Task`, signal handling, and timed jobs. Runtime-managed state
machines are covered by `../specs/runtime-state-machine-spec.md`.

## Scope and lifecycle

`Dmn_Runtime_Manager<QueueType>` is a singleton that privately owns a
`Dmn_Async` context, three priority queues, a timed-job min-heap, a signal-wait
thread, and a platform timer implementation. Its signals are blocked by
`runPriorToCreateInstance()` before singleton/thread creation; callers must
create the runtime through a path that performs this pre-initialization before
other worker threads are started.

`enterMainLoop()` starts the `sigwait()` thread and blocks the calling thread.
`exitMainLoop()` sets exit state, arms a short wake timer, joins the signal
waiter, and disables the timer. Default SIGINT and SIGTERM hooks request loop
exit. External signal hooks execute before internal hooks in the runtime async
context. Register/clear operations are enqueued asynchronously.

The runtime serializes job callbacks on one async context. Immediate jobs
accept callbacks returning `void` or `Dmn_Runtime_Task`; `void` work is wrapped
as a coroutine. High, medium, then low priority queues are selected by the
executor. Timed jobs are ordered by absolute `steady_clock` due time and
transferred to an immediate priority queue when the timer signal is handled.
The scheduler drives a coroutine until it completes or suspends; the current
executor immediately schedules another step for suspended work.

## Job contract

`Dmn_Runtime_Job` carries priority, coroutine-returning callable, absolute due
time, and optional error callback. The error callback receives an
`exception_ptr&`. Exceptions thrown while creating/running a task are passed to
the callback where possible; callback exceptions are ignored by the scheduler.
Priorities `kSched` and unknown values are not valid inputs to public immediate
job scheduling.

`Dmn_Runtime_Task` owns a coroutine handle, starts suspended, captures body
exceptions, and destroys its frame when the wrapper is destroyed. Its
compatibility `co_await` path installs a continuation. It is designed for the
runtime scheduler, not as an independently scheduled or general-purpose
coroutine task.

## Time and signal assumptions

Timed jobs use `steady_clock` for deadlines; timestamps in message schemas are
separate wall-clock values. The platform-specific timer uses a POSIX monotonic
timer where available and `setitimer(ITIMER_REAL)` otherwise. Signals must stay
blocked in worker threads so the dedicated waiter can consume them.

## Gaps / improvements

1. Document and test the required call order for
   `runPriorToCreateInstance()` and singleton creation. A constructor cannot
   repair signals already inherited by earlier threads.
2. Define scheduler fairness/starvation. High-priority traffic can defer lower
   priorities; suspended coroutines are resumed immediately rather than
   awaiting an event, so the yield model is not currently a nonblocking
   coroutine scheduler.
3. Protect or constrain concurrent `enterMainLoop()`/`exitMainLoop()` and
   signal-hook lifecycle operations. Test repeated loop entry, exit before
   entry, and shutdown while timed/immediate jobs are pending.
4. Ensure runtime job accounting remains correct when task creation, error
   callbacks, queue operations, or coroutine destruction throw. Provide
   observable failure reporting rather than silently ignoring error-handler
   failures.
5. Check and report timer implementation errors and define behavior on
   non-POSIX fallback platforms. Test equal deadlines, past deadlines, large
   durations, and timer shutdown.
6. Document that a coroutine suspension is immediately resumed by the current
   scheduler, or redesign awaitable semantics before exposing this as a
   general-purpose coroutine runtime.
