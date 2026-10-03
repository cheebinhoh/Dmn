# Blocking queues and shutdown safety

**Status:** Code-derived specification of the queue abstraction, mutex and
lock-free implementations, and in-flight operation guard.

## Module map

- `Dmn_BlockingQueue<Derived,T>` is a CRTP-facing API.
- `Dmn_BlockingQueue_Mt<T>` stores values in a mutex-protected `std::deque`.
- `Dmn_BlockingQueue_Lf<T>` is an MPMC Michael–Scott linked queue with
  active-wait pop behavior and epoch-based node reclamation.
- `Dmn_Inflight_Guard<T>` issues RAII tickets to prevent teardown/reclamation
  from racing with operations already admitted.
- `Dmn_Limit_BlockingQueue<T>` is deprecated and is not a supported variant of
  the current queue API.

## Shared queue contract

`push(const T&)` copies; `push(T&&)` forwards as a move. `pop()` waits for one
item and throws if shutdown interrupts it. `popNoWait()` returns an empty
optional when no item is available. `pop(count, timeout)` is an implementation
virtual: callers must consult the selected queue contract for timeout units and
partial-result behavior. `waitForEmpty()` waits until queued items have been
removed and returns a cumulative count. The abstract queue's `shutdown()` sets
the base shutdown flag; each concrete queue must wake/block/reject operations
appropriately.

### Mutex queue

The queue is unbounded and protects data/counters with one mutex. Pushes append
and notify a waiter. Single-item pop can block; `popNoWait()` returns
immediately. Bulk pop takes up to `count`; a positive timeout is in
microseconds and permits a partial vector when at least one item is available
at timeout. A zero timeout waits until the requested count is available.
Shutdown wakes waiters, prevents new in-flight operations, and waits for
previously admitted operations to leave. `waitForEmpty()` returns total items
popped after the queue is empty.

### Lock-free queue

The live FIFO is a linked list with a dummy head node and atomic head/tail
pointers. Push/pop follow the Michael–Scott helping pattern. The public
blocking pop actively retries and yields rather than sleeping on a condition
variable. Bulk pop returns up to `count`, bounded by a positive microsecond
timeout; zero waits indefinitely unless shutdown begins. Shutdown nulls the
tail, rejects future pushes, allows waiters to leave, and waits for in-flight
operations.

Removed head nodes are retired to an epoch bucket associated with the caller's
in-flight ticket and reclaimed only when that bucket is inactive. The epoch
table has 50 buckets; the current epoch advances based on operation counts,
not wall time. This is internal reclamation state and is not an application
visible epoch.

### In-flight guard

`enterInflightGate()` returns a unique ticket or throws if a derived object's
closed predicate is true. The ticket increments the active count, obtains an
optional per-operation value, and calls the matching leave hook at destruction.
`waitForEmptyInflight()` blocks until the count reaches zero. Derived
implementations must close admission before waiting; the guard does not itself
provide a shutdown flag.

## Required invariants and caller expectations

- Do not destroy a queue while another thread may newly call its API; shutdown
  must close admission and drain existing operations first.
- LF `waitForEmpty()` is a spin/yield observation, not a barrier against
  concurrent producers; externally coordinate producers before using it as a
  drain guarantee.
- Queue element construction/destruction and moves occur as part of queue
  operations and may throw according to `T`'s behavior.
- Do not infer FIFO order across multiple consumers from the order in which
  concurrently returned values are processed by callers.

## Gaps / improvements

1. `Dmn_BlockingQueue` comments describe the bulk timeout as milliseconds,
   while the mutex and lock-free implementations interpret it as
   microseconds. Standardize the API unit and update every caller/comment.
2. `Dmn_BlockingQueue_Mt::pop(count, timeout)` wakes on the requested count or
   shutdown; on shutdown it can drain a partial queue. Specify whether that is
   intended and test it explicitly.
3. LF `waitForEmpty()` is not protected by an in-flight ticket, as its comment
   warns. Define a safe external drain protocol or add a coordinated drain API.
4. Add stress/sanitizer coverage for epoch wrap/reuse, shutdown racing with
   push/pop, throwing element operations, and multiple simultaneous waiters.
5. The deprecated bounded queue refers to the old one-parameter queue base and
   old push/pop signatures. Keep it outside public support until repaired and
   independently built/tested, or remove it.
