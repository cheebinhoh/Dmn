# Utility and public-header surface

**Status:** Code-derived reference for debugging macros, small utilities, and
the umbrella include.

## Debug macro

`DMN_DEBUG_PRINT(statement)` evaluates the statement in non-`NDEBUG` builds
inside a `do/while(false)` wrapper. With `NDEBUG`, the statement is not
evaluated. It includes `<iostream>` for the expected stream usage. Code must
not rely on the argument's side effects.

## General helpers

`incrementByOne(value)` accepts integral types other than `bool` and computes
`max(1, value + 1)`. Signed values saturate at their maximum to avoid overflow;
unsigned maximum wraps to zero and the lower bound converts the result to one.

`stringCompare(lhs, rhs, caseInsensitive=true)` uses ICU to decode UTF-8,
apply locale-independent Unicode default case folding, canonically decompose
the results, and compare them. Case-insensitive comparison throws
`std::invalid_argument` for malformed UTF-8. With `caseInsensitive=false`, it
compares bytes directly and does not validate UTF-8. Locale-specific casing
rules are not applied.

`ScopeGuard<F>` is move-only and invokes its stored callable on destruction
unless `release()` is called. Moving transfers the cleanup responsibility and
disarms the source. The callable must be nothrow-invocable; throwing cleanup
callables are rejected at compile time.

## Umbrella header

`include/dmn.hpp` re-exports the common queue, async, DMesg, I/O, runtime,
socket, state, timer, interval-tree, DLock, and Kafka headers, including
`dmn-runtime-state.hpp`. It does not itself add symbols. Consumers needing a
narrow dependency surface should include component headers directly.

The `dmn-standalone-header-check` test compiles each supported public header
as an independent translation unit. The deprecated headers are excluded; they
still reference the pre-CRTP blocking-queue API and do not compile against the
current queue interface.
