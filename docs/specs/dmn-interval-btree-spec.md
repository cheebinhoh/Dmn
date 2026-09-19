# Feature Specification: DMN Interval B‑Tree (`Dmn_IntervalBTree`)

Status: initial standalone specification, extracted and refined from the DMN Distributed Range Lock (`Dmn_DLock`) specification.

## 1. Purpose and scope

`Dmn_IntervalBTree<T>` provides a reusable, template-based in-memory interval index over inclusive integer ranges, with associated values of arbitrary type `T`. It is designed to:

- support efficient overlap and range queries for `Dmn_DLock` and other DMN components;
- preserve canonical, deterministic enumeration independent of internal tree shape;
- follow DMN repository coding conventions and API signature guidelines.

This specification extracts and refines the interval-tree requirements from the DLock spec:

> “Implementations MAY use an interval tree per handler to store and query entries by range, provided that the interval tree is a pure in-memory representation; serialization uses a canonical ordering independent of the tree’s internal shape.”

The interval B‑tree is purely in-memory and does not define wire format or commit authority.

---

## 2. Range model and invariants

### 2.1 Range definition

A range is valid iff: start <= end

Overlap is defined as: a.start <= b.end && b.start <= a.end

Thus:

- `[1,2]` conflicts with `[2,3]`
- `[1,2]` does not conflict with `[3,4]`

Invalid ranges MUST be rejected.

Dmn_DLock MAY impose additional domain-specific constraints (e.g., start >= 0).
These constraints are NOT part of the generic interval B-tree.

### 2.2 Canonical ordering

Canonical ordering is normative and MUST be identical across all handlers.

Ordering keys:

1. ascending `range.start`
2. ascending `range.end`

Payload `T` MUST NOT participate in ordering.

Canonical enumeration MUST produce a byte-identical ordering for equal logical tables.

---

## 3. Template API shape

### 3.1 Core types

```cpp
struct Dmn_IntervalRange {
  std::int64_t start{};
  std::int64_t end{};

  bool isValid() const noexcept;
  bool overlaps(const Dmn_IntervalRange &other) const noexcept;
};

template <class T>
class Dmn_IntervalBTree;
```

Dmn_IntervalRange::isValid() implements the start <= end rule.

Dmn_IntervalRange::overlaps() implements the inclusive overlap rule.


### 3.2 Tree interface

The public API is normative and follows DMN naming and signature conventions:

```cpp
template <class T>
class Dmn_IntervalBTree {
public:
  using value_type = T;
  using range_type = Dmn_IntervalRange;

  // Construction
  Dmn_IntervalBTree() noexcept;
  explicit Dmn_IntervalBTree(std::function<bool(
      const range_type &, const value_type &,
      const range_type &, const value_type &)> canonicalComparator);

  // Rule of 5: delete copy/move
  Dmn_IntervalBTree(const Dmn_IntervalBTree&) = delete;
  Dmn_IntervalBTree& operator=(const Dmn_IntervalBTree&) = delete;
  Dmn_IntervalBTree(Dmn_IntervalBTree&&) = delete;
  Dmn_IntervalBTree& operator=(Dmn_IntervalBTree&&) = delete;

  ~Dmn_IntervalBTree() noexcept;

  // Insertion
  bool add(range_type range, const value_type &value);
  bool add(std::int64_t start, std::int64_t end, const value_type &value);

  // Removal
  bool remove(range_type range, const value_type &value);
  bool removeByRange(range_type range);
  std::size_t removeAllOverlapping(range_type range);

  // Queries
  bool hasOverlap(range_type range) const noexcept;
  std::vector<std::pair<range_type, value_type>>
  findOverlapping(range_type range) const;

  std::vector<std::pair<range_type, value_type>>
  enumerateCanonical() const;

  bool empty() const noexcept;
  std::size_t size() const noexcept;

  // Clear
  void clear() noexcept;
};
```

### 3.3 Example

```cpp
Dmn_IntervalBTree<std::string> tree;

tree.add(10, 15, "A");
tree.add(5, 9, "B");

// Invalid: start > end
bool ok = tree.add(10, 3, "C"); // ok == false

auto entries = tree.enumerateCanonical();
// entries: [ (5–9, "B"), (10–15, "A") ]
```

## 4. Internal B‑tree design

### 4.1 Node structure
```cpp
template <class T>
struct Dmn_IntervalBTreeNode {
  std::vector<Dmn_IntervalRange> ranges;
  std::vector<T> values;
  std::vector<std::unique_ptr<Dmn_IntervalBTreeNode>> children;
  bool isLeaf{true};
};
```

### 4.2 Insertion
- descend using canonical comparator;
- insert into leaf;
- split nodes when capacity exceeded;
- propagate splits upward;
- maintain sorted keys within nodes.

### 4.3 Overlap queries
- check each key for overlap;
- descend into children whose key ranges may overlap;
- optional subtree metadata (e.g., max end) may prune branches.

### 4.4 Canonical enumeration
- in-order traversal of B‑tree;
- stable canonical ordering;
- deterministic output independent of tree shape.

## 5. Deterministic test matrix

### 5.1 Range tests
- IntervalRangeRejectsNegativeAndReversed
- IntervalRangeInclusiveSharedEndpointConflicts
- IntervalRangeAdjacentRangesDoNotConflict

### 5.2 Ordering tests
- IntervalBTreeCanonicalOrderSimple
- IntervalBTreeCanonicalOrderStable

### 5.3 Overlap tests
- IntervalBTreeFindOverlappingSingle
- IntervalBTreeFindOverlappingMultiple
- IntervalBTreeOverlapQueryPrunesCorrectly

### 5.4 Add/remove tests
- IntervalBTreeAddAndRemoveExact
- IntervalBTreeRemoveByRange
- IntervalBTreeRemoveAllOverlapping
- IntervalBTreeClearResetsState

### 5.5 Structural tests
- IntervalBTreeNodeStoresMultipleKeys
- IntervalBTreeNodeChildrenPartitionCorrectly

## 6. Definition of done

- All range semantics match DLock invariants.
- Canonical ordering is stable and deterministic.
- B‑tree insertion and removal maintain structural invariants.
- Overlap queries return exactly the overlapping entries.
- Template payload support is complete.
- API follows DMN coding conventions.
- Documentation includes examples and test references.
