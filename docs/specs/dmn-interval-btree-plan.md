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

1. Add dmn-interval-btree.h with forward declarations.
2. Add empty test target dmn-test-interval-btree.
3. Build and run tests.

Exit: baseline compiles.

## 5. Layer 1: range semantics
Implementation

Implement Dmn_IntervalRange with:
- isValid()
- overlaps()

Tests
- IntervalRangeRejectsNegativeAndReversed
- IntervalRangeInclusiveSharedEndpointConflicts
- IntervalRangeAdjacentRangesDoNotConflict

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

Allow caller to supply a custom comparator to incorporate:
- priority,
- sequence,
- request_id,
- or other stable fields.

Tests
- IntervalBTreeCanonicalOrderSimple
- IntervalBTreeCanonicalOrderStable
- IntervalBTreeEmptyAndSize

Exit: basic API and canonical ordering complete

## 7. Layer 3: template payload support
Implementation

Ensure T:
- is stored per node,
- does not affect ordering,
- is movable/copyable.

Tests
- IntervalTreeSupportsStringPayload
- IntervalTreeSupportsStructPayload
- IntervalTreePayloadDoesNotAffectOrdering

Exit: template payload support complete.

## 8. Layer 4: B-tree node structure
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

Tests
- IntervalBTreeNodeStoresMultipleKeys
- IntervalBTreeNodeChildrenPartitionCorrectly

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
- IntervalBTreeOverlapQueryPrunesCorrectly

Exit: overlap queries complete.

## 11. Layer 7: removal operations
Implementation

Implement:
- remove(range,value)
- removeByRange(range)
- removeAllOverlapping(range)
- node merges and borrowing

Tests
- IntervalBTreeAddAndRemoveExact
- IntervalBTreeRemoveByRange
- IntervalBTreeRemoveAllOverlapping
- IntervalBTreeClearResetsState

Exit: removal complete.

## 12. Layer 8: canonical enumeration (final)
Implementation

Implement:
- in-order traversal,
- stable canonical ordering,
- deterministic output.

Tests
- IntervalBTreeCanonicalEnumerationMatchesSpec
- IntervalBTreeCanonicalEnumerationDeterministic

Exit: canonical enumeration complete.

## 13. Layer 9: integration hooks (optional)
Implementation

Provide:
- toVector()
- fromVector()

Tests
- IntervalTreeRebuildMatchesOriginalCanonicalOrder

Exit: integration complete.

## 14. Definition of done

- All tests pass.
- Range semantics match DLock invariants.
- Canonical ordering is deterministic.
- B‑tree insertion/removal maintain invariants.
- Overlap queries are correct.
- Template payload support complete.
- API follows DMN coding conventions.
- Documentation includes examples and test references.
