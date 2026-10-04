# Caller-driven state machine

**Status:** Code-derived contract for `Dmn_State`. The asynchronous wrapper
`Dmn_Runtime_State` has a dedicated specification in
`runtime-state-machine-spec.md`.

## State model

`Dmn_State` stores 1-based user callbacks. Slot zero is reserved internally.
`setStateFnc(callback, 0)` appends; `setStateFnc(callback, N+1)` also appends;
an index from 1 through N replaces an existing callback. Other indices throw
`std::out_of_range`. Registration is caller-driven and must not occur while a
callback is executing.

The first `runNext()` performs initialization and selects the first user
callback. Each call invokes at most one user callback. The selected callback
remains selected unless it calls `setNext()`, `setNext(index)`, or `setEnd()`.
`setNext()` selects the next sequential state; from the final state it selects
termination. `setNext(index)` accepts an existing user state or N+1 for end.
When `runNext()` processes the end selection, it finalizes the machine and
returns `false`; if a callback selects the end and then throws, finalization
waits until the next `runNext()`. An empty machine initializes and finalizes
without a user callback. Further `runNext()` calls on a base `Dmn_State` return
`false`; derived classes may reject them in `beforeRunNext()`. Calling
`setEnd()` before the first `runNext()` finalizes the machine without
initializing it.
Before initialization, `setNext()` and `setNext(index)` selections are replaced
by initialization's selection of the first user state.

The class has lifecycle hooks (`beforeSetStateFnc`, `beforeSetNext`,
`beforeSetEnd`, and `beforeRunNext`) for derived guards. It is not internally
thread-safe: configure callbacks before execution, and do not configure them
from a state callback or concurrently with `runNext()`. Such callback-time
registration is rejected with `std::logic_error`. Empty callbacks are rejected
with `std::invalid_argument`. Recursive calls to `runNext()` from a state
callback are rejected with `std::logic_error`.

Exceptions from lifecycle guards and state callbacks propagate to the caller.
They do not roll back the selected state or any transition already made by the
callback. If the initialization transition guard throws, initialization
remains pending and a later `runNext()` can retry it. Finalization only updates
the machine's internal flag; it does not invoke a user callback. If a callback
selects the end and then throws, the selection is retained and a later
`runNext()` performs finalization.

Tests cover the empty machine, explicit backward jumps, callback replacement,
end-before-start, initialization and callback exceptions, empty callback
rejection, callback-time registration rejection, and repeated calls after
finalization. They also verify that recursive execution is rejected.
