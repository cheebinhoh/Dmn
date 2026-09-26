/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-interval-btree.hpp
 * @brief Provides a template-based B-tree for indexing inclusive intervals
 *        and querying overlapping entries.
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

struct Dmn_IntervalRange {
  std::int64_t m_start{};
  std::int64_t m_end{};

  auto isValid() const noexcept -> bool;
  auto overlaps(const Dmn_IntervalRange &other) const noexcept -> bool;
}; // struct Dmn_IntervalRange

enum class Dmn_OverlayTopology {
  Clear,
  OverlaidLeft,
  OverlaidRight,
  OverlaidBoth,
  FullyCovered,
  CoveringExisting
};

template <class T> struct Dmn_TopologyResult {
  Dmn_OverlayTopology m_status{Dmn_OverlayTopology::Clear};
  bool m_isTop{true};
  std::vector<std::pair<Dmn_IntervalRange, T>> m_overlappingEntries;
};

struct Dmn_OverlayState {
  Dmn_OverlayTopology m_topology{Dmn_OverlayTopology::Clear};
  bool m_isTop{true};
};

template <class T> using entry_matcher = std::function<bool(const T &)>;

template <class T>
using duplicate_order_evaluator = std::function<bool(const T &, const T &)>;

template <class T>
using state_change_callback =
    std::function<void(const Dmn_IntervalRange &, const T &,
                       const Dmn_OverlayState &, const Dmn_OverlayState &)>;

template <class T> class Dmn_IntervalBTree {
public:
  using value_type = T;
  using range_type = Dmn_IntervalRange;
  using duplicate_order_evaluator = dmn::duplicate_order_evaluator<value_type>;
  using canonical_comparator =
      std::function<bool(const range_type &, const range_type &)>;
  using priority_evaluator =
      std::function<bool(const value_type &, const value_type &)>;
  using state_change_callback = dmn::state_change_callback<value_type>;
  using entry_matcher = dmn::entry_matcher<value_type>;
  using overlap_visitor =
      std::function<void(const range_type &, const value_type &)>;
  using callback_registration_id = std::uint64_t;
  using callback_context = std::shared_ptr<void>;
  using registered_state_callback =
      std::function<void(const callback_context &, const value_type &,
                         const Dmn_OverlayState &, const Dmn_OverlayState &)>;

  Dmn_IntervalBTree() noexcept = default;
  explicit Dmn_IntervalBTree(canonical_comparator comparator)
      : m_canonicalComparator(std::move(comparator)) {}
  Dmn_IntervalBTree(canonical_comparator comparator,
                    priority_evaluator priorityEvaluator)
      : m_canonicalComparator(std::move(comparator)),
        m_priorityEvaluator(std::move(priorityEvaluator)) {}

  // Rule of 5: delete copy/move
  Dmn_IntervalBTree(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree &operator=(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree(Dmn_IntervalBTree &&) = delete;
  Dmn_IntervalBTree &operator=(Dmn_IntervalBTree &&) = delete;

  ~Dmn_IntervalBTree() noexcept;

  bool add(range_type range, const value_type &value,
           state_change_callback onStateChange = {});
  bool add(range_type range, value_type &&value,
           state_change_callback onStateChange = {});
  bool add(std::int64_t start, std::int64_t end, const value_type &value,
           state_change_callback onStateChange = {}) {
    return add(range_type{start, end}, value, std::move(onStateChange));
  }
  bool add(std::int64_t start, std::int64_t end, value_type &&value,
           state_change_callback onStateChange = {}) {
    return add(range_type{start, end}, std::move(value),
               std::move(onStateChange));
  }
  std::pair<bool, Dmn_TopologyResult<value_type>>
  addWithTopology(range_type range, const value_type &value,
                  state_change_callback onStateChange = {});
  bool remove(range_type range, entry_matcher matcher = {});
  bool removeByRange(range_type range, entry_matcher matcher = {});
  std::size_t removeAllOverlapping(range_type range);
  void clear();
  bool empty() const noexcept;
  std::size_t size() const noexcept;

  std::vector<std::pair<range_type, value_type>>
  enumerateCanonical(duplicate_order_evaluator duplicateOrder = {}) const;
  std::vector<std::pair<range_type, value_type>>
  enumerateCanonicalMove(duplicate_order_evaluator duplicateOrder = {});
  void reconstructFromCanonical(
      const std::vector<std::pair<range_type, value_type>> &entries,
      duplicate_order_evaluator duplicateOrder = {});
  callback_registration_id
  registerStateCallback(std::function<bool(const value_type &)> matches,
                        callback_context context,
                        registered_state_callback callback);
  void unregisterStateCallback(callback_registration_id registration);
  bool hasOverlap(range_type range) const noexcept;
  std::vector<std::pair<range_type, value_type>>
  findOverlapping(range_type range) const;
  void forEachOverlapping(range_type range, overlap_visitor visitor) const;
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
      return false;
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
    return false;
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
    return false;
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
    return {false, std::move(result)};
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
    return false;
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
  if (!range.isValid() || !m_root) {
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
    return result;
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
  if (!range.isValid() || !m_root) {
    return;
  }

  MutationBlockGuard guard(m_dispatchingCallbacks);
  visitOverlappingInOrder(m_root.get(), range, visitor);
}

template <class T>
Dmn_TopologyResult<typename Dmn_IntervalBTree<T>::value_type>
Dmn_IntervalBTree<T>::queryTopology(range_type range,
                                    const value_type &value) const {
  Dmn_TopologyResult<value_type> result;
  if (!range.isValid()) {
    return result;
  }

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
