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
Finalization occurs as soon as end is selected, including during the same
`runNext()` call. An empty machine initializes and finalizes without a user
callback. Repeated execution after finalization is outside the documented
precondition.

The class has lifecycle hooks (`beforeSetStateFnc`, `beforeSetNext`,
`beforeSetEnd`, and `beforeRunNext`) for derived guards. It is not internally
thread-safe.

## Gaps / improvements

1. Decide whether calling `runNext()` after finalization should throw or return
   false in non-assert builds; the current assertion and fallback differ by
   build configuration.
2. Validate empty callbacks at registration or document that invoking one
   raises `std::bad_function_call`.
3. Specify exception behavior for initialization, state callbacks, and
   finalization. The current implementation propagates exceptions and does not
   roll back machine state.
4. Enforce or clarify the no-concurrent-configuration rule; vector mutation
   during callback execution can invalidate the callback reference.
5. Add tests for empty machine, explicit backward jumps, replacement,
   end-before-start, callback exceptions, and behavior after finalization.
