# Feature Specification: DMN Interval B‑Tree (`Dmn_IntervalBTree`)

Status: implemented API contract; verified coverage is tracked below and in
`dmn-interval-btree-plan.md`.

Implementation status: Layers 0-10 of `dmn-interval-btree-plan.md` are
implemented. The focused `dmn-test-interval-btree` target covers range
validation, insertion, canonical ordering, arbitrary and move-only payload
storage, degree-2 B-tree insertion/deletion, overlap and topology queries,
state callbacks, canonical move extraction/reconstruction, and incrementally
maintained subtree metadata for pruned overlap traversal. Layer 11
lifecycle/snapshot tests are present. A partial `Dmn_DLock` prototype exists,
but it currently does not use this tree for its table mirror.

Invalid-range behavior follows the compiled API and unit tests: `add`,
`addWithTopology`, exact/batch removal, `findOverlapping`,
`forEachOverlapping`, and `queryTopology` throw `std::invalid_argument`.
`hasOverlap` is `noexcept` and returns `false` for an invalid range.
`Dmn_IntervalRange::overlaps` also returns `false` for invalid operands.

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

A generic interval range is valid iff: `m_start <= m_end`.

Overlap is defined as: a.m_start <= b.m_end && b.m_start <= a.m_end

Thus:

- `[1,2]` conflicts with `[2,3]`
- `[1,2]` does not conflict with `[3,4]`

Invalid ranges MUST be rejected. `Dmn_IntervalRange::overlaps()` MUST return
`false` if either operand is invalid; callers must not be able to get a
positive overlap result from a reversed range. For B-tree operations, invalid
ranges throw `std::invalid_argument` except `hasOverlap()`, which returns
`false`; see the API contract in Section 3.2.
Implementations MUST avoid signed overflow when evaluating boundaries,
coverage, or subtree metadata. Inclusive overlap checks use endpoint
comparisons rather than predecessor/successor arithmetic. Topology coverage
may use a successor calculation only with an overflow guard; the current
implementation checks against `INT64_MAX` before evaluating `coveredEnd + 1`.

Dmn_DLock MAY impose additional domain-specific constraints (e.g., start >= 0).
These constraints are NOT part of the generic interval B-tree.

### 2.2 Canonical ordering

Canonical ordering is normative and MUST be identical across all handlers
that use the same canonical-comparator configuration.

With the default comparator, entries with distinct ranges are ordered by:

1. ascending `range.m_start`
2. ascending `range.m_end`

Payload `T` MUST NOT participate in ordering.

Duplicate ranges are allowed. The tree MUST retain a stable internal insertion
ordinal for each entry and use it as the final tie-breaker when no duplicate
ordering callback is supplied. A caller that needs canonical output
independent of duplicate insertion order MUST provide a duplicate ordering
callback over the opaque values. Payload data remains excluded from the
default ordering.

A caller MAY provide a custom range comparator. It MUST be a deterministic
strict weak ordering, MUST inspect ranges only, and MUST be configured
consistently by handlers that need identical canonical output. When the custom
comparator orders two distinct ranges as equivalent, the default
`(range.m_start, range.m_end)` ordering breaks that tie. For identical ranges,
`duplicateOrder` breaks the tie when supplied; otherwise insertion ordinal
does. The duplicate callback is never used for distinct ranges. Payloads do
not affect ordering unless the caller explicitly supplies them to
`duplicateOrder` for identical ranges.

`enumerateCanonical()` is also the logical topology snapshot operation. A
caller MAY feed its result to `reconstructFromCanonical()` on an empty tree.
The reconstruction MUST consume entries in exactly the supplied order, assign
new reconstruction ordinals in that order, rebuild all metadata, and produce
the same subsequent canonical enumeration when given the same duplicate-order
callback. This guarantees equivalent observable logical topology and query
results; it does not require identical physical node splits or allocation.

---

## 3. Template API shape

### 3.1 Core types

```cpp
struct Dmn_IntervalRange {
  std::int64_t m_start{};
  std::int64_t m_end{};

  bool isValid() const noexcept;
  bool overlaps(const Dmn_IntervalRange &other) const noexcept;
};

// Defines the structural relationship of a range against existing tree entries
enum class Dmn_OverlayTopology {
  Clear,             // No overlaps exist
  OverlaidLeft,      // Existing entries cover the left boundary (e.g., new 13-17 overlaps existing 10-15)
  OverlaidRight,     // Existing entries cover the right boundary
  OverlaidBoth,      // Existing entries cover both left and right boundaries independently
  FullyCovered,      // Range is entirely engulfed by one or more existing entries
  CoveringExisting   // Range entirely engulfs one or more existing entries
};

// Rich return type for topological insertions and queries
template <class T>
struct Dmn_TopologyResult {
  Dmn_OverlayTopology m_status{Dmn_OverlayTopology::Clear};
  bool m_isTop{true};  // True if this entry holds the highest priority among all its overlaps
  std::vector<std::pair<Dmn_IntervalRange, T>> m_overlappingEntries;
};

template <class T>
class Dmn_IntervalBTree;
```

An entry is the tuple of its range, value, and optional state-change callback.
Entries with identical ranges are allowed. The value is opaque to the tree
except when passed to the priority evaluator or an entry-removal predicate.

```cpp
template <class T>
using entry_matcher = std::function<bool(const T&)>;

template <class T>
using duplicate_order_evaluator = std::function<bool(const T&, const T&)>;

struct Dmn_OverlayState {
  Dmn_OverlayTopology m_topology{Dmn_OverlayTopology::Clear};
  bool m_isTop{true};
};

template <class T>
using state_change_callback = std::function<void(
    const Dmn_IntervalRange&, const T&,
    const Dmn_OverlayState& oldState,
    const Dmn_OverlayState& newState)>;
```

The callback is runtime metadata. It is not part of canonical ordering,
equality, serialization, or topology calculation. A callback MAY be attached
at insertion time or through a registration keyed by client-supplied opaque
data. This permits a tree rebuilt from a canonical entry list to reconnect
runtime callbacks without serializing function objects.

Registrations are evaluated against each reconstructed entry's opaque value.
The first matching registration is used; registrations MUST be deterministic
and clients MUST NOT register overlapping matchers unless precedence is
intentional. A missing registration leaves the entry without a callback and
is not an error. Registration and reconstruction obey the tree's external
synchronization requirement.
Callbacks are optional and are invoked only when either `topology` or
`isTop` changes for an already-existing entry.

Dmn_IntervalRange::isValid() implements the `m_start <= m_end` rule.

Dmn_IntervalRange::overlaps() implements the inclusive overlap rule.

### 3.1 Duplicate ranges and removal matching

Multiple entries may have the same `start` and `end`. `remove` and
`removeByRange` perform exact range matching and accept an optional predicate
over the opaque value. The predicate is evaluated only for exact-range
candidates. The first matching entry in the tree's deterministic traversal
order is removed. An empty predicate matches the first exact-range entry and
is intended for callers that guarantee range uniqueness.

The predicate MUST NOT mutate the tree or any entry. It is evaluated in
deterministic canonical order and SHOULD be non-throwing. No ordering or
identity is inferred from the predicate. `removeAllOverlapping` removes every
entry whose range overlaps the argument and does not accept a matcher.

`remove` and `removeByRange` are equivalent exact-range operations; the
preferred name is `removeByRange` when no future value-based overloads are
planned. Both return `false` without mutation when the range is valid but no
candidate matches. Invalid ranges throw `std::invalid_argument`.

#### 3.1.1 OverlaidBoth
Condition: The new range's left boundary falls inside one existing lock, and its right boundary falls inside a different existing lock, leaving an uncovered gap in the middle.

Key Distinguishing Feature: Both endpoints are covered, but by independent, disconnected locks.

Example: Existing in Tree: [5, 10] and [20, 25], New Entry: [8, 22], Result: OverlaidBoth, Why:
- Left boundary (8) falls inside [5, 10].
- Right boundary (22) falls inside [20, 25].
- The middle section (10, 20) is open space.

#### 3.1.2 FullyCovered
Condition: The entire new range falls completely within existing lock coverage without any gap. The new lock adds no new range scope to the tree.

Key Distinguishing Feature: Every single point from start to end is already held by one or more existing contiguous locks.

Example: Existing in Tree: [10, 20], New Entry: [12, 18], Result: FullyCovered

#### 3.1.3 CoveringExisting
Condition: The new range completely engulfs/swallows one or more existing locks, with both its left and right boundaries extending past the existing entries into open space.

Key Distinguishing Feature: The new lock is strictly larger than existing overlapping locks and completely contains them.

Example: Existing in Tree: [10, 15], New Entry: [5, 20], Result:
CoveringExisting, because the new range strictly contains the existing range
and extends beyond it on both sides.

#### 3.1.4 Summary Matrix
Topology Enum    | Left Boundary State | Right Boundary State | Existing Entries Contained Inside?
Clear            | Open                | Open                 | No existing entries overlapped
OverlaidLeft     | Covered             | Open                | No
OverlaidRight    | Open                | Covered              | No
OverlaidBoth     | Covered             | Covered              | No (gap in middle)
FullyCovered     | Covered             | Covered              | Fully continuous coverage throughout
CoveringExisting | Open                | Open                 | Yes (swallows existing locks)

Topology classification MUST use this precedence:

1. `Clear` if there are no overlaps.
2. `FullyCovered` if the existing overlapping ranges form continuous
   inclusive coverage of the candidate range.
3. `CoveringExisting` if the candidate strictly contains at least one
   existing range and extends beyond the complete covered span on both sides.
4. `OverlaidBoth` if both candidate boundaries are covered but the coverage
   has a gap.
5. `OverlaidLeft` or `OverlaidRight` if exactly one boundary is covered.

The implementation MUST document and test the equality-boundary cases. A
boundary point counts as covered because overlap is inclusive. If a range
matches an existing range exactly, it is `FullyCovered`, not
`CoveringExisting`.

Topology is evaluated from the perspective of the entry being described.
Insertion order defines overlay level: a newly inserted entry is higher than
entries already present. When that higher-level entry strictly contains an
existing entry, the new entry is `CoveringExisting` and the existing entry is
`FullyCovered`. This level-aware distinction applies even though both entries
refer to the same geometric overlap. An exact duplicate range is not strict
containment: the new entry is `FullyCovered`, and the existing entry remains
`FullyCovered` unless another overlap changes its state.
For every stored entry, classification must use the entry's own range and
level against all overlapping entries: a strict container is
`CoveringExisting`, while an entry strictly contained by a higher-level
entry is `FullyCovered`. This rule is applied consistently when recomputing
existing-entry state after insertion or removal.

### 3.2 Tree interface

The public API is normative and follows DMN naming and signature conventions:

```cpp
template <class T>
class Dmn_IntervalBTree {
public:
  using value_type = T;
  using range_type = Dmn_IntervalRange;

  // Evaluates which value_type is "higher" in the stack (e.g., comparing sequence IDs).
  // Returns true if 'a' has strictly higher priority/Z-order than 'b'.
  using priority_evaluator = std::function<bool(const value_type& a, const value_type& b)>;

  // Construction
  Dmn_IntervalBTree() noexcept = default;
  explicit Dmn_IntervalBTree(std::function<bool(
      const range_type &, const range_type &)> canonicalComparator);
  Dmn_IntervalBTree(
      std::function<bool(const range_type &, const range_type &)> canonicalComparator,
      priority_evaluator priorityEvaluator);

  // Rule of 5: delete copy/move
  Dmn_IntervalBTree(const Dmn_IntervalBTree&) = delete;
  Dmn_IntervalBTree& operator=(const Dmn_IntervalBTree&) = delete;
  Dmn_IntervalBTree(Dmn_IntervalBTree&&) = delete;
  Dmn_IntervalBTree& operator=(Dmn_IntervalBTree&&) = delete;

  ~Dmn_IntervalBTree() noexcept;

  // Insertion. These overloads mirror Dmn_BlockingQueue::push(const T&)
  // and push(T&&): lvalues are copied and rvalues are moved.
  bool add(range_type range, const value_type &value,
           state_change_callback<value_type> onStateChange = {});
  bool add(range_type range, value_type &&value,
           state_change_callback<value_type> onStateChange = {});
  bool add(std::int64_t start, std::int64_t end, const value_type &value,
           state_change_callback<value_type> onStateChange = {});
  bool add(std::int64_t start, std::int64_t end, value_type &&value,
           state_change_callback<value_type> onStateChange = {});

  // Layer-Aware Insertion
  // The new entry's state is returned; its callback is not invoked for this
  // initial state.
  std::pair<bool, Dmn_TopologyResult<T>> addWithTopology(
      range_type range, const value_type &value,
      state_change_callback<value_type> onStateChange = {});

  // Removal
  // Removes one exact-range entry for which matcher returns true. An empty
  // matcher matches the first exact-range entry.
  bool remove(range_type range, entry_matcher<value_type> matcher = {});
  bool removeByRange(range_type range, entry_matcher<value_type> matcher = {});
  std::size_t removeAllOverlapping(range_type range);

  // Queries
  bool hasOverlap(range_type range) const noexcept;
  std::vector<std::pair<range_type, value_type>> findOverlapping(range_type range) const;
  using overlap_visitor = std::function<void(
      const range_type&, const value_type&)>;
  void forEachOverlapping(range_type range, overlap_visitor visitor) const;

  // Layer-Aware Query
  // Evaluates where a given range and value sit in the stack without modifying the tree
  Dmn_TopologyResult<T> queryTopology(range_type range, const value_type &value) const;

  std::vector<std::pair<range_type, value_type>> enumerateCanonical(
      duplicate_order_evaluator<value_type> duplicateOrder = {}) const;
  std::vector<std::pair<range_type, value_type>> enumerateCanonicalMove(
      duplicate_order_evaluator<value_type> duplicateOrder = {});

  // Rebuilds from an enumerateCanonical() result in exactly that order.
  // Callback dispatch is suppressed during loading; net state transitions
  // are computed only after the complete tree is present.
  void reconstructFromCanonical(
      const std::vector<std::pair<range_type, value_type>>& entries,
      duplicate_order_evaluator<value_type> duplicateOrder = {});

  using callback_registration_id = std::uint64_t;
  using callback_context = std::shared_ptr<void>;
  using registered_state_callback = std::function<void(
      const callback_context&, const value_type&,
      const Dmn_OverlayState&, const Dmn_OverlayState&)>;
  callback_registration_id registerStateCallback(
      std::function<bool(const value_type&)> matches,
      callback_context context,
      registered_state_callback callback);
  void unregisterStateCallback(callback_registration_id registration);

  bool empty() const noexcept;
  std::size_t size() const noexcept;

  // Clear
  // Removes all entries without invoking callbacks for entries that no longer exist.
  // May throw std::logic_error when called reentrantly from a callback.
  void clear();
};
```

The canonical comparator MUST define a deterministic strict weak ordering and
receives ranges only. If it considers distinct ranges equivalent, the default
range ordering `(start, end)` is the tie-breaker. The default comparator uses
range start and range end. Identical ranges are resolved by the
enumeration-time duplicate-order callback or, when that callback is empty,
insertion ordinal. A custom range comparator MUST NOT inspect payloads or rely
on transient addresses or pointers. Handlers requiring identical output MUST
use equivalent comparator configurations and duplicate-order callbacks.
Duplicate payload ordering belongs to `duplicateOrder`.
The priority evaluator must be strict: `priority_evaluator(a, b)` and
`priority_evaluator(b, a)` must not both be true.

The `const T&` insertion overload copies, while the `T&&` overload moves,
matching `Dmn_BlockingQueue::push`. The move overload is required to support
move-only payloads. Copy-returning query APIs, including `findOverlapping`, `queryTopology`, and
`enumerateCanonical`, require a copy-constructible `T`. `enumerateCanonicalMove`
transfers stored payloads and supports move-only `T`; it is non-const and
leaves the tree empty. Insertion
retains the callback for the lifetime of the entry; callback and matcher
objects must remain valid copies of their `std::function` targets.
`addWithTopology` intentionally accepts only `const T&` because its result
contains copied overlapping payloads; move-only clients use `add` and state
callbacks instead.

`enumerateCanonical` invokes `duplicateOrder(a, b)` only when two entries
have identical ranges. Returning `true` means the first opaque value belongs
before the second; returning `false` means it does not. The callback MUST
define a strict weak ordering. If it is empty, insertion ordinal orders
duplicates. The callback never orders entries with different ranges.

`reconstructFromCanonical(entries, duplicateOrder)` is a replacement
operation intended for restoring a logical topology snapshot. It validates
all entries before mutation, loads them in vector order, assigns ordinals in
that order, restores callback registrations by matching each opaque value,
updates stored states as entries are loaded so the completed tree reflects
the complete snapshot, and dispatches no callbacks caused solely by loading.
The reconstruction `duplicateOrder` parameter is not invoked; canonical order
is an input precondition, and the supplied order is preserved as the
insertion-ordinal tie-breaker. If entries came from
`enumerateCanonical(duplicateOrder)`, the postcondition is:

```text
destination.enumerateCanonical(duplicateOrder) == entries
```

The operation MUST leave the destination unchanged if validation fails.
Because callbacks are runtime registrations, they are not included in the
canonical entry vector.

For a registered callback, the first callback argument is the client-supplied
context and the second is the reconstructed entry's opaque value. The old and
new overlay states follow those arguments. The context is retained by shared
ownership until the registration is removed and any already-computed
callback dispatch completes.

Move-only payloads are supported by plain `add` and the `T&&` overload.
`findOverlapping`, `queryTopology`, and `enumerateCanonical` use
copy-returning result types and therefore require a copyable `T` when
instantiated or called. `forEachOverlapping` visits entries by const
reference and does not copy payloads; it requires a non-empty visitor and a
valid query range, and invokes the visitor in canonical order. An empty
visitor or invalid query throws `std::invalid_argument`. The visitor MUST NOT
mutate the tree or retain references after it returns. Clients with move-only
payloads must use plain `add`, state callbacks, predicates,
`forEachOverlapping`, and
`enumerateCanonicalMove`.
`enumerateCanonicalMove` is an extraction operation: it computes its complete
ordered result, moves every payload into the result, clears the tree, and
does not invoke state callbacks for the extracted entries.

`addWithTopology()` reports the new entry's initial geometric topology and
priority state. It does not itself define an application-level grant or
commit decision. Clients may use the result to initialize entry state, but
must apply their own state filtering and transition rules when stored values
have lifecycle states such as waiting, granted, or terminal.

All insertion and removal methods reject an invalid range without mutation by
throwing `std::invalid_argument`. This includes `add`,
`addWithTopology`, `remove`, `removeByRange`, and `removeAllOverlapping`.
`hasOverlap` instead returns `false` for an invalid range. `findOverlapping`,
`forEachOverlapping`, and `queryTopology` throw `std::invalid_argument` for an
invalid range; `forEachOverlapping` also throws for an empty visitor. For a
valid range with no removal match, `remove` and `removeByRange` return
`false`, and `removeAllOverlapping` returns zero. A missing priority evaluator
means `isTop` is `true` for every entry; topology remains fully supported.
Plain `add` computes the same initial topology and priority state as
`addWithTopology`, discards that state, and still performs all existing-entry
state-transition callbacks.

`queryTopology` treats its `(range, value)` argument as a hypothetical new,
higher-level entry. It does not modify the tree or invoke callbacks and
returns the state that `addWithTopology` would produce at that point.
Stored-entry state is always computed from the entry's actual insertion
level.

If a priority evaluator is supplied and two overlapping values are tied
(neither evaluator direction returns `true`), both entries are considered
top for purposes of `isTop`. The evaluator is not required to establish a
total order.

The tree is not thread-safe. Concurrent reads, or any read concurrent with a
mutation, require external synchronization.

Callbacks execute synchronously after the mutation has reached a structurally
valid state. If a callback throws, the mutation is not rolled back; the
exception propagates to the caller after the tree remains valid. A callback
must not re-enter a mutating tree operation. Reentrant mutation is rejected
by the implementation with `std::logic_error`, and is not part of the
supported API contract. The mutation guard is cleared before callback
dispatch exits, including when a callback throws, so a later independent
operation remains usable. `clear()` is potentially throwing for this reason:
it also rejects reentrant calls instead of violating `noexcept` or destroying
entries while a callback batch is using them.

### 3.3 Example

```cpp
struct LockData {
  std::string m_nodeId;
  std::uint64_t m_sequence;
};

// Setup tree with a priority evaluator (higher sequence = "on top")
Dmn_IntervalBTree<LockData>::priority_evaluator topCheck = 
    [](const LockData& a, const LockData& b) {
      return a.m_sequence > b.m_sequence;
    };

Dmn_IntervalBTree<LockData> tree({}, topCheck);

// Node B locks 5-9, Node A locks 10-15
tree.add(5, 9, {"NodeB", 1});
tree.add(10, 15, {"NodeA", 2});

// Node C attempts to lock 13-17 with a higher sequence
auto [success, result] = tree.addWithTopology({13, 17}, {"NodeC", 3});

// success == true
// result.m_status == Dmn_OverlayTopology::OverlaidLeft (left boundary 13 is within 10-15)
// result.m_isTop == true (Node C's sequence 3 > Node A's sequence 2)
// result.m_overlappingEntries contains [ ({10, 15}, {"NodeA", 2}) ]

// Querying the state later
auto queryState = tree.queryTopology({13, 17}, {"NodeC", 3});
// queryState matches the topology output from insertion
```

## 4. Internal B‑tree design

### 4.1 Node structure
```cpp
struct Node {
  std::vector<std::unique_ptr<Entry>> m_entries;
  std::vector<std::unique_ptr<Node>> m_children;
  std::int64_t m_subtreeMaxEnd{std::numeric_limits<std::int64_t>::min()};
  bool m_isLeaf{true};
};
```

`Node` and `Entry` are private implementation details, not required public
types. The owning tree stores its root as `m_root` (`std::unique_ptr`) and owns
every descendant through the node `m_children` vectors. Parent pointers and
`std::shared_ptr` are not required. An entry record keeps its range, value,
insertion ordinal, and optional callback together so they cannot become
misaligned when keys move during splits, merges, or borrowing. Nodes own
these entry records through `unique_ptr`, so structural shifts move ownership
handles rather than imposing payload copy or assignment requirements.
`m_subtreeMaxEnd` is introduced when overlap pruning is implemented. It MUST be
updated after insertion, split, merge, borrowing, and removal.

### 4.2 Insertion
- descend using canonical comparator;
- insert into leaf;
- split nodes when capacity exceeded;
- propagate splits upward;
- maintain sorted keys within nodes.
- assign an insertion ordinal that is unique within the tree;
- update callback and subtree metadata with every structural change.

The B-tree implementation MUST use a fixed minimum degree of at least two
for all nodes. The chosen degree is an implementation constant and must be
covered by structural tests; all non-root nodes must satisfy the resulting
minimum and maximum key counts.

### 4.3 Overlap queries
- check each key for overlap;
- descend into all candidate children for the initial correct implementation;
- use `m_subtreeMaxEnd` to prune only after its metadata maintenance is
  implemented and validated.

Pruning MUST never skip a child whose `m_subtreeMaxEnd` can reach the query
start. Query results are returned in canonical order.

### 4.4 Canonical enumeration
- in-order traversal of B‑tree;
- stable canonical ordering;
- deterministic output independent of tree shape.

### 4.5 State transition tracking

Topology is a property of an entry relative to the complete set of
overlapping entries, not merely its immediate predecessor or successor.
Therefore, checking only one left or right neighbor is not correct in
general. For example, two separated entries can independently cover the
left and right boundaries, and removing either one can change
`OverlaidBoth`; multiple contiguous entries can also jointly provide
`FullyCovered`.

The implementation MUST maintain the following transition behavior:

1. Before a mutating operation, capture the current `Dmn_OverlayState` for
   affected existing entries.
2. Apply the insertion or removal.
3. Recompute topology and `isTop` from the resulting complete overlap set.
4. Invoke callbacks only for existing entries whose state differs from the
   captured state.
5. Do not invoke the callback for the newly inserted entry. Its initial state
   is returned by `addWithTopology`.

For insertion, the initial affected set is every existing entry overlapping
the new range. For removal, it is every remaining entry overlapping the
removed range. This includes all entries in the range, not only the
canonical immediate left and right entries. For the current model, no
further propagation is required: an entry's callback or current topology
does not alter its range or priority, so it cannot change another entry's
state. An implementation MAY conservatively recompute all entries
overlapping the union of the changed ranges; it MUST NOT assume that only
one neighboring entry can change.

Callbacks run synchronously after the tree reaches a consistent state and
receive the entry's range, value, old state, and new state. A callback is
not invoked when the state is unchanged. Callback execution is outside
internal mutation/traversal bookkeeping; callbacks MUST NOT mutate the tree
reentrantly unless reentrancy is explicitly added as a future API guarantee.
Exceptions from callbacks MUST be handled according to the library's
non-silent error policy and must not leave the tree structurally inconsistent.

State transitions can be triggered by:

- inserting an overlapping entry, including an entry that becomes higher
  priority than an existing entry;
- removing an overlapping entry;
- removing one of several entries that jointly cover a boundary or an
  entire range;
- removing an entry that was the sole higher-priority overlap;
- any future operation that changes the priority relationship or effective
  overlap set.

`removeAllOverlapping` applies the same before/after transition processing
once for the whole batch and invokes each surviving entry's callback at most
once with its net state transition.
When multiple callbacks are fired by one operation, they run in canonical
entry order. All affected states are computed before the first callback is
invoked, so no callback observes a partially recomputed state.

## 5. Deterministic test matrix

The following names refer to the executable tests in
`test/dmn-test-interval-btree.cpp`.

### 5.1 Range and API tests
- `IntervalRange.RejectsReversed`, `IntervalRange.OverlapRejectsInvalidOperands`,
  `IntervalRange.AcceptsSinglePoint`, `IntervalRange.AcceptsNegativeValues`,
  `IntervalRange.InclusiveSharedEndpointConflicts`,
  `IntervalRange.AdjacentRangesDoNotConflict`, and
  `IntervalRange.HandlesInt64BoundariesWithoutOverflow`.
- `IntervalBTree.DefaultConstructs`, `IntervalBTree.StoresOneEntry`, and
  `IntervalBTree.RejectsInvalidInsertionWithoutMutation`.

### 5.2 Canonical order, payload, and insertion structure
- Ordering: `CanonicalOrderSimple`, `DuplicateRangesUseStableTieBreak`,
  `DuplicateRangesUseCallerOrdering`,
  `DuplicateOrderingCallbackNotCalledForDistinctRanges`,
  `DuplicateOrderingCallbackCannotMutateTreeReentrantly`,
  `CustomComparatorIsUsed`,
  `CustomComparatorEquivalentRangesUseDefaultTieBreak`, and
  `ConstructorsUseDefaultAndCustomComparators`.
- Payloads: `SupportsStringPayload`, `SupportsStructPayload`,
  `PayloadDoesNotAffectDefaultRangeOrdering`,
  `MoveOnlyPayloadCanBeInserted`,
  `InvalidMoveInsertionDoesNotConsumePayload`, and
  `CopyAndMoveInsertionOverloads`.
- Structure: `RootStoragePreservesMultipleEntries`, `NodeStoresMultipleKeys`,
  `RootSplitCreatesOwnedRoot`, `NodeChildrenPartitionCorrectly`,
  `InsertionSplitsNodes`, `InsertionDeterministicAcrossOrders`,
  `EnumerationIsIndependentOfNodeSplits`, and
  `SplitPreservesEntryValuesAndOrdinals`.

### 5.3 Overlap and topology tests
- Overlap queries: `FindOverlappingSingle`,
  `FindOverlappingMultipleInCanonicalOrder`,
  `FindOverlappingIncludesSharedEndpoints`,
  `FindOverlappingRejectsInvalidQuery`, `HasOverlapRejectsInvalidQuery`,
  `HasOverlapFindsAndRejectsNonOverlappingRanges`,
  `OverlapVisitorAvoidsPayloadCopies`,
  `OverlapVisitorRejectsInvalidQueryWithoutInvocation`,
  `OverlapVisitorRequiresCallableVisitor`,
  `OverlapVisitorSupportsMoveOnlyPayload`, and
  `OverlapVisitorCannotMutateTreeReentrantly`.
- Topology queries: `TopologyReturnsClearWhenEmpty`,
  `TopologyIdentifiesOverlaidLeftAndRight`, `TopologyIdentifiesFullyCovered`,
  `TopologyPrecedenceIsDeterministic`, `TopologyExactMatchIsFullyCovered`,
  `TopologyIdentifiesCoveringExisting`,
  `TopologyHandlesContiguousCoverageAndCoverageGaps`,
  `TopologyHandlesInt64BoundariesWithoutOverflow`,
  `QueryTopologyIsHypothetical`,
  `TopologyPriorityUsesEvaluatorAndDefaultsToTop`, and
  `InvalidTopologyQueryReturnsClearWithoutMutation`.

### 5.4 State callback and mutation tests
- Insertion transitions: `AddWithTopologyMatchesHypotheticalQuery`,
  `NewEntryCallbackIsNotInvokedOnInsertion`,
  `StateCallbackReceivesTopologyTransition`,
  `StateCallbackReceivesPriorityTransition`,
  `MoveOnlyPayloadSupportsStateCallback`,
  `RangeEndpointInsertionOverloadsForwardCallbacks`,
  `InsertionNotifiesNonAdjacentAffectedEntries`,
  `InsertionCallbacksFollowCanonicalOrder`,
  `TopologyUsesInsertionLevelForContainment`, and
  `ExactDuplicateInsertionKeepsBothEntriesFullyCovered`.
- Removal: `RemoveByRangeRemovesOneExactEntry`,
  `InvalidAndMissingRemovalLeaveTreeUnchanged`,
  `RemoveByRangeMatchesDuplicateByOpaqueValue`,
  `FailedRemovalPredicateLeavesTreeUnchanged`,
  `RemoveAliasMatchesRemoveByRange`,
  `RemoveAllOverlappingReturnsExactCount`,
  `RemovalRebalancesBTree`,
  `RemovalMaintainsInvariantsAcrossMixedOrders`,
  `RemovalSupportsMoveOnlyPayloads`,
  `RemovalRecomputesSurvivorTopologyAndPriority`,
  `RemovalNotifiesFormerlyOverlaidEntry`,
  `BatchRemovalNotifiesEachSurvivorOnce`,
  `RemovalCallbackRunsAfterTreeMutation`,
  `RemovalCallbackExceptionLeavesTreeValid`,
  `RemovalCallbacksFollowCanonicalOrder`, and
  `RemovalPredicateCannotMutateTreeReentrantly`.
- Callback and clear safety: `CallbackExceptionLeavesTreeConsistentAndReusable`,
  `ReentrantMoveEnumerationFromCallbackIsRejected`,
  `ReentrantInsertionFromCallbackIsRejected`,
  `ClearSuppressesCallbacksAndAllowsReuse`, and
  `ReentrantClearFromCallbackIsRejected`.

### 5.5 Metadata, extraction, reconstruction, and lifecycle
- Metadata/pruning: `SubtreeMaxEndRemainsCorrectAcrossMutations`,
  `Int64BoundarySubtreeMaxEndRemainsCorrect`, and
  `PrunedOverlapQueriesMatchCanonicalBaseline`.
- Move extraction: `MoveOnlyPayloadCanBeEnumeratedByMove`,
  `MoveEnumerationUsesDuplicateOrdering`, and
  `MoveEnumerationDoesNotDispatchCallbacks`.
- Reconstruction/registration: `CanonicalReconstructionPreservesTopology`,
  `ReconstructionValidationFailureLeavesTreeUnchanged`,
  `ReconstructionReconnectsRegisteredCallbacks`,
  `UnregisterCallbackStopsFutureDispatch`,
  `ReconstructionUsesFirstMatchingRegistration`,
  `ReconstructionPreservesDuplicateCanonicalOrder`, and
  `RebuildMatchesOriginalCanonicalOrder`.
- Destruction: `DestructorReleasesAllUniqueOwnedEntries`; the registered
  Valgrind test additionally checks for leaks and invalid memory use.

### 5.6 Public API edge cases

- `EmptyTreeQueriesAndRemovalsAreNoOps` checks valid empty-tree queries,
  visitors, and no-op removals.
- `EndpointInsertionRejectsInvalidRangesBeforeMutation` checks endpoint
  overload validation and preservation of an rvalue payload on failure.
- `EmptyMatcherRemovesFirstDuplicateInInsertionOrder` verifies default
  duplicate selection.
- `CallbackRegistrationValidatesFunctionsAndIgnoresUnknownId` covers empty
  callback/matcher rejection and unknown unregistration.
- `UserComparatorsCannotMutateTreeReentrantly` checks custom range comparator
  reentrancy protection.
- `PriorityEvaluatorCannotMutateTreeReentrantly` checks priority callback
  reentrancy protection.

## 6. Definition of done

- All range semantics match DLock invariants.
- Canonical ordering is stable and deterministic.
- B‑tree insertion and removal maintain structural invariants.
- Overlap queries return exactly the overlapping entries.
- Duplicate ranges can be selected deterministically by an opaque-value
  predicate.
- Existing-entry callbacks report every applicable net state transition and
  never report the initial state of a newly inserted entry.
- Template payload support is complete.
- API follows DMN coding conventions.
- Copy-returning APIs, reference visitor APIs, callback registration, and
  reconstruction validation have explicit coverage in the implementation
  plan.
- Documentation includes examples and test references.
