# Utility and public-header surface

**Status:** Code-derived reference for debugging macros, small utilities, and
the umbrella include.

## Debug macro

`DMN_DEBUG_PRINT(statement)` evaluates the statement in non-`NDEBUG` builds
inside a `do/while(false)` wrapper. With `NDEBUG`, the statement is not
evaluated. It includes `<iostream>` for the expected stream usage. Code must
not rely on the argument's side effects.

## General helpers

`incrementByOne(value)` computes `max(1, value + 1)`. It is intended for
integer-like types. Signed maximum overflow is undefined; unsigned wrap to zero
is converted to one.

`stringCompare(lhs, rhs, caseInsensitive=true)` copies both views into
`std::string`, optionally lowercases bytes with `::tolower`, then compares.
This is not Unicode case folding, and passing a negative signed `char` to
`tolower` is undefined unless converted to `unsigned char`.

`ScopeGuard<F>` invokes its stored callable unconditionally on destruction.
There is no dismiss/release operation, and a throwing callable terminates
because the destructor is `noexcept`.

## Umbrella header

`include/dmn.hpp` re-exports the common queue, async, DMesg, I/O, runtime,
socket, state, timer, interval-tree, and Kafka headers. It does not itself add
symbols. Consumers needing a narrow dependency surface should include component
headers directly.

## Gaps / improvements

1. Convert each input byte to `unsigned char` before `tolower`; specify whether
   the intended comparison is ASCII-only, locale-based, or Unicode-aware.
2. Constrain `incrementByOne` to supported integer types and provide explicit
   overflow semantics for signed types.
3. Make `ScopeGuard` move-only and add a release operation, or replace it with
   the standard scope-exit facility where available. Require a non-throwing
   cleanup callable.
4. Decide whether `dmn.hpp` intentionally omits `dmn-dlock.hpp` and
   `dmn-runtime-state.hpp`; document opt-in headers or include them in the
   umbrella consistently.
5. Add standalone-header compile tests to detect missing direct includes and
   guarantee each public header is self-contained.
