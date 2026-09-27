/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-interval-btree.hpp
 * @brief Template B-tree for indexing inclusive intervals and their values.
 *
 * Overview
 * --------
 * @ref Dmn_IntervalBTree is an in-memory, degree-2 B-tree that stores
 * inclusive @ref Dmn_IntervalRange keys alongside opaque payloads. It
 * supports canonical enumeration, overlap queries, optional topology and
 * priority evaluation, per-entry state-change callbacks, deletion, and
 * reconstruction from canonical snapshots. The implementation is private to
 * this header so each payload type can instantiate the complete template.
 *
 * Ordering and determinism
 * ------------------------
 * By default, distinct ranges are ordered by ascending start and then
 * ascending end; payloads never participate in range ordering. A custom
 * range comparator can replace that order, with the default range order used
 * to break equivalence between distinct ranges. Equal ranges are ordered by
 * insertion ordinal unless an enumeration call supplies a duplicate-order
 * evaluator. Callers that need identical snapshots across trees must supply
 * compatible range and duplicate-order comparators.
 *
 * Ownership and payload requirements
 * -----------------------------------
 * Nodes and entry records are uniquely owned by the tree. Entries retain
 * their payload, ordinal, and runtime callback together while B-tree
 * operations rebalance nodes. Lvalue insertion copies the payload; rvalue
 * insertion moves it, allowing move-only payloads. Copy-returning operations
 * require a copy-constructible payload when used. @ref forEachOverlapping()
 * visits payloads by const reference, while @ref enumerateCanonicalMove()
 * extracts payloads and empties the tree.
 *
 * Topology and callbacks
 * ----------------------
 * @ref queryTopology() computes a hypothetical insertion result without
 * changing the tree. @ref addWithTopology() returns the same initial
 * classification while inserting; the new entry's callback is not called for
 * its initial state. Existing entries are notified only when their topology
 * or priority state changes. Callbacks are synchronous, run after the
 * mutation is structurally complete, and propagate exceptions without
 * rolling back that mutation. Reentrant mutation is rejected with
 * @c std::logic_error.
 *
 * Thread safety
 * -------------
 * The tree is not thread-safe. Callers must externally synchronize all
 * concurrent reads and mutations. User-supplied visitors, comparators,
 * matchers, and callbacks must not mutate the tree reentrantly.
 *
 * Implementation notes
 * --------------------
 * Overlap traversal is pruned using each node's maximum endpoint
 * (@c m_subtreeMaxEnd). Insertions and deletions maintain this metadata along
 * with the B-tree invariants. Canonical enumeration is independent of the
 * tree's physical shape.
 */

#ifndef DMN_INTERVAL_BTREE_HPP_
#define DMN_INTERVAL_BTREE_HPP_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace dmn {

namespace detail {
template <class T> struct Dmn_IntervalBTreeTestAccess;
} // namespace detail

/**
 * @struct Dmn_IntervalRange
 * @brief Inclusive interval with signed 64-bit endpoints.
 *
 * A range is valid when @c m_start is no greater than @c m_end. Two valid
 * ranges overlap when they share at least one endpoint or interior point;
 * adjacent but non-overlapping ranges remain distinct.
 */
struct Dmn_IntervalRange {
  /** @brief Inclusive lower endpoint of the range. */
  std::int64_t m_start{};
  /** @brief Inclusive upper endpoint of the range. */
  std::int64_t m_end{};

  /**
   * @brief Return whether the endpoints describe a valid range.
   * @return @c true exactly when @c m_start <= @c m_end.
   */
  auto isValid() const noexcept -> bool;

  /**
   * @brief Test inclusive overlap with another range.
   *
   * Invalid ranges never overlap. The implementation uses comparisons and
   * does not calculate endpoint successors or predecessors.
   *
   * @param other Range to compare with this range.
   * @return @c true when both ranges are valid and share at least one point.
   */
  auto overlaps(const Dmn_IntervalRange &other) const noexcept -> bool;
}; // struct Dmn_IntervalRange

/**
 * @enum Dmn_OverlayTopology
 * @brief Geometric relationship between a candidate interval and overlaps.
 *
 * The classifier gives precedence to @c Clear, @c FullyCovered,
 * @c CoveringExisting, and @c OverlaidBoth, then reports a one-sided overlay.
 * Exact duplicates are @c FullyCovered. For stored entries, strict
 * containment is interpreted relative to insertion level.
 */
enum class Dmn_OverlayTopology {
  Clear,           ///< The candidate has no overlapping entries.
  OverlaidLeft,    ///< Only the candidate's left boundary is covered.
  OverlaidRight,   ///< Only the candidate's right boundary is covered.
  OverlaidBoth,    ///< Both boundaries are covered, with a gap between.
  FullyCovered,    ///< Existing ranges continuously cover the candidate.
  CoveringExisting ///< The candidate strictly contains existing ranges.
};

/**
 * @struct Dmn_TopologyResult
 * @brief Initial topology, priority, and overlap snapshot for a query/add.
 * @tparam T Payload type stored with each interval.
 */
template <class T> struct Dmn_TopologyResult {
  /** @brief Geometric classification of the candidate interval. */
  Dmn_OverlayTopology m_status{Dmn_OverlayTopology::Clear};
  /** @brief Whether the candidate is top-ranked among its overlaps. */
  bool m_isTop{true};
  /** @brief Canonically ordered copies of all overlapping range/value pairs. */
  std::vector<std::pair<Dmn_IntervalRange, T>> m_overlappingEntries;
};

/**
 * @struct Dmn_OverlayState
 * @brief Topology and priority state recorded for a stored entry.
 */
struct Dmn_OverlayState {
  /** @brief Current geometric relationship to overlapping entries. */
  Dmn_OverlayTopology m_topology{Dmn_OverlayTopology::Clear};
  /** @brief True when no overlapping entry has strictly higher priority. */
  bool m_isTop{true};
};

/**
 * @brief Predicate used to select an entry by its opaque payload.
 * @tparam T Payload type.
 */
template <class T> using entry_matcher = std::function<bool(const T &)>;

/**
 * @brief Strict weak ordering used to order values of identical ranges.
 * @tparam T Payload type.
 */
template <class T>
using duplicate_order_evaluator = std::function<bool(const T &, const T &)>;

/**
 * @brief Callback invoked when an existing entry's overlay state changes.
 * @tparam T Payload type.
 */
template <class T>
using state_change_callback =
    std::function<void(const Dmn_IntervalRange &, const T &,
                       const Dmn_OverlayState &, const Dmn_OverlayState &)>;

/**
 * @class Dmn_IntervalBTree
 * @brief Own and query a balanced index of inclusive ranges and payloads.
 *
 * @tparam T Payload type associated with each range.
 *
 * The class is non-copyable and non-movable. It stores duplicate ranges and
 * uses an insertion ordinal as their default stable tie-breaker. A custom
 * canonical comparator affects the ordering of distinct ranges; a priority
 * evaluator is independent of canonical ordering.
 *
 * Overlap result APIs return entries in canonical order. Invalid input ranges
 * are rejected by insertion and removal, and produce empty/false query
 * results. The tree does not impose domain constraints such as non-negative
 * endpoints.
 *
 * @note Not thread-safe. External synchronization is required.
 */
template <class T> class Dmn_IntervalBTree {
public:
  /** @brief Payload type associated with each range. */
  using value_type = T;
  /** @brief Inclusive signed 64-bit interval key type. */
  using range_type = Dmn_IntervalRange;
  /** @brief Ordering callback used only for payloads of identical ranges. */
  using duplicate_order_evaluator = dmn::duplicate_order_evaluator<value_type>;
  /** @brief Strict weak ordering over ranges, independent of payload values. */
  using canonical_comparator =
      std::function<bool(const range_type &, const range_type &)>;
  /** @brief Returns true when its first payload has strictly higher priority.
   */
  using priority_evaluator =
      std::function<bool(const value_type &, const value_type &)>;
  /** @brief Callback for changes to an existing entry's overlay state. */
  using state_change_callback = dmn::state_change_callback<value_type>;
  /** @brief Predicate for selecting an exact-range entry by its payload. */
  using entry_matcher = dmn::entry_matcher<value_type>;
  /** @brief Callback receiving matching ranges and payloads by const reference.
   */
  using overlap_visitor =
      std::function<void(const range_type &, const value_type &)>;
  /** @brief Opaque identifier returned when registering a state callback. */
  using callback_registration_id = std::uint64_t;
  /** @brief Shared client context retained by a callback registration. */
  using callback_context = std::shared_ptr<void>;
  /** @brief Registered callback receiving context, payload, and old/new state.
   */
  using registered_state_callback =
      std::function<void(const callback_context &, const value_type &,
                         const Dmn_OverlayState &, const Dmn_OverlayState &)>;

  /** @brief Construct an empty tree using the default range ordering. */
  Dmn_IntervalBTree() noexcept = default;

  /**
   * @brief Construct an empty tree with a custom range comparator.
   *
   * The comparator must define a deterministic strict weak ordering over
   * ranges only. Equivalent distinct ranges use the default endpoint order
   * as a tie-breaker.
   *
   * @param comparator Custom ordering for distinct ranges; may be empty.
   */
  explicit Dmn_IntervalBTree(canonical_comparator comparator)
      : m_canonicalComparator(std::move(comparator)) {}

  /**
   * @brief Construct an empty tree with range and priority evaluators.
   * @param comparator Strict weak ordering over ranges; may be empty.
   * @param priorityEvaluator Returns true when its first value is strictly
   *        higher priority than its second; ties are allowed.
   */
  Dmn_IntervalBTree(canonical_comparator comparator,
                    priority_evaluator priorityEvaluator)
      : m_canonicalComparator(std::move(comparator)),
        m_priorityEvaluator(std::move(priorityEvaluator)) {}

  /** @brief Copying a tree is disabled because it uniquely owns its nodes. */
  Dmn_IntervalBTree(const Dmn_IntervalBTree &) = delete;
  /** @brief Copy assignment is disabled. */
  Dmn_IntervalBTree &operator=(const Dmn_IntervalBTree &) = delete;
  /** @brief Moving a tree is disabled. */
  Dmn_IntervalBTree(Dmn_IntervalBTree &&) = delete;
  /** @brief Move assignment is disabled. */
  Dmn_IntervalBTree &operator=(Dmn_IntervalBTree &&) = delete;

  /** @brief Destroy the tree and release all uniquely owned nodes and entries.
   */
  ~Dmn_IntervalBTree() noexcept;

  /**
   * @brief Insert a copy of a payload for a valid range.
   * @param range Inclusive interval to index.
   * @param value Payload to copy into the tree.
   * @param onStateChange Optional callback for later state changes to this
   *        entry; it is not called for the entry's initial state.
   * @return @c true on insertion, or @c false for an invalid range.
   * @throws std::logic_error if invoked reentrantly from a user callback.
   */
  bool add(range_type range, const value_type &value,
           state_change_callback onStateChange = {});

  /**
   * @brief Move a payload into the tree for a valid range.
   * @param range Inclusive interval to index.
   * @param value Payload to move into the tree.
   * @param onStateChange Optional callback for later state changes.
   * @return @c true on insertion, or @c false for an invalid range.
   */
  bool add(range_type range, value_type &&value,
           state_change_callback onStateChange = {});

  /**
   * @brief Insert a copied payload using endpoint arguments.
   * @param start Inclusive lower endpoint.
   * @param end Inclusive upper endpoint.
   * @param value Payload to copy.
   * @param onStateChange Optional callback for later state changes.
   * @return @c true on insertion, or @c false for a reversed range.
   */
  bool add(std::int64_t start, std::int64_t end, const value_type &value,
           state_change_callback onStateChange = {}) {
    return add(range_type{start, end}, value, std::move(onStateChange));
  }

  /**
   * @brief Insert a moved payload using endpoint arguments.
   * @param start Inclusive lower endpoint.
   * @param end Inclusive upper endpoint.
   * @param value Payload to move.
   * @param onStateChange Optional callback for later state changes.
   * @return @c true on insertion, or @c false for a reversed range.
   */
  bool add(std::int64_t start, std::int64_t end, value_type &&value,
           state_change_callback onStateChange = {}) {
    return add(range_type{start, end}, std::move(value),
               std::move(onStateChange));
  }

  /**
   * @brief Query a candidate's state and insert it if its range is valid.
   *
   * The returned overlap list and topology describe the candidate before it
   * is inserted. Its callback is retained for later state changes but is
   * never invoked for this initial state.
   *
   * @param range Candidate inclusive interval.
   * @param value Payload to copy into the tree.
   * @param onStateChange Optional callback for later state changes.
   * @return A pair of insertion success and the candidate's initial topology
   *         result. A failed insertion returns a default result.
   * @throws std::logic_error if invoked reentrantly from a user callback.
   */
  std::pair<bool, Dmn_TopologyResult<value_type>>
  addWithTopology(range_type range, const value_type &value,
                  state_change_callback onStateChange = {});

  /**
   * @brief Remove the first matching entry with exactly equal endpoints.
   * @param range Exact interval to match.
   * @param matcher Optional payload predicate; an empty matcher selects the
   *        first exact-range entry in canonical order.
   * @return @c true if one entry was removed.
   */
  bool remove(range_type range, entry_matcher matcher = {});

  /**
   * @brief Remove one exact-range entry; equivalent to @ref remove().
   * @param range Exact interval to match.
   * @param matcher Optional payload predicate.
   * @return @c true if one entry was removed.
   */
  bool removeByRange(range_type range, entry_matcher matcher = {});

  /**
   * @brief Remove every entry overlapping a valid range.
   * @param range Interval used to select entries.
   * @return Number of removed entries, or zero for an invalid range/no match.
   */
  std::size_t removeAllOverlapping(range_type range);

  /**
   * @brief Remove all entries without notifying callbacks for destroyed
   *        entries.
   * @throws std::logic_error if invoked reentrantly from a user callback.
   */
  void clear();

  /** @brief Return whether the tree contains no entries. */
  bool empty() const noexcept;

  /** @brief Return the number of stored entries, including duplicate ranges. */
  std::size_t size() const noexcept;

  /**
   * @brief Return a copy of all entries in canonical order.
   *
   * The duplicate-order evaluator is called only for entries with identical
   * ranges. Without it, their insertion ordinals determine their order.
   *
   * @param duplicateOrder Optional strict weak ordering for equal-range
   *        payloads.
   * @return Canonically ordered range/value copies.
   * @note Requires a copy-constructible @c T when called.
   * @throws std::logic_error if a supplied ordering callback attempts
   *         reentrant mutation.
   */
  std::vector<std::pair<range_type, value_type>>
  enumerateCanonical(duplicate_order_evaluator duplicateOrder = {}) const;

  /**
   * @brief Move entries out in canonical order and leave the tree empty.
   *
   * @param duplicateOrder Optional strict weak ordering for equal-range
   *        payloads.
   * @return Canonically ordered range/value pairs owning the extracted
   *         payloads.
   * @throws std::logic_error if invoked reentrantly from a user callback or
   *         if the ordering callback attempts mutation.
   */
  std::vector<std::pair<range_type, value_type>>
  enumerateCanonicalMove(duplicate_order_evaluator duplicateOrder = {});

  /**
   * @brief Replace the tree contents from a canonical snapshot.
   *
   * Every range is validated before the current contents are replaced.
   * Entries are loaded in supplied order with new insertion ordinals, and
   * registered callbacks are reconnected by the first matching registration.
   * Loading does not dispatch state-change callbacks.
   *
   * @param entries Range/value snapshot to load.
   * @param duplicateOrder Reserved for symmetry with canonical enumeration;
   *        it is not invoked while loading. The supplied vector order becomes
   *        the insertion-ordinal order for equal ranges. Pass the evaluator
   *        to subsequent enumeration when payload-defined duplicate order is
   *        required.
   * @throws std::invalid_argument if any range is invalid; the current tree
   *         remains unchanged.
   * @throws std::logic_error if invoked reentrantly.
   */
  void reconstructFromCanonical(
      const std::vector<std::pair<range_type, value_type>> &entries,
      duplicate_order_evaluator duplicateOrder = {});

  /**
   * @brief Register a callback to reconnect to matching reconstructed entries.
   *
   * Registrations are considered in insertion order, and the first matcher
   * returning true is used. The context is retained by shared ownership.
   *
   * @param matches Predicate over the opaque payload; must be callable.
   * @param context Client-owned callback context.
   * @param callback Callback invoked on a later state transition.
   * @return Nonzero registration identifier for later unregistration.
   * @throws std::invalid_argument if either function is empty.
   * @throws std::overflow_error if no registration identifiers remain.
   */
  callback_registration_id
  registerStateCallback(std::function<bool(const value_type &)> matches,
                        callback_context context,
                        registered_state_callback callback);

  /**
   * @brief Unregister a callback and detach it from currently stored entries.
   * @param registration Identifier returned by @ref registerStateCallback().
   *        Unknown identifiers are ignored.
   */
  void unregisterStateCallback(callback_registration_id registration);

  /**
   * @brief Return whether any stored interval overlaps a query interval.
   * @param range Inclusive query interval.
   * @return @c true if a valid query overlaps at least one stored range.
   */
  bool hasOverlap(range_type range) const noexcept;

  /**
   * @brief Return copies of all overlapping entries in canonical order.
   * @param range Inclusive query interval.
   * @return Empty for an invalid query or no matches; otherwise matching
   *         range/value copies in canonical order.
   * @note Requires a copy-constructible @c T when called.
   */
  std::vector<std::pair<range_type, value_type>>
  findOverlapping(range_type range) const;

  /**
   * @brief Visit overlapping entries in canonical order without copying.
   *
   * @param range Inclusive query interval.
   * @param visitor Callable receiving const references to each range and
   *        payload. References must not be retained after the call.
   * @throws std::invalid_argument if @p visitor is empty, including for an
   *         invalid query.
   * @throws std::logic_error if the visitor attempts reentrant mutation.
   */
  void forEachOverlapping(range_type range, overlap_visitor visitor) const;

  /**
   * @brief Compute the topology of a hypothetical higher-level entry.
   *
   * Does not mutate the tree or invoke state-change callbacks. If no priority
   * evaluator was configured, @c m_isTop is true.
   *
   * @param range Candidate inclusive interval.
   * @param value Candidate payload used by the priority evaluator.
   * @return Topology, priority status, and canonically ordered overlap copies.
   * @note Requires a copy-constructible @c T when called.
   */
  Dmn_TopologyResult<value_type> queryTopology(range_type range,
                                               const value_type &value) const;

private:
  struct MutationBlockGuard {
    explicit MutationBlockGuard(bool &blocked)
        : m_blocked(blocked), m_wasBlocked(blocked) {
      m_blocked = true;
    }

    ~MutationBlockGuard() { m_blocked = m_wasBlocked; }

    bool &m_blocked;
    bool m_wasBlocked;
  };

  static auto classifyTopology(range_type range,
                               const std::vector<range_type> &overlaps)
      -> Dmn_OverlayTopology {
    if (overlaps.empty()) {
      return Dmn_OverlayTopology::Clear;
    }

    std::vector<range_type> clippedRanges;
    clippedRanges.reserve(overlaps.size());

    for (const auto &overlap : overlaps) {
      clippedRanges.push_back({std::max(range.m_start, overlap.m_start),
                               std::min(range.m_end, overlap.m_end)});
    }

    std::sort(clippedRanges.begin(), clippedRanges.end(),
              [](const range_type &lhs, const range_type &rhs) {
                if (lhs.m_start != rhs.m_start) {
                  return lhs.m_start < rhs.m_start;
                }

                return lhs.m_end < rhs.m_end;
              });

    bool leftCovered = clippedRanges.front().m_start == range.m_start;
    bool rightCovered = false;
    auto coveredEnd = clippedRanges.front().m_end;
    bool continuousCoverage = leftCovered;

    for (std::size_t index = 1; index < clippedRanges.size(); ++index) {
      const auto &next = clippedRanges[index];

      if (coveredEnd != std::numeric_limits<std::int64_t>::max() &&
          next.m_start > coveredEnd + 1) {
        continuousCoverage = false;
      }

      coveredEnd = std::max(coveredEnd, next.m_end);
    }

    rightCovered = coveredEnd == range.m_end;
    if (continuousCoverage && rightCovered) {
      return Dmn_OverlayTopology::FullyCovered;
    }

    auto overlapStart = overlaps.front().m_start;
    auto overlapEnd = overlaps.front().m_end;
    bool hasContainedRange = false;

    for (const auto &overlap : overlaps) {
      overlapStart = std::min(overlapStart, overlap.m_start);
      overlapEnd = std::max(overlapEnd, overlap.m_end);
      hasContainedRange =
          hasContainedRange ||
          (range.m_start < overlap.m_start && overlap.m_end < range.m_end);
    }

    if (hasContainedRange && range.m_start < overlapStart &&
        range.m_end > overlapEnd) {
      return Dmn_OverlayTopology::CoveringExisting;
    }

    if (leftCovered && rightCovered) {
      return Dmn_OverlayTopology::OverlaidBoth;
    }

    if (leftCovered) {
      return Dmn_OverlayTopology::OverlaidLeft;
    }

    if (rightCovered) {
      return Dmn_OverlayTopology::OverlaidRight;
    }

    return Dmn_OverlayTopology::Clear;
  }

  static bool defaultRangeLess(const range_type &lhs,
                               const range_type &rhs) noexcept {
    if (lhs.m_start != rhs.m_start) {
      return lhs.m_start < rhs.m_start;
    }

    return lhs.m_end < rhs.m_end;
  }

  struct Entry {
    range_type m_range;
    value_type m_value;
    std::size_t m_ordinal;
    state_change_callback m_onStateChange;
    callback_registration_id m_callbackRegistrationId{};
    Dmn_OverlayState m_state;
  };

  struct CallbackRegistration {
    callback_registration_id m_id;
    std::function<bool(const value_type &)> m_matches;
    callback_context m_context;
    registered_state_callback m_callback;
  };

  struct Node {
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::vector<std::unique_ptr<Node>> m_children;
    std::int64_t m_subtreeMaxEnd{std::numeric_limits<std::int64_t>::min()};
    bool m_isLeaf{true};
  };

  static constexpr std::size_t kMinimumDegree = 2;

  auto entryLess(const Entry &lhs, const Entry &rhs) const -> bool {
    const bool identicalRanges = lhs.m_range.m_start == rhs.m_range.m_start &&
                                 lhs.m_range.m_end == rhs.m_range.m_end;

    if (identicalRanges) {
      return lhs.m_ordinal < rhs.m_ordinal;
    }

    if (m_canonicalComparator) {
      MutationBlockGuard guard(m_dispatchingCallbacks);

      if (m_canonicalComparator(lhs.m_range, rhs.m_range)) {
        return true;
      }

      if (m_canonicalComparator(rhs.m_range, lhs.m_range)) {
        return false;
      }
    }

    return defaultRangeLess(lhs.m_range, rhs.m_range);
  }

  static void collectEntriesInOrder(const Node *node,
                                    std::vector<const Entry *> &entries) {
    if (node->m_isLeaf) {
      for (const auto &entry : node->m_entries) {
        entries.push_back(entry.get());
      }

      return;
    }

    for (std::size_t index = 0; index < node->m_entries.size(); ++index) {
      collectEntriesInOrder(node->m_children[index].get(), entries);
      entries.push_back(node->m_entries[index].get());
    }

    collectEntriesInOrder(node->m_children.back().get(), entries);
  }

  static void collectEntriesInOrder(Node *node, std::vector<Entry *> &entries) {
    if (node->m_isLeaf) {
      for (const auto &entry : node->m_entries) {
        entries.push_back(entry.get());
      }

      return;
    }

    for (std::size_t index = 0; index < node->m_entries.size(); ++index) {
      collectEntriesInOrder(node->m_children[index].get(), entries);
      entries.push_back(node->m_entries[index].get());
    }

    collectEntriesInOrder(node->m_children.back().get(), entries);
  }

  auto calculateState(const Entry *candidate) const -> Dmn_OverlayState {
    Dmn_OverlayState state;

    std::vector<range_type> overlaps;
    std::vector<const Entry *> entries;
    entries.reserve(m_size);

    if (m_root) {
      collectEntriesInOrder(m_root.get(), entries);
    }

    for (const Entry *entry : entries) {
      if (entry != candidate && candidate->m_range.overlaps(entry->m_range)) {
        overlaps.push_back(entry->m_range);
        if (m_priorityEvaluator) {
          MutationBlockGuard guard(m_dispatchingCallbacks);
          if (m_priorityEvaluator(entry->m_value, candidate->m_value)) {
            state.m_isTop = false;
          }
        }
      }
    }

    state.m_topology = classifyTopology(candidate->m_range, overlaps);
    return state;
  }

  auto refreshNodeMaxEnd(Node *node) -> std::int64_t {
    auto maximumEnd = std::numeric_limits<std::int64_t>::min();
    for (const auto &entry : node->m_entries) {
      maximumEnd = std::max(maximumEnd, entry->m_range.m_end);
    }

    for (const auto &child : node->m_children) {
      maximumEnd = std::max(maximumEnd, child->m_subtreeMaxEnd);
    }

    node->m_subtreeMaxEnd = maximumEnd;

    return maximumEnd;
  }

  auto insertEntry(std::unique_ptr<Entry> entry) -> bool {
    if (!entry->m_range.isValid()) {
      throw std::invalid_argument("range is invalid");
    }

    std::vector<Entry *> existingEntries;
    std::vector<std::pair<Entry *, Dmn_OverlayState>> oldStates;
    existingEntries.reserve(m_size);

    if (m_root) {
      collectEntriesInOrder(m_root.get(), existingEntries);
    }

    for (Entry *existing : existingEntries) {
      if (existing->m_range.overlaps(entry->m_range)) {
        oldStates.emplace_back(existing, existing->m_state);
      }
    }

    auto *insertedEntry = entry.get();
    if (!m_root) {
      m_root = std::make_unique<Node>();
    }

    if (m_root->m_entries.size() == 2 * kMinimumDegree - 1) {
      auto newRoot = std::make_unique<Node>();

      newRoot->m_isLeaf = false;
      newRoot->m_children.reserve(1);
      newRoot->m_children.push_back(std::move(m_root));

      try {
        splitChild(*newRoot, 0);
      } catch (...) {
        m_root = std::move(newRoot->m_children.front());
        throw;
      }

      m_root = std::move(newRoot);
    }

    insertNonFull(*m_root, std::move(entry));
    ++m_nextOrdinal;
    ++m_size;

    insertedEntry->m_state = calculateState(insertedEntry);
    updateStatesAndNotify(oldStates);

    return true;
  }

  void updateStatesAndNotify(
      const std::vector<std::pair<Entry *, Dmn_OverlayState>> &oldStates) {
    std::vector<std::tuple<Entry *, Dmn_OverlayState, Dmn_OverlayState>>
        transitions;

    for (const auto &[entry, oldState] : oldStates) {
      const auto newState = calculateState(entry);
      entry->m_state = newState;

      if (oldState.m_topology != newState.m_topology ||
          oldState.m_isTop != newState.m_isTop) {
        transitions.emplace_back(entry, oldState, newState);
      }
    }

    if (transitions.empty()) {
      return;
    }

    if (m_suppressCallbacks) {
      return;
    }

    {
      MutationBlockGuard guard(m_dispatchingCallbacks);

      for (const auto &[entry, oldState, newState] : transitions) {
        if (entry->m_onStateChange) {
          entry->m_onStateChange(entry->m_range, entry->m_value, oldState,
                                 newState);
        }
      }
    }
  }

  auto findExactEntry(range_type range,
                      const entry_matcher &matcher) -> Entry * {
    std::vector<Entry *> entries;
    entries.reserve(m_size);

    if (m_root) {
      collectEntriesInOrder(m_root.get(), entries);
    }

    for (Entry *entry : entries) {
      if (entry->m_range.m_start == range.m_start &&
          entry->m_range.m_end == range.m_end) {
        bool matches = true;

        if (matcher) {
          MutationBlockGuard guard(m_dispatchingCallbacks);
          matches = matcher(entry->m_value);
        }

        if (matches) {
          return entry;
        }
      }
    }

    return nullptr;
  }

  auto callbackForReconstruction(const value_type &value,
                                 callback_registration_id &registrationId)
      -> state_change_callback {
    for (const auto &registration : m_callbackRegistrations) {
      bool matches = false;

      {
        MutationBlockGuard guard(m_dispatchingCallbacks);
        matches = registration.m_matches(value);
      }

      if (matches) {
        registrationId = registration.m_id;
        const auto context = registration.m_context;
        const auto callback = registration.m_callback;

        return [context, callback](const range_type &, const value_type &item,
                                   const Dmn_OverlayState &oldState,
                                   const Dmn_OverlayState &newState) {
          callback(context, item, oldState, newState);
        };
      }
    }

    registrationId = 0;

    return {};
  }

  static auto containsEntry(const std::vector<Entry *> &entries,
                            const Entry *target) -> bool {
    return std::find(entries.begin(), entries.end(), target) != entries.end();
  }

  auto removeMaximum(Node &node) -> std::unique_ptr<Entry> {
    if (node.m_isLeaf) {
      auto entry = std::move(node.m_entries.back());
      node.m_entries.pop_back();
      refreshNodeMaxEnd(&node);

      return entry;
    }

    auto childIndex = node.m_children.size() - 1;
    if (node.m_children[childIndex]->m_entries.size() == kMinimumDegree - 1) {
      if (childIndex > 0 &&
          node.m_children[childIndex - 1]->m_entries.size() >= kMinimumDegree) {
        borrowFromPrevious(node, childIndex);
      } else if (childIndex > 0) {
        mergeChildren(node, childIndex - 1);
        --childIndex;
      }
    }

    auto entry = removeMaximum(*node.m_children[childIndex]);
    refreshNodeMaxEnd(&node);

    return entry;
  }

  auto removeMinimum(Node &node) -> std::unique_ptr<Entry> {
    if (node.m_isLeaf) {
      auto entry = std::move(node.m_entries.front());
      node.m_entries.erase(node.m_entries.begin());
      refreshNodeMaxEnd(&node);

      return entry;
    }

    constexpr std::size_t childIndex = 0;
    if (node.m_children[childIndex]->m_entries.size() == kMinimumDegree - 1) {
      if (node.m_children.size() > 1 &&
          node.m_children[1]->m_entries.size() >= kMinimumDegree) {
        borrowFromNext(node, childIndex);
      } else if (node.m_children.size() > 1) {
        mergeChildren(node, childIndex);
      }
    }

    auto entry = removeMinimum(*node.m_children[childIndex]);
    refreshNodeMaxEnd(&node);

    return entry;
  }

  void borrowFromPrevious(Node &parent, std::size_t childIndex) {
    Node &child = *parent.m_children[childIndex];
    Node &sibling = *parent.m_children[childIndex - 1];
    child.m_entries.insert(child.m_entries.begin(),
                           std::move(parent.m_entries[childIndex - 1]));
    parent.m_entries[childIndex - 1] = std::move(sibling.m_entries.back());
    sibling.m_entries.pop_back();

    if (!child.m_isLeaf) {
      child.m_children.insert(child.m_children.begin(),
                              std::move(sibling.m_children.back()));
      sibling.m_children.pop_back();
    }

    refreshNodeMaxEnd(&child);
    refreshNodeMaxEnd(&sibling);
    refreshNodeMaxEnd(&parent);
  }

  void borrowFromNext(Node &parent, std::size_t childIndex) {
    Node &child = *parent.m_children[childIndex];
    Node &sibling = *parent.m_children[childIndex + 1];
    child.m_entries.push_back(std::move(parent.m_entries[childIndex]));
    parent.m_entries[childIndex] = std::move(sibling.m_entries.front());
    sibling.m_entries.erase(sibling.m_entries.begin());

    if (!child.m_isLeaf) {
      child.m_children.push_back(std::move(sibling.m_children.front()));
      sibling.m_children.erase(sibling.m_children.begin());
    }

    refreshNodeMaxEnd(&child);
    refreshNodeMaxEnd(&sibling);
    refreshNodeMaxEnd(&parent);
  }

  void mergeChildren(Node &parent, std::size_t separatorIndex) {
    Node &left = *parent.m_children[separatorIndex];
    Node &right = *parent.m_children[separatorIndex + 1];
    left.m_entries.push_back(std::move(parent.m_entries[separatorIndex]));
    for (auto &entry : right.m_entries) {
      left.m_entries.push_back(std::move(entry));
    }

    if (!left.m_isLeaf) {
      for (auto &child : right.m_children) {
        left.m_children.push_back(std::move(child));
      }
    }

    parent.m_entries.erase(parent.m_entries.begin() +
                           static_cast<std::ptrdiff_t>(separatorIndex));
    parent.m_children.erase(parent.m_children.begin() +
                            static_cast<std::ptrdiff_t>(separatorIndex + 1));
    refreshNodeMaxEnd(&left);
    refreshNodeMaxEnd(&parent);
  }

  void eraseEntry(Node &node, const Entry *target) {
    std::size_t index = 0;
    while (index < node.m_entries.size() &&
           entryLess(*node.m_entries[index], *target)) {
      ++index;
    }

    if (index < node.m_entries.size() &&
        node.m_entries[index].get() == target) {
      if (node.m_isLeaf) {
        node.m_entries.erase(node.m_entries.begin() +
                             static_cast<std::ptrdiff_t>(index));
        refreshNodeMaxEnd(&node);
      } else if (node.m_children[index]->m_entries.size() >= kMinimumDegree) {
        node.m_entries[index] = removeMaximum(*node.m_children[index]);
        refreshNodeMaxEnd(&node);
      } else if (node.m_children[index + 1]->m_entries.size() >=
                 kMinimumDegree) {
        node.m_entries[index] = removeMinimum(*node.m_children[index + 1]);
        refreshNodeMaxEnd(&node);
      } else {
        mergeChildren(node, index);
        eraseEntry(*node.m_children[index], target);
        refreshNodeMaxEnd(&node);
      }

      return;
    }

    if (node.m_isLeaf) {
      return;
    }

    if (node.m_children[index]->m_entries.size() == kMinimumDegree - 1) {
      if (index > 0 &&
          node.m_children[index - 1]->m_entries.size() >= kMinimumDegree) {
        borrowFromPrevious(node, index);
      } else if (index < node.m_entries.size() &&
                 node.m_children[index + 1]->m_entries.size() >=
                     kMinimumDegree) {
        borrowFromNext(node, index);
      } else if (index < node.m_entries.size()) {
        mergeChildren(node, index);
      } else {
        mergeChildren(node, index - 1);
        --index;
      }
    }

    eraseEntry(*node.m_children[index], target);
    refreshNodeMaxEnd(&node);
  }

  auto eraseEntry(const Entry *target) -> bool {
    if (!m_root) {
      return false;
    }

    eraseEntry(*m_root, target);
    if (m_root->m_entries.empty()) {
      if (m_root->m_isLeaf) {
        m_root.reset();
      } else {
        m_root = std::move(m_root->m_children.front());
      }
    }

    --m_size;

    return true;
  }

  void ensureMutationAllowed() const {
    if (m_dispatchingCallbacks) {
      throw std::logic_error("reentrant interval B-tree mutation");
    }
  }

  void splitChild(Node &parent, std::size_t childIndex) {
    Node &child = *parent.m_children[childIndex];
    auto sibling = std::make_unique<Node>();
    sibling->m_isLeaf = child.m_isLeaf;

    constexpr auto maxKeys = 2 * kMinimumDegree - 1;
    sibling->m_entries.reserve(kMinimumDegree - 1);

    if (!child.m_isLeaf) {
      sibling->m_children.reserve(kMinimumDegree);
    }

    parent.m_entries.reserve(parent.m_entries.size() + 1);
    parent.m_children.reserve(parent.m_children.size() + 1);

    for (std::size_t index = kMinimumDegree; index < maxKeys; ++index) {
      sibling->m_entries.push_back(std::move(child.m_entries[index]));
    }

    auto promoted = std::move(child.m_entries[kMinimumDegree - 1]);
    child.m_entries.resize(kMinimumDegree - 1);

    if (!child.m_isLeaf) {
      for (std::size_t index = kMinimumDegree; index < child.m_children.size();
           ++index) {
        sibling->m_children.push_back(std::move(child.m_children[index]));
      }

      child.m_children.resize(kMinimumDegree);
    }

    parent.m_entries.insert(parent.m_entries.begin() +
                                static_cast<std::ptrdiff_t>(childIndex),
                            std::move(promoted));
    parent.m_children.insert(parent.m_children.begin() +
                                 static_cast<std::ptrdiff_t>(childIndex + 1),
                             std::move(sibling));
    parent.m_isLeaf = false;
    refreshNodeMaxEnd(&child);
    refreshNodeMaxEnd(parent.m_children[childIndex + 1].get());
    refreshNodeMaxEnd(&parent);
  }

  void insertNonFull(Node &node, std::unique_ptr<Entry> entry) {
    auto childIndex = node.m_entries.size();
    while (childIndex > 0 &&
           entryLess(*entry, *node.m_entries[childIndex - 1])) {
      --childIndex;
    }

    if (node.m_isLeaf) {
      node.m_entries.reserve(node.m_entries.size() + 1);
      node.m_entries.insert(node.m_entries.begin() +
                                static_cast<std::ptrdiff_t>(childIndex),
                            std::move(entry));
      refreshNodeMaxEnd(&node);

      return;
    }

    if (node.m_children[childIndex]->m_entries.size() ==
        2 * kMinimumDegree - 1) {
      splitChild(node, childIndex);

      if (entryLess(*node.m_entries[childIndex], *entry)) {
        ++childIndex;
      }
    }

    insertNonFull(*node.m_children[childIndex], std::move(entry));
    refreshNodeMaxEnd(&node);
  }

  auto hasOverlapInSubtree(const Node *node,
                           range_type range) const noexcept -> bool {
    if (node->m_subtreeMaxEnd < range.m_start) {
      return false;
    }

    for (std::size_t index = 0; index < node->m_entries.size(); ++index) {
      if (!node->m_isLeaf &&
          hasOverlapInSubtree(node->m_children[index].get(), range)) {
        return true;
      }

      if (node->m_entries[index]->m_range.overlaps(range)) {
        return true;
      }
    }

    return !node->m_isLeaf &&
           hasOverlapInSubtree(node->m_children.back().get(), range);
  }

  void visitOverlappingInOrder(const Node *node, range_type range,
                               const overlap_visitor &visitor) const {
    if (node->m_subtreeMaxEnd < range.m_start) {
      return;
    }

    for (std::size_t index = 0; index < node->m_entries.size(); ++index) {
      if (!node->m_isLeaf) {
        visitOverlappingInOrder(node->m_children[index].get(), range, visitor);
      }

      const auto &entry = *node->m_entries[index];
      if (entry.m_range.overlaps(range)) {
        visitor(entry.m_range, entry.m_value);
      }
    }

    if (!node->m_isLeaf) {
      visitOverlappingInOrder(node->m_children.back().get(), range, visitor);
    }
  }

  friend struct detail::Dmn_IntervalBTreeTestAccess<T>;

  canonical_comparator m_canonicalComparator;
  priority_evaluator m_priorityEvaluator;
  mutable bool m_dispatchingCallbacks{};
  bool m_suppressCallbacks{};
  std::unique_ptr<Node> m_root;
  std::size_t m_size{};
  std::size_t m_nextOrdinal{};
  std::vector<CallbackRegistration> m_callbackRegistrations;
  callback_registration_id m_nextCallbackRegistrationId{1};
};

template <class T> Dmn_IntervalBTree<T>::~Dmn_IntervalBTree() noexcept {}

template <class T>
bool Dmn_IntervalBTree<T>::add(range_type range, const value_type &value,
                               state_change_callback onStateChange) {
  ensureMutationAllowed();
  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  auto entry = std::make_unique<Entry>(
      Entry{range, value, m_nextOrdinal, std::move(onStateChange), 0, {}});

  return insertEntry(std::move(entry));
}

template <class T>
bool Dmn_IntervalBTree<T>::add(range_type range, value_type &&value,
                               state_change_callback onStateChange) {
  ensureMutationAllowed();
  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  auto entry = std::make_unique<Entry>(Entry{
      range, std::move(value), m_nextOrdinal, std::move(onStateChange), 0, {}});

  return insertEntry(std::move(entry));
}

template <class T>
auto Dmn_IntervalBTree<T>::addWithTopology(range_type range,
                                           const value_type &value,
                                           state_change_callback onStateChange)
    -> std::pair<bool, Dmn_TopologyResult<value_type>> {
  ensureMutationAllowed();
  Dmn_TopologyResult<value_type> result;
  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  result = queryTopology(range, value);
  auto entry = std::make_unique<Entry>(
      Entry{range, value, m_nextOrdinal, std::move(onStateChange), 0, {}});

  if (!insertEntry(std::move(entry))) {
    return {false, {}};
  }

  return {true, std::move(result)};
}

template <class T>
bool Dmn_IntervalBTree<T>::remove(range_type range, entry_matcher matcher) {
  return removeByRange(range, std::move(matcher));
}

template <class T>
bool Dmn_IntervalBTree<T>::removeByRange(range_type range,
                                         entry_matcher matcher) {
  ensureMutationAllowed();
  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  auto *target = findExactEntry(range, matcher);
  if (!target) {
    return false;
  }

  std::vector<Entry *> entries;
  entries.reserve(m_size);
  collectEntriesInOrder(m_root.get(), entries);
  std::vector<std::pair<Entry *, Dmn_OverlayState>> oldStates;
  oldStates.reserve(entries.size());

  for (Entry *entry : entries) {
    if (entry != target && entry->m_range.overlaps(target->m_range)) {
      oldStates.emplace_back(entry, entry->m_state);
    }
  }

  eraseEntry(target);
  updateStatesAndNotify(oldStates);

  return true;
}

template <class T>
std::size_t Dmn_IntervalBTree<T>::removeAllOverlapping(range_type range) {
  ensureMutationAllowed();

  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  if (!m_root) {
    return 0;
  }

  std::vector<Entry *> entries;
  entries.reserve(m_size);
  collectEntriesInOrder(m_root.get(), entries);
  std::vector<Entry *> targets;

  for (Entry *entry : entries) {
    if (entry->m_range.overlaps(range)) {
      targets.push_back(entry);
    }
  }

  if (targets.empty()) {
    return 0;
  }

  std::vector<std::pair<Entry *, Dmn_OverlayState>> oldStates;
  for (Entry *entry : entries) {
    if (containsEntry(targets, entry)) {
      continue;
    }

    const bool affected = std::any_of(
        targets.begin(), targets.end(), [entry](const Entry *target) {
          return entry->m_range.overlaps(target->m_range);
        });

    if (affected) {
      oldStates.emplace_back(entry, entry->m_state);
    }
  }

  for (Entry *target : targets) {
    eraseEntry(target);
  }

  updateStatesAndNotify(oldStates);

  return targets.size();
}

template <class T> void Dmn_IntervalBTree<T>::clear() {
  ensureMutationAllowed();

  m_root.reset();
  m_size = 0;
  m_nextOrdinal = 0;
}

template <class T> bool Dmn_IntervalBTree<T>::empty() const noexcept {
  return m_size == 0;
}

template <class T> std::size_t Dmn_IntervalBTree<T>::size() const noexcept {
  return m_size;
}

template <class T>
std::vector<std::pair<typename Dmn_IntervalBTree<T>::range_type,
                      typename Dmn_IntervalBTree<T>::value_type>>
Dmn_IntervalBTree<T>::enumerateCanonical(
    duplicate_order_evaluator duplicateOrder) const {
  std::vector<const Entry *> orderedEntries;
  orderedEntries.reserve(m_size);

  if (m_root) {
    collectEntriesInOrder(m_root.get(), orderedEntries);
  }

  {
    MutationBlockGuard guard(m_dispatchingCallbacks);
    std::stable_sort(
        orderedEntries.begin(), orderedEntries.end(),
        [this, &duplicateOrder](const Entry *lhs, const Entry *rhs) {
          if (lhs->m_range.m_start == rhs->m_range.m_start &&
              lhs->m_range.m_end == rhs->m_range.m_end) {
            if (duplicateOrder) {
              return duplicateOrder(lhs->m_value, rhs->m_value);
            }

            return lhs->m_ordinal < rhs->m_ordinal;
          }

          return entryLess(*lhs, *rhs);
        });
  }

  std::vector<std::pair<range_type, value_type>> result;
  result.reserve(orderedEntries.size());

  for (const Entry *entry : orderedEntries) {
    result.emplace_back(entry->m_range, entry->m_value);
  }

  return result;
}

template <class T>
std::vector<std::pair<typename Dmn_IntervalBTree<T>::range_type,
                      typename Dmn_IntervalBTree<T>::value_type>>
Dmn_IntervalBTree<T>::enumerateCanonicalMove(
    duplicate_order_evaluator duplicateOrder) {
  ensureMutationAllowed();

  std::vector<Entry *> orderedEntries;
  orderedEntries.reserve(m_size);

  if (m_root) {
    collectEntriesInOrder(m_root.get(), orderedEntries);
  }

  {
    MutationBlockGuard guard(m_dispatchingCallbacks);
    std::stable_sort(
        orderedEntries.begin(), orderedEntries.end(),
        [this, &duplicateOrder](const Entry *lhs, const Entry *rhs) {
          if (lhs->m_range.m_start == rhs->m_range.m_start &&
              lhs->m_range.m_end == rhs->m_range.m_end) {
            if (duplicateOrder) {
              return duplicateOrder(lhs->m_value, rhs->m_value);
            }

            return lhs->m_ordinal < rhs->m_ordinal;
          }

          return entryLess(*lhs, *rhs);
        });
  }

  std::vector<std::pair<range_type, value_type>> result;
  result.reserve(orderedEntries.size());

  for (Entry *entry : orderedEntries) {
    result.emplace_back(entry->m_range, std::move(entry->m_value));
  }

  m_root.reset();
  m_size = 0;
  m_nextOrdinal = 0;

  return result;
}

template <class T>
void Dmn_IntervalBTree<T>::reconstructFromCanonical(
    const std::vector<std::pair<range_type, value_type>> &entries,
    duplicate_order_evaluator duplicateOrder) {
  ensureMutationAllowed();
  (void)duplicateOrder;

  for (const auto &entry : entries) {
    if (!entry.first.isValid()) {
      throw std::invalid_argument("canonical entries contain an invalid range");
    }
  }

  Dmn_IntervalBTree rebuilt(m_canonicalComparator, m_priorityEvaluator);
  rebuilt.m_suppressCallbacks = true;

  for (const auto &entry : entries) {
    callback_registration_id registrationId{};
    auto callback = callbackForReconstruction(entry.second, registrationId);
    auto rebuiltEntry = std::make_unique<Entry>(Entry{entry.first,
                                                      entry.second,
                                                      rebuilt.m_nextOrdinal,
                                                      std::move(callback),
                                                      registrationId,
                                                      {}});
    rebuilt.insertEntry(std::move(rebuiltEntry));
  }

  rebuilt.m_suppressCallbacks = false;

  m_root.swap(rebuilt.m_root);
  m_size = rebuilt.m_size;
  m_nextOrdinal = rebuilt.m_nextOrdinal;
}

template <class T>
auto Dmn_IntervalBTree<T>::registerStateCallback(
    std::function<bool(const value_type &)> matches, callback_context context,
    registered_state_callback callback) -> callback_registration_id {
  ensureMutationAllowed();

  if (!matches || !callback) {
    throw std::invalid_argument(
        "state callback registration requires callable matcher and callback");
  }

  if (m_nextCallbackRegistrationId ==
      std::numeric_limits<callback_registration_id>::max()) {
    throw std::overflow_error("state callback registration id exhausted");
  }

  const auto id = m_nextCallbackRegistrationId++;
  m_callbackRegistrations.push_back(
      {id, std::move(matches), std::move(context), std::move(callback)});

  return id;
}

template <class T>
void Dmn_IntervalBTree<T>::unregisterStateCallback(
    callback_registration_id registration) {
  ensureMutationAllowed();

  const auto registrationIt = std::find_if(
      m_callbackRegistrations.begin(), m_callbackRegistrations.end(),
      [registration](const CallbackRegistration &candidate) {
        return candidate.m_id == registration;
      });

  if (registrationIt == m_callbackRegistrations.end()) {
    return;
  }

  std::vector<Entry *> entries;
  entries.reserve(m_size);
  if (m_root) {
    collectEntriesInOrder(m_root.get(), entries);
  }

  m_callbackRegistrations.erase(registrationIt);
  for (Entry *entry : entries) {
    if (entry->m_callbackRegistrationId == registration) {
      entry->m_onStateChange = {};
      entry->m_callbackRegistrationId = 0;
    }
  }
}

template <class T>
bool Dmn_IntervalBTree<T>::hasOverlap(range_type range) const noexcept {
  return range.isValid() && m_root && hasOverlapInSubtree(m_root.get(), range);
}

template <class T>
std::vector<std::pair<typename Dmn_IntervalBTree<T>::range_type,
                      typename Dmn_IntervalBTree<T>::value_type>>
Dmn_IntervalBTree<T>::findOverlapping(range_type range) const {
  std::vector<std::pair<range_type, value_type>> result;
  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  forEachOverlapping(range, [&result](const range_type &overlapRange,
                                      const value_type &value) {
    result.emplace_back(overlapRange, value);
  });

  return result;
}

template <class T>
void Dmn_IntervalBTree<T>::forEachOverlapping(range_type range,
                                              overlap_visitor visitor) const {
  if (!visitor) {
    throw std::invalid_argument("overlap visitor must be callable");
  }

  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  if (!m_root) {
    return;
  }

  MutationBlockGuard guard(m_dispatchingCallbacks);
  visitOverlappingInOrder(m_root.get(), range, visitor);
}

template <class T>
Dmn_TopologyResult<typename Dmn_IntervalBTree<T>::value_type>
Dmn_IntervalBTree<T>::queryTopology(range_type range,
                                    const value_type &value) const {
  if (!range.isValid()) {
    throw std::invalid_argument("range is invalid");
  }

  Dmn_TopologyResult<value_type> result;
  result.m_overlappingEntries = findOverlapping(range);
  std::vector<range_type> overlapRanges;
  overlapRanges.reserve(result.m_overlappingEntries.size());

  for (const auto &entry : result.m_overlappingEntries) {
    overlapRanges.push_back(entry.first);
  }

  result.m_status = classifyTopology(range, overlapRanges);
  if (m_priorityEvaluator) {
    MutationBlockGuard guard(m_dispatchingCallbacks);

    for (const auto &entry : result.m_overlappingEntries) {
      if (m_priorityEvaluator(entry.second, value)) {
        result.m_isTop = false;

        break;
      }
    }
  }

  return result;
}

} // namespace dmn

#endif /* DMN_INTERVAL_BTREE_HPP_ */
