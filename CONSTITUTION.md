# Dmn Codebase Constitution

This document records the repository's coding, layout, and documentation
conventions for contributors and agentic coding tools. It is derived from the
existing C++ headers, implementations, tests, build files, and
`scripts/run-tab-check-and-clang-format.sh`.

These rules guide new and modified code. They do not authorize broad
reformatting or unrelated cleanup of existing files. Preserve established local
patterns where they do not conflict with correctness or an explicit task
requirement.

## 1. Priority of instructions

1. Follow the user's explicit requirements and the repository's build/test
   constraints.
2. Preserve existing behavior and public API unless the task calls for a
   behavior change.
3. Follow this constitution and the relevant module specification.
4. Match nearby code where this document does not define a convention.

Treat existing code as evidence of convention, not proof that every existing
pattern is correct. If a local pattern conflicts with correctness, type safety,
or a documented contract, prefer the correct and documented behavior and
explain the necessary deviation.

## 2. Repository layout

- `include/` contains public C++ headers and header-only/template
  implementations. Put non-template implementations in `src/` where practical.
- `src/` contains non-template library implementations and executable entry
  points. Kafka-specific code belongs in `src/kafka/`; its public headers
  belong in `include/kafka/`.
- `src/proto/` contains protobuf schemas. Generated protobuf files belong in
  the build tree, not as hand-maintained source.
- `test/` contains standalone GoogleTest executables, conventionally named
  `dmn-test-<component>[-variant].cpp`.
- `docs/specs/` contains code-derived specifications and separately labeled
  design proposals. Keep statements about current behavior distinct from
  recommendations and future design.
- Keep deprecated or unsupported components clearly segregated under
  `include/deprecated/` or `test/deprecated/`; do not imply that they are part
  of the supported public API without build and test evidence.

Use descriptive lowercase hyphen-separated filenames, such as
`dmn-runtime-state.hpp` and `dmn-runtime-state.cpp`. Keep paired header and
implementation basenames consistent.

## 3. C++ language and namespaces

- The project uses C++23, configured in the root `CMakeLists.txt`.
- Put public library declarations and definitions in `namespace dmn`.
- Put implementation-only reusable helpers in `namespace dmn::detail` (or a
  nested `detail` namespace). Do not expose internal helpers as public API
  without a clear need.
- Prefer the existing delegation/composition approach and small focused
  abstractions over adding inheritance layers solely for code reuse.
- Reuse established project types, helpers, and ownership patterns before
  introducing parallel mechanisms.

## 4. Naming

- Public class and struct names use the project prefix and form, commonly
  `Dmn_` followed by PascalCase, for example `Dmn_Async` and
  `Dmn_Runtime_State`.
- Enum types use the same type naming style. Enum values commonly use `k`
  followed by PascalCase, for example `kReady` and `kInvalidRange`.
- New functions and methods use lower camelCase, such as `addExecTask()` and
  `waitForEmpty()`. Preserve existing legacy names and public API spellings.
- Non-static class, struct, and nested-type data members use the `m_` prefix
  followed by a descriptive name, commonly lower snake_case: `m_name`,
  `m_topic_running_counter`. Apply this to private, protected, and public
  stored attributes; static class state follows the existing `s_` convention.
  Do not use either prefix for local variables or parameters.
- Local variables and function parameters generally use descriptive
  lower camelCase (`priorityEvaluator`, `onStateChange`, `timeout`). Keep
  existing protocol, generated-code, or externally specified spellings when
  required.
- Compile-time macros use uppercase names with underscores. Project macros
  are commonly prefixed with `DMN_`.
- Keep fault-injection seams visibly distinct from normal implementation
  helpers. A private method or function used only to inject a failure should
  include `ForFaultInjection` in its name (for example,
  `createOutputForFaultInjection`). Prefer named libfiu failure points for
  injection embedded in normal code paths; do not add test-only methods to the
  public API. Guard injection-only code with `FIU_ENABLE`.
- Test suites and test names use descriptive PascalCase components, e.g.
  `TEST(DmnUtilTest, StringCompareUsesUnicodeCaseFolding)`.

Do not rename public API, protobuf fields, or wire-level identifiers just to
make them match a naming preference; compatibility takes precedence.

## 5. Formatting and whitespace

### Formatter

`scripts/run-tab-check-and-clang-format.sh` is the repository's formatter
workflow. It checks for leading tab characters and invokes:

```sh
clang-format -i --style=LLVM <file>
```

The script enumerates `.cpp` and `.hpp` files in its configured source
directories. There is no committed `.clang-format` file, so do not describe
`-style=file` as the repository's configured style. When formatting a focused
change, use the same LLVM style:

```sh
clang-format -i --style=LLVM path/to/file.hpp path/to/file.cpp
```

Do not run a formatter over the whole repository for a localized change:
format only touched C++ files, and inspect the resulting diff for unrelated
changes. Avoid introducing leading tabs. Use spaces for indentation.

### Blank lines and readable segments

- Use an empty line to separate logical segments: include groups, a function
  from the next function, a declaration from an unrelated declaration, and
  distinct steps or phases within a longer function.
- Separate adjacent, independent control-flow blocks with a blank line when
  they represent different decisions or processing phases. Keep the branches
  of one `if`/`else if`/`else`, a `switch`, or a loop together as one unit;
  do not add blank lines mechanically between each branch or statement.
- When a line ends with `}` closing a block, leave a blank line before the
  next statement at the same scope. Apply this after control-flow blocks,
  function bodies, and blocks inside lambdas. Keep parts of one construct
  together: do not put a blank line between an `if` and its `else`/`else if`,
  or between a `try` and its `catch` clauses.
- If the closing `}` is followed by a comma as part of an argument list, keep
  the arguments together without adding a blank line between them.
- In longer functions, use blank lines to make phase transitions visible
  (for example, validation, state changes, and notification/return). Keep
  tightly coupled statements together when splitting them would obscure their
  relationship.
- In class definitions, visually separate access sections and meaningful
  groups of members/functions with blank lines. Keep closely related
  declarations together.
- Separate paragraphs in documentation with a blank comment line.
- In Doxygen blocks, put a blank comment line between an `@brief` description
  and following tags such as `@param`, `@return`, or `@throws`.
- Do not insert blank lines mechanically between every statement or every
  member. The goal is visible logical grouping, not extra vertical space.
- In CMake files, use blank lines to distinguish meaningful configuration
  phases and command groups, such as dependency/tool discovery, path or target
  setup, and custom-command definitions. Keep commands that form one operation
  together—for example, a target declaration with its immediately related
  properties or dependencies—and avoid adding blank lines between every
  adjacent CMake command.
- Separate a standalone `return` from preceding executable statements with an
  empty line only when those statements are at the same block level. Do not
  add a blank line when the return is the first statement in its block, such as
  `if (condition) { return false; }`. If an explanatory comment immediately
  precedes a return that is separated from prior statements, keep the comment
  attached and put the empty line before the comment. Inline return
  expressions are not split solely to add vertical spacing.
- Separate a standalone `throw` from preceding executable statements with an
  empty line only when those statements are at the same block level. Do not
  add a blank line when the throw is the only statement in its block, such as
  `if (condition) { throw error; }`. If an explanatory comment immediately
  precedes a throw that is separated from prior statements, keep the comment
  attached and put the empty line before the comment. Keep a throw expression
  together; do not split it solely to add vertical spacing.
- Keep braces, indentation, continuation alignment, and other whitespace
  consistent with clang-format's LLVM output.

### Includes and guards

- Public headers use the `#ifndef FILENAME_HPP_` / `#define FILENAME_HPP_`
  include-guard form and close with a matching `#endif` comment.
- Include the header's direct dependencies explicitly; each supported public
  header should compile as a standalone include.
- In a `.cpp`, include its paired public header first where applicable, then
  required project and system/dependency headers in logical groups, matching
  the nearby file's include-order pattern.
- Remove unused includes from code you modify, but do not reorder unrelated
  include lists solely for cosmetic consistency.

## 6. Class and function structure

- Keep declarations and related implementation easy to locate. Header-only
  templates and small inline helpers may be implemented beside their
  declarations; non-template method bodies generally belong in the paired
  `.cpp`.
- Within a class or struct, declare or define member functions before
  non-static data members, regardless of access level. Treat callable methods
  as the type's interface and stored data as implementation details; keep the
  state declarations after the methods, including private methods and state.
  This is a source-layout convention and does not change access control.
- Preserve required data-member declaration order when applying this layout:
  C++ initializes members in declaration order and destroys them in reverse
  order, regardless of the constructor's initializer-list order.
- Use `public`, `protected`, and `private` access sections intentionally.
  Keep implementation state private unless derived classes genuinely require
  access.
- Delete copy/move operations explicitly for objects whose synchronization
  state, ownership, or thread identity must not be duplicated. Add move
  support only when its ownership semantics are defined.
- Mark destructors `noexcept` where the class contract requires it, and ensure
  cleanup behavior is safe under that guarantee.
- Prefer standard library facilities, RAII, explicit ownership, and proper
  constraints/types over raw resource management or unchecked casts.
- Use `const`, references, `std::string_view`, smart pointers, and forwarding
  references according to ownership and lifetime semantics. Do not retain a
  view/reference beyond the lifetime guaranteed by its contract.
- Keep templates constrained when the API only supports a defined type
  category. State meaningful template requirements in Doxygen and tests.

The repository uses both conventional return-type placement and trailing
return types (`auto f() -> T`) in existing code. Follow the local declaration
pattern; do not perform signature-style churn unrelated to the task.

## 7. Documentation and comments

- Public headers begin with the repository copyright notice, `@file`, and
  `@brief`. Add an overview or focused sections when they clarify a component's
  responsibilities, execution model, thread safety, ownership, or failure
  behavior.
- Document public functions and methods in Doxygen style. Include relevant
  `@param`, `@return`, `@throws`, and concurrency/lifetime constraints.
- Add blank Doxygen comment lines between the brief paragraph and subsequent
  tags, and between separate prose paragraphs.
- Keep comments accurate to executable behavior. Update directly related
  documentation whenever behavior, error handling, ownership, or API contracts
  change. Code is the source of truth for current behavior; label future
  requirements as proposals rather than implemented guarantees.
- Prefer comments that explain contracts, rationale, non-obvious behavior, or
  constraints. Do not narrate an obvious statement or duplicate the code.
- Use standard terminology and proofread spelling and grammar in comments
  touched by a change.

## 8. Errors, resources, and concurrency

- Surface failures through the API's established error channel: exceptions,
  return values, futures, callbacks, or explicit status. Do not silently
  discard errors or return success-shaped fallbacks.
- Catch exceptions only where the component contract requires recovery or a
  thread/destructor boundary requires containment. Do not add broad catches
  that hide failures without reporting or recording them.
- Use RAII for locks, file descriptors, memory, thread joins, and in-flight
  operations. Make ownership transfer and cleanup order explicit.
- Define the synchronization domain of shared state. A queue or serialized
  executor protects only the operations routed through it; do not claim it
  protects unrelated caller access.
- For destruction and shutdown, prevent new operations before draining or
  releasing resources. Document required external coordination when callers
  cannot safely race methods with object destruction.
- Do not perform blocking work in signal context. Respect the repository's
  runtime-thread and callback execution constraints.

## 9. Tests and build integration

- Add or update focused GoogleTest cases for observable behavior, edge cases,
  failure paths, and concurrency contracts affected by a change.
- Prefer deterministic tests with explicit synchronization over sleep-based
  timing. Do not rely on test execution order or shared process state.
- Register new test executables in `test/CMakeLists.txt` using the existing
  `ADD_TEST_EXECUTABLE` convention. Kafka integration tests remain conditional
  on `BUILD_KAFKA_TEST`.
- Public headers should be covered by the `dmn-standalone-header-check`
  target, which compiles supported headers independently. Generated and
  deprecated headers may have different build constraints; state exclusions
  explicitly rather than counting them as passing checks.
- Use the smallest relevant build/test command first, then expand validation
  when failures or cross-module effects require it.
- Project targets use `-Wall -Wextra -Wpedantic`; do not introduce avoidable
  warnings.

## 10. Agent workflow

Before editing:

1. Read the target code and its callers/tests/specification.
2. Check the worktree and preserve unrelated staged and unstaged changes.
3. Identify public API, ownership, and behavior consequences before changing
   implementation.

While editing:

1. Make focused, complete changes; avoid speculative abstractions and
   unrelated cleanup.
2. Keep code, tests, and current-behavior documentation synchronized.
3. Use existing helpers, naming, and project patterns unless they conflict
   with a correctness requirement.

Before reporting completion:

1. Format touched C++ files with the repository LLVM clang-format workflow.
2. Run `git diff --check`.
3. Build and run the smallest test set that exercises the change.
4. State what changed and disclose meaningful validation limits or failures.

Never claim tests, formatting, or a complete codebase review that was not
actually performed. Do not discard or rewrite pre-existing user changes.
