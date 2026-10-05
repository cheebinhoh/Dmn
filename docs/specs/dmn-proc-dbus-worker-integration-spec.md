# D-Bus endpoint workers using `Dmn_Proc`

**Status:** Implemented in the D-Bus endpoints without changing `Dmn_Proc`.

## 1. Purpose and decision

This specification records the replacement of the worker thread owners in
`Dmn_DbusInput::Impl` and `Dmn_DbusOutput::Impl` with `Dmn_Proc`, and defines
the cooperative-stop contract needed to preserve endpoint behavior and
resource lifetime.

**The migration is feasible, but `Dmn_Proc` is not a drop-in replacement.**
Its `stopExec()` and destructor use deferred `pthread_cancel`, whereas the
D-Bus endpoints stop cooperatively: they publish a stop flag, wake local
waiters, let bounded libdbus dispatch return, and join before touching the
connection or filter again. Calling `stopExec()` for these workers would
replace that safe protocol with cancellation at a point not controlled by the
endpoint.

The implementation keeps stop ownership in each endpoint, starts a
`Dmn_Proc` task that runs the existing loop, and calls `wait()` only after
setting its atomic stop flag and waking local waits. It does not call
`stopExec()` for D-Bus workers. Cooperative stop is task behavior and does not
require extending `Dmn_Proc` or replacing pthreads.

## 2. Current implementation and guarantees

### D-Bus input worker

`Dmn_DbusInput::Impl` opens its connection, installs a filter and match rule,
then starts a `Dmn_Proc` running `dispatchLoop()`. The loop checks an
endpoint-owned atomic stop flag between calls to
`dbus_connection_read_write_dispatch()`, whose timeout is `kWorkerWait`
(50 ms). The filter catches callback exceptions and records malformed,
oversized, allocation-failure, and queue-drop outcomes.
`shutdown()` is idempotent: it sets the endpoint stop state and atomic flag,
wakes blocked readers, calls `Dmn_Proc::wait()`, then removes the filter and
closes the private connection. Queued payloads remain readable before
shutdown is reported to readers.

The ordering matters: the worker must no longer be dispatching or invoking the
filter before the filter is removed or its `Impl` state and connection are
destroyed. A D-Bus connection must not be closed while its dispatch worker is
using it.

### D-Bus output worker

`Dmn_DbusOutput::Impl` opens its private connection and starts a
`Dmn_Proc` running `sendLoop()`. The loop admits bounded application-queue
items to libdbus, dispatches with the same 50 ms timeout, and records worker
errors. `shutdown()` stops new writes, sets a steady-clock deadline one second
ahead, sets the worker's atomic stop flag, wakes the worker, and calls
`Dmn_Proc::wait()`. The loop may submit queued messages until the deadline;
remaining application-queue items and libdbus outgoing bytes are recorded as
unsent/discarded before closing the connection.

The one-second period is a **drain deadline**, not a deadline for arbitrary
thread cancellation. The worker is expected to leave its loop cooperatively;
the connection is closed only after join.

### `Dmn_Proc`

`Dmn_Proc` wraps one pthread and a stored `std::function<void()>`.
`exec()` returns `false` if `pthread_create` fails. `wait()` joins a running
thread, restores the ready state, and optionally rethrows a captured task
exception. The default exception policy terminates the process for an
uncaught task exception. `stopExec()` sets its cancellation marker, calls
`pthread_cancel`, and joins. `Dmn_Proc` does not provide cooperative stop; a
client task may implement it by capturing caller-owned synchronized state and
checking it while running. The destructor invokes `stopExec()` when its state
is still running and suppresses cleanup exceptions.

Its lifecycle state, task, and pthread handle are not internally synchronized.
Callers must serialize lifecycle operations and must not destroy the object
while its task uses owner state. The state remains `kRunning` after the task
returns until a caller joins it with `wait()`; it therefore represents a
joinable execution, not a precise indication that user code is currently
running.

## 3. Feasibility and compatibility analysis

| Concern | Existing endpoint behavior | `Dmn_Proc` implication |
|---|---|---|
| Stop request | Endpoint-owned stop state, atomic worker flag, and condition-variable notification | Keep the wakeups and check the worker flag in the loop. `Dmn_Proc::wait()` joins the cooperatively exiting task; `stopExec()` remains cancellation-based and is not used. |
| Blocking dispatch | libdbus dispatch is called with a finite 50 ms timeout | A cooperative worker can finish after the call returns. Do not assume libdbus functions are pthread cancellation points or cancellation-safe. |
| Output drain | Worker observes an endpoint deadline and reports pending output | Preserve the deadline check in `sendLoop()`; joining must follow loop completion, not cancel it at the deadline. |
| Join and resource lifetime | The worker join completes before filter removal or connection close | `Dmn_Proc::wait()` provides the join; calls remain serialized by endpoint shutdown. |
| Startup failure | Worker construction failure aborts endpoint construction and releases initialized resources | `Dmn_Proc::exec()` returns `false`; both endpoints translate this to `std::system_error` with `resource_unavailable_try_again`. Input removes its filter and closes its connection; output releases its connection through member cleanup. |
| Worker errors | Input records callback/bus errors; output catches loop errors and records terminal status | Preserve endpoint-level status and wakeups; do not let future worker failures silently stop processing. |
| Destruction | Endpoint explicitly shuts down and joins before member teardown | Explicitly call endpoint shutdown and `wait()`. Never rely on `Dmn_Proc`'s destructor cancellation as normal cleanup. |
| Concurrent shutdown | `std::call_once` serializes each endpoint's shutdown body | Retain this serialization around the `Dmn_Proc` lifecycle calls; `Dmn_Proc` itself does not make concurrent `wait()`/`exec()` safe. |

No behavior requires an asynchronous-cancellation primitive. The task
functions already have a cooperative termination condition, and their loops
already use finite waits. `Dmn_Proc` is used as the start-and-join owner; the
endpoint loops and local wakeups remain the stop policy.

The bounded output-drain behavior must not be described as a hard upper bound
on all shutdown work: the current code bounds the loop's drain phase and uses
finite dispatch waits, but joining still depends on each operation returning
as specified. A future libdbus call that blocks indefinitely would also block
`std::thread::join()` and `Dmn_Proc::wait()`.

## 4. Implemented endpoint behavior

1. Keep each endpoint's connection, queue, stop flag, drain deadline, and
   worker loop owned by its `Impl`. Do not move D-Bus protocol state into
   `Dmn_Proc`.
2. Construct the process wrapper with a task that invokes the endpoint loop.
   Start it only after the connection and required input filter/match setup
   have succeeded.
3. Treat a `false` result from `exec()` as worker-start failure and throw
   `std::system_error` with `resource_unavailable_try_again`. Input
   construction removes its installed filter and closes its connection;
   output construction releases its connection through member cleanup.
   No partially ready endpoint escapes.
4. On shutdown, publish endpoint stop state, set the worker atomic flag,
   notify local waiters, let the worker observe stop/deadline and exit, join
   using `wait()`, and only then remove the input filter or close either
   connection.
5. Do not call `stopExec()` from endpoint shutdown or destructors. Keep the
   endpoint shutdown/destructor contract non-throwing. Worker loops continue
   to report failures through their existing endpoint status/diagnostic paths.
   Any new failure path must update endpoint status and wake blocked callers
   when it occurs, not only when shutdown later joins the task.
6. Keep explicit idempotent shutdown before member destruction. After an
   explicit successful `wait()`, `Dmn_Proc` is ready and its destructor must
   not need to cancel a worker. The member declaration/destruction order must
   also ensure the worker wrapper is destroyed before state accessed by its
   task.
7. Preserve the output worker's one-second drain deadline, unsent counters,
   input queue-draining semantics, and current status/error behavior. Changing
   any of these is a separate behavior change.

## 5. Client-owned cooperative stopping

No `Dmn_Proc` API change is required for cooperative task exit. A client can
capture an atomic or otherwise synchronized stop state in the no-argument
callable passed to `exec()`. The client sets the flag, wakes any blocking
operation, and calls `wait()` to join after the callable returns. The unit
test `DmnProc.ClientFlagLetsTaskExitCooperativelyBeforeWait` demonstrates
this contract. D-Bus follows the same pattern with endpoint-owned atomic
flags.

This approach does not change `stopExec()`; existing callers retain deferred
pthread cancellation. Cooperative stopping remains the task owner's
responsibility: a flag cannot interrupt a task or wake blocking I/O by itself.
The endpoint must maintain its own stop state and wakeup mechanism.

### Pthreads remain suitable

Nothing in this design requires abandoning pthreads. `Dmn_Proc` already uses
`pthread_create`, `pthread_join`, and deferred `pthread_cancel`. A client can
implement a cooperative task protocol on that pthread without changing the
wrapper. Replacing pthreads with `std::thread` would not by itself provide
cooperative cancellation, wake blocked operations, or bound a join.

## 6. Migration and test plan

The migration is implemented for both endpoints. The `Dmn_Proc` client-flag
unit test was added before the endpoint integration; focused D-Bus tests and
a dedicated worker-start fault-injection test cover the endpoint worker
lifecycle.

Required regression coverage:

- Successful input and output startup and normal message transfer.
- Deterministic worker-start failure for both endpoints, verifying
  construction failure and complete connection/filter rollback.
- Input shutdown while `read()` is blocked: the reader wakes, queued messages
  remain readable, and the worker is joined before filter removal/connection
  close.
- Repeated shutdown and concurrent shutdown/delivery, preserving the existing
  idempotence and lifetime guarantees.
- Output shutdown with an empty queue and with queued output; verify the
  existing drain deadline, pending-message/byte counters, and connection close
  after worker completion.
- Output worker failure and bus disconnect: writes/readers receive the
  established error behavior, status is updated, and shutdown still joins and
  releases resources.
- Endpoint destruction without prior explicit shutdown for both worker types.
- Run normal D-Bus tests and D-Bus fault-injection tests with `ENABLE_DBUS=ON`;
  run the regular project test selection to detect changes in `Dmn_Proc`
  behavior. When available, run the relevant memory-check tests.

The tests must prove endpoint-visible behavior and cleanup; merely asserting
that a `Dmn_Proc` task starts or joins does not prove preservation of the
D-Bus connection, filter, queue, or drain contracts.

## 7. Acceptance criteria

- Both endpoints use `Dmn_Proc` without calling `stopExec()` for normal
  shutdown.
- Worker-start failure is explicit and rolls back all partially initialized
  endpoint resources.
- Shutdown requests cooperative stop, wakes local waits, joins the worker,
  and only then releases libdbus resources.
- The input queue-drain contract, output drain deadline and counters, worker
  failure reporting, and public APIs remain unchanged.
- No task can outlive the endpoint state it accesses, and concurrent endpoint
  shutdown remains serialized.
- Tests cover startup failure, shutdown races, queued output, worker failure,
  and destruction, with no dependence on undocumented pthread cancellation
  behavior inside libdbus.
