# Implementation Plan: `Dmn_IntervalBTree` (Template Interval B‑Tree)

## 1. Delivery rule

Implement `Dmn_IntervalBTree` in small, independently buildable, test‑driven increments:

1. introduce one focused failing test;
2. add the smallest real API and implementation needed to satisfy that test;
3. build only the affected target and run its completed tests;
4. run `git diff --check`;
5. avoid placeholder success methods and uncompilable future tests.

No interval‑tree implementation or test target currently exists.  
Create tests only in layers with compilable production code.

---

## 2. Target architecture

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
cmake --build build --target dmn-interval-btree dmn-test-interval-btree
ctest --test-dir build -R 'dmn-test-interval-btree' --output-on-failure
git diff --check
```

## 4. Layer 0: baseline

1. Add dmn-interval-btree.hpp with forward declarations.
2. Add empty test target dmn-test-interval-btree.
3. Build and run tests.

Exit: baseline compiles.

## 5. Layer 1: range semantics
Implementation

Implement Dmn_IntervalRange with:
- isValid()
- overlaps()
- boundary comparisons that do not compute `start - 1` or `end + 1`.

Tests
- IntervalRangeRejectsNegativeAndReversed
- IntervalRangeAcceptsSinglePoint
- IntervalRangeAcceptsNegativeValues
- IntervalRangeInclusiveSharedEndpointConflicts
- IntervalRangeAdjacentRangesDoNotConflict
- IntervalRangeHandlesInt64BoundariesWithoutOverflow

Exit: range semantics complete.

## 6. Layer 2: API skeleton and canonical comparator
Implementation

Define Dmn_IntervalBTree<T> with:
- constructors,
- add() storing entries in std::vector,
- enumerateCanonical() sorting via canonical comparator.

Default canonical comparator:
1. ascending range.start
2. ascending range.end
3. per-tree insertion ordinal for duplicate ranges

Allow caller to supply a custom range-only comparator and, at enumeration time,
an optional duplicate-order callback over two opaque values. The duplicate
callback follows the standard sorting convention: `true` places its first
argument before its second. It is called only for identical ranges; the
range comparator remains authoritative for distinct ranges.

The range comparator may only customize ordering of ranges. Priority,
sequence, request IDs, and opaque payload fields belong in the duplicate
ordering callback or priority evaluator, not in canonical range ordering.

Tests
- IntervalBTreeCanonicalOrderSimple
- IntervalBTreeCanonicalOrderStable
- IntervalBTreeDuplicateRangesUseStableTieBreak
- IntervalBTreeDuplicateRangesUseCallerOrdering
- IntervalBTreeDuplicateOrderingCallbackNotCalledForDistinctRanges
- IntervalBTreeCustomComparatorIsUsed
- IntervalBTreeEmptyAndSize

Exit: basic API and canonical ordering complete

## 7. Layer 3: template payload support
Implementation

Ensure T:
- is stored per node,
- does not affect ordering,
- supports both copy insertion and move insertion;
- supports move-only payloads for storage and move enumeration;
- is copy-constructible only for copy-returning query APIs.

Tests
- IntervalTreeSupportsStringPayload
- IntervalTreeSupportsStructPayload
- IntervalTreePayloadDoesNotAffectOrdering
- IntervalBTreeMoveOnlyPayloadCanBeInserted
- IntervalBTreeMoveOnlyPayloadCanBeEnumeratedByMove
- IntervalBTreeMoveOnlyPayloadSupportsStateCallback
- IntervalBTreeMoveOnlyPayloadDoesNotInstantiateCopyQueries

Exit: template payload support complete.

## 8. Layer 4: B-tree node structure and ownership
Implementation

Introduce:
```cpp
template <class T>
struct Dmn_IntervalBTreeNode {
  std::vector<Dmn_IntervalRange> ranges;
  std::vector<T> values;
  std::vector<std::unique_ptr<Dmn_IntervalBTreeNode>> children;
  bool isLeaf{true};
};
```

The tree owns `root_` with `std::unique_ptr`; children own descendants with
`std::unique_ptr`. Do not add parent pointers or `std::shared_ptr`. Preserve
each entry's value and optional callback when keys move during split, merge,
or borrowing.
Choose and document a fixed B-tree minimum degree of at least two. Structural
tests must verify root exceptions and minimum/maximum key counts for every
non-root node.

Tests
- IntervalBTreeNodeStoresMultipleKeys
- IntervalBTreeNodeChildrenPartitionCorrectly
- IntervalBTreeRootSplitCreatesOwnedRoot
- IntervalBTreeSplitPreservesCallbacksAndOrdinals

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
- IntervalBTreeInsertionMaintainsOrder
- IntervalBTreeInsertionSplitsNodes
- IntervalBTreeInsertionDeterministicAcrossOrders
- IntervalBTreeEnumerationIsIndependentOfNodeSplits

Exit: insertion complete.

## 10. Layer 6: overlap queries
Implementation

Implement:
- hasOverlap(range)
- findOverlapping(range)

Traversal rules:
- check each key in node for overlap,
- descend into children whose key ranges may overlap,
- optionally maintain subtree metadata (e.g., max end) to prune branches.

Tests
- IntervalBTreeFindOverlappingSingle
- IntervalBTreeFindOverlappingMultiple
- IntervalBTreeFindOverlappingIncludesSharedEndpoints
- IntervalBTreeFindOverlappingRejectsInvalidQuery
- IntervalBTreeHasOverlapRejectsInvalidQuery
- IntervalBTreeOverlapQueryPrunesCorrectly

Exit: overlap queries complete.

## 11. Layer 7: topological overlay logic and entry state
Implementation

Introduce Dmn_OverlayTopology, Dmn_TopologyResult, and the priority_evaluator.
Introduce `Dmn_OverlayState`, an optional per-entry state-change callback,
and an entry matcher for exact-range removal. Duplicate ranges are valid.
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
- IntervalBTreeTopologyReturnsClearWhenEmpty
- IntervalBTreeTopologyIdentifiesOverlaidLeftAndRight
- IntervalBTreeTopologyIdentifiesFullyCovered
- IntervalBTreeTopologyPrecedenceIsDeterministic
- IntervalBTreeTopologyExactMatchIsFullyCovered
- IntervalBTreeTopologyHandlesContiguousCoverage
- IntervalBTreeTopologyHandlesCoverageGap
- IntervalBTreeTopologyHandlesInt64BoundariesWithoutOverflow
- IntervalBTreePriorityTieMarksBothEntriesTop
- IntervalBTreeQueryTopologyIsHypothetical
- IntervalBTreeTopologyUsesInsertionLevelForContainment
- IntervalBTreeTopologyEvaluatesIsTopCorrectly
- IntervalBTreeNoPriorityEvaluatorMarksEntryTop
- IntervalBTreeAddWithTopologyMatchesQueryTopology
- IntervalBTreeNewEntryCallbackNotInvokedOnInsertion
- IntervalBTreeInsertionNotifiesExistingOverlappedEntry
- IntervalBTreeInsertionNotifiesPriorityChange
- IntervalBTreeInsertionNotifiesNonAdjacentAffectedEntries

Exit: layered range locking queries and topology states complete.

## 12. Layer 8: removal operations and state transitions
Implementation

Implement:
- remove(range, matcher)
- removeByRange(range, matcher)
- removeAllOverlapping(range)
- node merges and borrowing
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
- IntervalBTreeAddAndRemoveExact
- IntervalBTreeRejectsInvalidInsertionWithoutMutation
- IntervalBTreeRemoveByRange
- IntervalBTreeRemoveDuplicateRangeWithMatcher
- IntervalBTreeEmptyMatcherRemovesUniqueRange
- IntervalBTreeRemovePredicateSelectsOpaqueValue
- IntervalBTreeRemovePredicateMissLeavesTreeUnchanged
- IntervalBTreeRemoveAllOverlapping
- IntervalBTreeRemoveAllOverlappingReturnsCount
- IntervalBTreeRemovalNotifiesFormerlyOverlaidEntry
- IntervalBTreeRemovalRecomputesBothBoundaryOverlays
- IntervalBTreeRemovalRecomputesFullyCoveredState
- IntervalBTreeRemovalNotifiesAllAffectedNonAdjacentEntries
- IntervalBTreeBatchRemovalNotifiesEachEntryAtMostOnce
- IntervalBTreeCallbacksRunInCanonicalOrder
- IntervalBTreeClearDoesNotNotifyDestroyedEntries
- IntervalBTreeCallbackRunsAfterMutationIsConsistent
- IntervalBTreeCallbackExceptionLeavesTreeValid
- IntervalBTreeCallbackExceptionDoesNotPoisonTree
- IntervalBTreeReentrantMutationIsRejected
- IntervalBTreeClearResetsState

Exit: removal complete.

## 13. Layer 9: canonical enumeration (final)
Implementation

Implement:
- in-order traversal,
- stable canonical ordering,
- deterministic output.
- `enumerateCanonical(duplicateOrder)` applies the callback only to
  identical ranges, where `true` places its first opaque value before its
  second;
- `enumerateCanonicalMove(duplicateOrder)` moves all stored payloads out and
  supports move-only `T`; it computes the full result before clearing the
  tree and does not invoke callbacks for extracted entries.

Tests
- IntervalBTreeCanonicalEnumerationMatchesSpec
- IntervalBTreeCanonicalEnumerationDeterministic
- IntervalBTreeEnumerationIsIndependentOfNodeSplits
- IntervalBTreeDuplicateRangesUseCallerOrdering
- IntervalBTreeMoveOnlyPayloadCanBeEnumeratedByMove
- IntervalBTreeDuplicateOrderingCallbackNotCalledForDistinctRanges

Exit: canonical enumeration complete.

## 14. Layer 10: deletion balancing and metadata
Implementation

Complete internal deletion balancing:
- leaf deletion;
- internal-key replacement;
- sibling borrowing;
- node merging;
- root contraction;
- `subtreeMaxEnd` maintenance after every mutation.

Tests
- IntervalBTreeRemoveRebalancesLeaf
- IntervalBTreeRemoveRebalancesInternalNode
- IntervalBTreeSubtreeMaxEndRemainsCorrect
- IntervalBTreeInt64BoundaryMetadataRemainsCorrect

Exit: deletion preserves all B-tree and overlap-pruning invariants.

## 15. Layer 11: lifecycle and integration
Implementation

Provide optional `toVector()` and `fromVector()` only if a consuming
component requires them. `fromVector()` MUST preserve canonical ordering
semantics and callback behavior, and MUST define whether callbacks are
installed or suppressed during reconstruction.

Tests
- IntervalTreeRebuildMatchesOriginalCanonicalOrder
- IntervalBTreeClearResetsSizeAndAllowsReuse
- IntervalBTreeDestructorReleasesAllUniqueOwnedNodes

Exit: integration complete.

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
- `subtreeMaxEnd` is required for safe pruning; every structural mutation
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
