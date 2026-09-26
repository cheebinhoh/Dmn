# Implementation Plan: `Dmn_IntervalBTree` (Template Interval B‑Tree)

## Current implementation status

Implemented and verified:
- Layer 0: template construction smoke test.
- Layer 1: range validity and inclusive overlap semantics.
- Layer 2A: vector-backed insertion, invalid-range rejection, `empty()`, and
  `size()`.
- Layer 2B: default/custom canonical range ordering, stable duplicate ordering,
  and caller-defined ordering for identical ranges.
- Layer 3: arbitrary copyable payloads, copy/move insertion, and storage of
  move-only payloads.
- Layer 4: uniquely owned root-leaf node storage with public behavior
  preserved.
- Layer 5: degree-2 B-tree insertion, child/root splitting, sorted traversal,
  and structural invariant coverage.
- Layer 6: unpruned overlap existence, copy-returning result, and
  const-reference visitor queries.

Layer 7 is implemented and verified: pure topology classification,
hypothetical `queryTopology()`, priority evaluation, insertion-state
calculation, canonical synchronous notifications for changed existing
entries, exception-safe callback dispatch, and reentrant-mutation rejection.

The focused `dmn-test-interval-btree` target and its registered tests pass for
the completed increments. Layer 8 is implemented and verified: exact and
batch removal, degree-2 deletion balancing, net state notifications, and
reusable clear, including mixed-order structural stress, move-only payload
removal, callback ordering, exception recovery, and reentrancy guards.
Layer 9 is implemented and verified: canonical move extraction, callback
registration/reconnection, and transactional reconstruction.
Layer 10 is implemented and verified: incrementally maintained
`m_subtreeMaxEnd` and pruned overlap traversal, checked against a linear
canonical baseline. Layer 11's interval-tree lifecycle tests pass, but direct
consumer integration is blocked until a `Dmn_DLock` production module exists.

## 1. Delivery rule

Implement `Dmn_IntervalBTree` in small, independently buildable, test‑driven increments:

1. introduce one focused failing test;
2. if useful, temporarily add API declarations and stub definitions to move
   from a compile-time failure to a runtime test failure;
3. replace temporary stubs with the smallest real implementation needed to
   satisfy the test;
4. build only the affected target and run its completed tests;
5. run `git diff --check`;
6. do not commit or finish an increment with placeholder success methods or
   uncompilable future tests.

Create tests only in layers with compilable production code. Temporary stubs
are permitted only while driving a focused test from a compile-time failure to
a runtime failure; remove them before completing or committing the increment.

---

## 2. Target architecture

Follow the repository's C++ data-member naming convention: prefix class and
struct data members with `m_` (for example, `m_start`, `m_entries`, and
`m_nextOrdinal`). Do not apply this prefix to local variables, function
parameters, type aliases, or protocol field names.

```text
application / Dmn_DLock / other DMN components
       |
  Dmn_IntervalBTree<T> (template)
       |
balanced B-tree nodes (internal)
       |
canonical enumeration / overlap queries
```

Key properties:
- template type T is arbitrary;
- pure in-memory structure;
- canonical ordering independent of tree shape;
- deterministic overlap queries.

## 3. Build and test commands
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dmn-test-interval-btree
ctest --test-dir build -R 'dmn-test-interval-btree' --output-on-failure
git diff --check
```

## 4. Layer 0: template construction smoke test

1. Add `include/dmn-interval-btree.hpp` with the `Dmn_IntervalBTree<T>`
   class template and a usable default constructor. Keep the template
   definition in the public header; do not move it into a `.cpp` file.
2. Register `dmn-test-interval-btree` in CMake and add the
   `IntervalBTreeDefaultConstructs` test, which instantiates
   `Dmn_IntervalBTree<int>`.
3. Build the focused test target and run the test.

This is a compile-and-construction smoke test only. It does not claim that
the tree stores entries or implements any other operation. Do not add
placeholder methods that report fabricated success or fixed results.

Exit: the public template header is included by a test and its default
constructor can be instantiated.

## 5. Layer 1: range semantics
Implementation

Implement Dmn_IntervalRange with:
- isValid()
- overlaps()
- boundary comparisons that do not compute `start - 1` or `end + 1`.

Tests
- IntervalRangeRejectsReversed
- IntervalRangeOverlapRejectsInvalidOperands
- IntervalRangeAcceptsSinglePoint
- IntervalRangeAcceptsNegativeValues
- IntervalRangeInclusiveSharedEndpointConflicts
- IntervalRangeAdjacentRangesDoNotConflict
- IntervalRangeHandlesInt64BoundariesWithoutOverflow

Exit: range semantics complete.

## 6. Layer 2: API skeleton and canonical comparator
Implementation

### 2A. Add and size behavior

Begin with the `IntervalBTreeStoresOneEntry` test. It should verify an empty
tree starts empty with size zero, then that adding one valid range/value
succeeds and updates `empty()` and `size()`.

Use this test-first sequence:

1. Add the focused test before the API. Build the test target and confirm the
   expected compile-time failure because the methods do not exist yet.
2. Add declarations and temporary stub definitions for `add`, `empty`, and
   `size` in the public header. Build and run the test; it should compile and
   fail at runtime because the stubs do not implement storage.
3. Replace all temporary stubs with real vector-backed storage and working
   implementations of the three methods. Build and run the focused test;
   it must pass.
4. Add `IntervalBTreeRejectsInvalidInsertionWithoutMutation` as a separate
   test. Verify that adding a reversed range returns `false` and leaves the
   tree's observable contents unchanged (`empty()` and `size()` at this stage).

The temporary stubs are a local RED-stage technique, not a finished
implementation: do not commit or end the increment with them. Keep all
template definitions in `dmn-interval-btree.hpp` so callers can instantiate
the template.

### 2B. Canonical ordering

Keep the vector storage from 2A. Implement enumeration over a temporary view;
do not reorder the stored vector. Each increment below adds a focused test
before the corresponding implementation change.

#### 2B.1 Default ordering

Add `enumerateCanonical()` returning
`std::vector<std::pair<range_type, value_type>>`. With no custom comparator,
sort by ascending `range.m_start`, then ascending `range.m_end`. Payload values
must not participate in this ordering.

Test: `IntervalBTreeCanonicalOrderSimple`, inserting distinct ranges in an
order different from the expected enumeration.

#### 2B.2 Stable duplicate ordering

Add a monotonically increasing insertion ordinal to each stored entry.
Identical ranges enumerate by ordinal when no duplicate callback is supplied.
Invalid insertion must not consume an ordinal. Preserve insertion order in
storage so enumeration does not mutate the tree.

Test: `IntervalBTreeDuplicateRangesUseStableTieBreak` asserts that identical
ranges retain insertion order when no duplicate callback is supplied. Use
this single test for the ordinal guarantee; the separate
`IntervalBTreeCanonicalOrderStable` name was redundant and is intentionally
not part of the test matrix.

#### 2B.3 Caller ordering for identical ranges

Add an optional duplicate-order callback to `enumerateCanonical()`. Invoke it
only when ranges are identical; `true` means its first value sorts before its
second. The callback must define a strict weak ordering. For identical ranges,
use insertion ordinal when the callback is empty. Do not use payload ordering
for distinct ranges.

Tests: `DuplicateRangesUseCallerOrdering`,
`DuplicateOrderingCallbackNotCalledForDistinctRanges`, and
`DuplicateOrderingCallbackCannotMutateTreeReentrantly`.

#### 2B.4 Custom range comparator

Add the custom range-comparator constructor. The comparator receives ranges
only, must be deterministic and a strict weak ordering, and must not inspect
payloads. It orders distinct ranges. If it considers distinct ranges
equivalent, break the tie by the default `(m_start, m_end)` order. Identical
ranges are resolved by the duplicate callback or insertion ordinal. Callers
that require matching canonical output across handlers must use equivalent
comparator and duplicate-callback configurations.

Tests: `IntervalBTreeCustomComparatorIsUsed`,
`IntervalBTreeCustomComparatorEquivalentRangesUseDefaultTieBreak`, and
`IntervalBTreeConstructorsUseDefaultAndCustomComparators`.

Existing Layer 2A tests (`IntervalBTreeStoresOneEntry` and
`IntervalBTreeRejectsInvalidInsertionWithoutMutation`) remain regression
coverage; do not duplicate their implementation in this increment.

Exit: basic API and canonical ordering complete

## 7. Layer 3: template payload support
Implementation

Ensure T:
- is stored per entry,
- does not affect ordering,
- supports both copy insertion and move insertion;
- supports move-only payloads for storage;
- is copy-constructible only for copy-returning query APIs.

Tests
- IntervalTreeSupportsStringPayload
- IntervalTreeSupportsStructPayload
- IntervalTreePayloadDoesNotAffectOrdering
- IntervalBTreePayloadDoesNotAffectDefaultRangeOrdering
- IntervalBTreeMoveOnlyPayloadCanBeInserted
- IntervalBTreeInvalidMoveInsertionDoesNotConsumePayload
- IntervalBTreeCopyAndMoveInsertionOverloads

Exit: template payload support complete.

`IntervalBTreeMoveOnlyPayloadCanBeInserted` also verifies that merely
instantiating and using storage operations for a move-only payload does not
instantiate copy-returning enumeration.

State-callback coverage belongs to Layer 7, where callbacks are introduced.
Move enumeration belongs to Layer 9, alongside canonical extraction.

## 8. Layer 4: B-tree node structure and ownership
Implementation

Replace the flat entry vector with a private root leaf node. The root leaf may
temporarily hold more keys than the eventual B-tree maximum; Layer 5 adds
splitting and balancing.

Introduce a private node representation conceptually equivalent to:
```cpp
struct Node {
  std::vector<std::unique_ptr<Entry>> m_entries;
  std::vector<std::unique_ptr<Node>> m_children;
  bool m_isLeaf{true};
};
```

The tree owns `m_root` with `std::unique_ptr`; children own descendants with
`std::unique_ptr`. Do not add parent pointers or `std::shared_ptr`. Preserve
each entry's value, insertion ordinal, and optional callback when keys move
during split, merge, or borrowing.
Use minimum degree two. Structural tests for split/root/child invariants are
introduced with insertion in Layer 5, when those structures are first
produced. Avoid adding public inspection methods; a test-only friend peer may
validate private invariants. Preserve all observable behavior from Layers
2A-3: copy and move payloads, insertion ordinals, comparator configuration,
enumeration, empty, and size.

Tests: run all existing interval-B-tree tests as regression coverage for the
representation change. Add no future-layer structure tests before the
implementation can produce the structures under test.

Exit: node structure complete.

## 9. Layer 5: B-tree insertion
Implementation

Implement:
- descending to leaf using canonical comparator,
- inserting key/value,
- splitting nodes,
- propagating splits upward,
- maintaining sorted keys.

Tests
- IntervalBTreeNodeStoresMultipleKeys
- IntervalBTreeRootSplitCreatesOwnedRoot
- IntervalBTreeNodeChildrenPartitionCorrectly
- IntervalBTreeInsertionMaintainsOrder
- IntervalBTreeInsertionSplitsNodes
- IntervalBTreeInsertionDeterministicAcrossOrders
- IntervalBTreeEnumerationIsIndependentOfNodeSplits
- IntervalBTreeSplitPreservesEntryValuesAndOrdinals

Callback preservation during splits is covered in Layer 7 after per-entry
callbacks are introduced.

Exit: insertion complete.

## 10. Layer 6: overlap queries
Implementation

Implement:
- hasOverlap(range)
- findOverlapping(range)
- `forEachOverlapping(range, visitor)`, visiting matching payloads by const
  reference in canonical order without copying them.

Traversal rules:
- check each key in node for overlap,
- descend into all candidate children; do not prune using metadata yet.

Start with correct traversal without subtree metadata. Invalid queries return
the documented empty/false result and do not invoke visitors. Visitor results
must be in canonical order. Layer 10 adds and tests pruning metadata after
correct unpruned query behavior is established.

Tests
- IntervalBTreeFindOverlappingSingle
- IntervalBTreeFindOverlappingMultipleInCanonicalOrder
- IntervalBTreeFindOverlappingIncludesSharedEndpoints
- IntervalBTreeFindOverlappingRejectsInvalidQuery
- IntervalBTreeHasOverlapRejectsInvalidQuery
- IntervalBTreeHasOverlapFindsAndRejectsNonOverlappingRanges
- IntervalBTreeOverlapVisitorAvoidsPayloadCopies
- IntervalBTreeOverlapVisitorRejectsInvalidQueryWithoutInvocation
- IntervalBTreeOverlapVisitorRequiresCallableVisitor
- IntervalBTreeOverlapVisitorSupportsMoveOnlyPayload

Exit: overlap queries complete.

## 11. Layer 7: topological overlay logic and entry state
Implementation

Deliver this layer in independently passing slices:

1. Implement the pure range-coverage classifier and test empty, single
   overlap, precedence, inclusive boundaries, contiguous coverage, gaps, and
   int64 boundaries. Keep priority and callbacks out of this slice.
2. Add `queryTopology()` using the classifier and overlap results. Test
   invalid-input behavior and that the query is hypothetical and does not
   mutate the tree.
3. Add the priority evaluator and test top, lower-priority, and tied entries,
   including the no-evaluator default.
4. Add insertion state calculation and `addWithTopology()`. Verify its result
   matches `queryTopology()` and the new entry's callback is not invoked for
   initial state.
5. Add callbacks for existing entries, first verifying a single overlapping
   entry, then priority changes and non-adjacent affected entries. Compute all
   transitions before dispatch, and test canonical callback order and
   exception/reentrancy behavior in the later callback-hardening slice.
6. Test move-only `add()` and callback storage only after the Layer 3 payload
   overloads exist.

Introduce Dmn_OverlayTopology, Dmn_TopologyResult, and the priority_evaluator.
Introduce `Dmn_OverlayState`, an optional per-entry state-change callback,
and duplicate ranges remain valid. Add the entry matcher with the removal API
in Layer 8.
Implement:
- invalid-range rejection with no mutation and defined empty results;
- boundary analysis logic comparing a target range against a resolved std::vector of overlaps to output the correct Dmn_OverlayTopology enum.
- explicit topology precedence: Clear, FullyCovered, CoveringExisting,
  OverlaidBoth, then one-sided overlays;
- classify from each entry's level: a new higher-level strict container is
  `CoveringExisting`, while the contained older entry is `FullyCovered`;
- classify exact duplicate ranges as `FullyCovered`, not strict containment;
- classify each stored entry from its own range and insertion level when
  recomputing state after a mutation;
- inclusive endpoint, exact-match, contiguous-coverage, and coverage-gap
  handling;
- add(range, value, callback) and addWithTopology(range, value, callback):
  execute insertion and pipe the new entry through the boundary analyzer.
  The new entry's callback is never invoked for its initial state.
- provide `const T&` and `T&&` overloads for `add`, mirroring
  `Dmn_BlockingQueue::push(const T&)` and `push(T&&)`;
- keep `addWithTopology` on the copyable `const T&` path because its
  copy-returning result contains overlapping payload values;
- plain `add` computes the same insertion state as `addWithTopology`, but
  discards the returned state while still notifying existing entries.
- queryTopology(range, value): runs a standard overlap query and pipes the result through the boundary analyzer.
- priority evaluation loop verifying if the passed value is structurally "on top" of all returned overlaps via the user-supplied priority_evaluator.
- capture the old state of affected existing entries before insertion;
- after insertion, recompute affected entries against their complete overlap
  sets and synchronously notify only entries whose topology or `isTop` changed.
- compute every callback transition before invoking any callback, then invoke
  changed-entry callbacks in canonical order.

Do not limit this to the immediate canonical left/right neighbors. The
initial affected set is all existing entries overlapping the inserted range.
Because callbacks and computed states do not alter an entry's range or
priority, no second-level propagation is required for the current model.
Recomputing all entries overlapping the union of changed ranges is a valid
conservative implementation. This is required because non-adjacent entries
can jointly cover a boundary or provide continuous `FullyCovered` coverage.

Tests
- TopologyReturnsClearWhenEmpty
- TopologyIdentifiesOverlaidLeftAndRight
- TopologyIdentifiesFullyCovered
- TopologyPrecedenceIsDeterministic
- TopologyExactMatchIsFullyCovered
- TopologyIdentifiesCoveringExisting
- TopologyHandlesContiguousCoverageAndCoverageGaps
- TopologyHandlesInt64BoundariesWithoutOverflow
- QueryTopologyIsHypothetical
- TopologyPriorityUsesEvaluatorAndDefaultsToTop
- InvalidTopologyQueryReturnsClearWithoutMutation
- AddWithTopologyMatchesHypotheticalQuery
- NewEntryCallbackIsNotInvokedOnInsertion
- StateCallbackReceivesTopologyTransition
- StateCallbackReceivesPriorityTransition
- MoveOnlyPayloadSupportsStateCallback
- RangeEndpointInsertionOverloadsForwardCallbacks
- InsertionNotifiesNonAdjacentAffectedEntries
- InsertionCallbacksFollowCanonicalOrder
- TopologyUsesInsertionLevelForContainment
- ExactDuplicateInsertionKeepsBothEntriesFullyCovered
- CallbackExceptionLeavesTreeConsistentAndReusable
- ReentrantInsertionFromCallbackIsRejected
- ReentrantMoveEnumerationFromCallbackIsRejected

Exit: topology queries, priorities, insertion states, and existing-entry
callbacks are implemented and tested.

## 12. Layer 8: removal operations and state transitions
Implementation

Proceed from basic mutation behavior to callback edge cases:

1. Implement exact `removeByRange()` with an optional matcher and test success,
   no match, duplicate ranges, invalid input, and no mutation on predicate
   miss.
2. Add `remove()` as the documented alias and test equivalence.
3. Implement `removeAllOverlapping()` and its exact removal count.
4. For each removal form, capture affected states once, mutate, compute net
   transitions, then dispatch callbacks once per surviving entry in canonical
   order. Add tests for non-adjacent effects and batch deduplication.
5. Add callback consistency, exception, and reentrancy tests.
6. Implement and test B-tree deletion balancing (leaf deletion, internal
   replacement, borrowing, merging, root contraction) while preserving all
   prior insertion, enumeration, query, and callback behavior.

Implement:
- remove(range, matcher)
- removeByRange(range, matcher)
- removeAllOverlapping(range)
- B-tree deletion balancing, including borrowing, merging, and root contraction
- exact range matching followed by the optional opaque-value predicate;
- an empty matcher selects the first exact-range entry in deterministic
  traversal order;
- a failed predicate leaves the tree and callback state unchanged;
- return the exact removal count for batch removal;
- batch removal captures state once and notifies each surviving entry at most
  once with its net old/new state;
- removal state propagation using the same complete-overlap affected-set
  algorithm as insertion.

Tests
- RemoveByRangeRemovesOneExactEntry
- InvalidAndMissingRemovalLeaveTreeUnchanged
- RemoveByRangeMatchesDuplicateByOpaqueValue
- FailedRemovalPredicateLeavesTreeUnchanged
- RemoveAliasMatchesRemoveByRange
- RemoveAllOverlappingReturnsExactCount
- RemovalRebalancesBTree
- RemovalMaintainsInvariantsAcrossMixedOrders
- RemovalSupportsMoveOnlyPayloads
- RemovalRecomputesSurvivorTopologyAndPriority
- RemovalNotifiesFormerlyOverlaidEntry
- BatchRemovalNotifiesEachSurvivorOnce
- RemovalCallbackRunsAfterTreeMutation
- RemovalCallbackExceptionLeavesTreeValid
- RemovalCallbacksFollowCanonicalOrder
- RemovalPredicateCannotMutateTreeReentrantly
- ClearSuppressesCallbacksAndAllowsReuse
- ReentrantClearFromCallbackIsRejected

This is the only layer that implements deletion balancing. Layer 10 is limited
to subtree metadata and overlap-query pruning; it must not reimplement
deletion.

Exit: removal semantics and deletion balancing complete.

## 13. Layer 9: canonical enumeration and reconstruction
Implementation

Keep the already completed copy-returning `enumerateCanonical()` behavior
from Layer 2B. Add these capabilities in order:

1. Implement `enumerateCanonicalMove()` and test empty, populated, duplicate,
   callback ordering, move-only payload, and extraction-clears-tree behavior.
2. Add callback registration/unregistration and test context ownership,
   matching precedence, and dispatch.
3. Implement `reconstructFromCanonical()` using a temporary validated
   representation, then replace the destination only after validation and
   construction succeed. Test invalid input leaves the destination unchanged.
4. Test callback reconnection and the round-trip enumeration/topology
   invariant. Suppress callbacks caused only by reconstruction.

Implement:
- `enumerateCanonicalMove(duplicateOrder)` moves all stored payloads out and
  supports move-only `T`; it computes the full result before clearing the
  tree and does not invoke callbacks for extracted entries.
- `reconstructFromCanonical(entries, duplicateOrder)` validates the complete
  vector, loads entries in exactly vector order, assigns reconstruction
  ordinals in that order, reconnects registered callbacks by matching opaque
  values, recomputes state after loading, and suppresses load-time callbacks.
  It leaves the destination unchanged on validation failure.
- callback registration and unregistration by a client-supplied matcher over
  opaque values. Each registration carries client-supplied shared context and
  invokes its callback with `(context, opaqueValue, oldState, newState)`;
  registrations are runtime metadata and are not serialized.

The required round-trip invariant is:

```text
destination.enumerateCanonical(order) ==
source.enumerateCanonical(order)
```

after reconstructing an empty destination from the source result. The
invariant concerns observable logical topology and query results, not
physical node shape.

Tests
- MoveOnlyPayloadCanBeEnumeratedByMove
- MoveEnumerationUsesDuplicateOrdering
- MoveEnumerationDoesNotDispatchCallbacks
- CanonicalReconstructionPreservesTopology
- ReconstructionValidationFailureLeavesTreeUnchanged
- ReconstructionReconnectsRegisteredCallbacks
- UnregisterCallbackStopsFutureDispatch
- ReconstructionUsesFirstMatchingRegistration
- ReconstructionPreservesDuplicateCanonicalOrder
- RebuildMatchesOriginalCanonicalOrder

Exit: canonical enumeration complete.

## 14. Layer 10: overlap metadata and pruning
Implementation

Add and maintain `m_subtreeMaxEnd` metadata for overlap pruning after every
insertion, split, deletion, borrow, merge, and root contraction. Keep the
unpruned overlap traversal from Layer 6 as the correctness baseline and
compare pruned results against it in tests.

Tests
- SubtreeMaxEndRemainsCorrectAcrossMutations
- Int64BoundarySubtreeMaxEndRemainsCorrect
- PrunedOverlapQueriesMatchCanonicalBaseline

Exit: metadata and pruning preserve all B-tree and overlap-query invariants.

## 15. Layer 11: lifecycle and integration
Implementation

Verify the API's snapshot/rebuild and lifecycle behavior at the interval-tree
boundary. Reconstruction itself is implemented and tested in Layer 9.
Consumer wiring into Dmn_DLock cannot be completed until a Dmn_DLock
production module exists; the repository currently contains only its design
documents, so do not invent a parallel lock implementation in this layer.

Tests
- RebuildMatchesOriginalCanonicalOrder
- ClearSuppressesCallbacksAndAllowsReuse
- DestructorReleasesAllUniqueOwnedEntries

Exit: interval-tree lifecycle and snapshot boundary verified. Dmn_DLock
consumer integration remains a downstream task in the DLock implementation
plan.

## 16. Feasibility and design risks

The design is feasible in C++23, but the following constraints are
implementation-critical:

- Duplicate ranges require an explicit enumeration-time ordering callback
  when canonical output must be independent of insertion history. Without
  that callback, the per-tree insertion ordinal provides deterministic
  ordering only for that tree's insertion history.
- State transitions must be computed from the complete overlapping set.
  Immediate left/right neighbors alone are insufficient for
  `OverlaidBoth` and `FullyCovered`.
- No second-level callback propagation is needed while callbacks cannot
  mutate ranges or priorities. Reentrant mutation is explicitly rejected.
- Callback exceptions occur after a successful, consistent mutation and are
  propagated without rollback; callers must handle this contract.
- `m_subtreeMaxEnd` is required for safe pruning; every structural mutation
  must update it or overlap queries can become incorrect.
- Parallel vectors are fragile. An internal entry record containing range,
  value, ordinal, and callback is preferred, even if the public node layout
  remains equivalent.
- `T` must satisfy the copying/moving requirements of the selected storage
  and result APIs. The `const T&` insertion path copies, the `T&&` path
  moves, and move-only payloads require move-oriented enumeration because
  copy-returning query APIs cannot expose them.
- Duplicate ordering must be supplied explicitly when canonical output must
  be independent of insertion history. The callback follows standard sort
  semantics and must be a strict weak ordering.

The design is therefore implementable with the duplicate-order,
callback-exception, and reentrancy policies documented above. These policies
must be covered by tests before production integration.

The tree is intentionally not thread-safe. Callers must externally
synchronize concurrent reads and mutations. The implementation should not
add internal locking unless the public contract is expanded to define lock
ownership and callback execution under locking.

## 17. Definition of done

- All tests pass.
- Range semantics match DLock invariants.
- Canonical ordering is deterministic.
- B‑tree insertion/removal maintain invariants.
- Overlap queries are correct.
- Duplicate-range removal predicates are deterministic and side-effect safe.
- Copy and move insertion paths both work, with move-only payload support
  through move enumeration.
- State callbacks are complete, ordered after mutation, and never fire for
  a new entry's initial state.
- Template payload support complete.
- API follows DMN coding conventions.
- Documentation includes examples and test references.
