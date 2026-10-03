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

An already-admitted `pop(count, timeout)` wakes when shutdown begins and drains
up to `count` items currently queued, returning a partial batch or an empty
vector if no items remain. Calls that begin after shutdown are rejected.

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

`waitForEmpty()` observes an atomic outstanding-item count rather than
traversing queue nodes. The count includes published items and pushes currently
being published; it is decremented on dequeue or if a push fails. The method
holds an in-flight ticket for teardown coordination and returns when the count
reaches zero or shutdown begins. It returns the cumulative successful push
count. Producers still require external coordination: callers must stop
producers before calling `waitForEmpty()` and keep them stopped until it
returns. Concurrent consumers are safe.

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
- Stop and coordinate all producers before calling LF `waitForEmpty()`; it
  waits for consumers to remove outstanding items but does not prevent later
  producers from enqueueing new items after it returns.
- Queue element construction/destruction and moves occur as part of queue
  operations and may throw according to `T`'s behavior.
- Do not infer FIFO order across multiple consumers from the order in which
  concurrently returned values are processed by callers.

## Lock-free queue stress coverage

`dmn-test-blockingqueue-lf-stress` exercises repeated epoch-bucket reuse,
concurrent push/pop racing with shutdown, throwing element copy/move operations,
and multiple blocking consumers interrupted by shutdown. It can be run under
AddressSanitizer and UndefinedBehaviorSanitizer with a separate build:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan --target dmn-test-blockingqueue-lf-stress
ctest --test-dir build-asan -R '^dmn-test-blockingqueue-lf-stress$' \
  --output-on-failure
```

ThreadSanitizer requires a separate build using `-fsanitize=thread` in both
compiler and linker flags; it cannot be combined with AddressSanitizer.

## Gaps / improvements

1. The deprecated bounded queue refers to the old one-parameter queue base and
   old push/pop signatures. Keep it outside public support until repaired and
   independently built/tested, or remove it.
