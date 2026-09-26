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
#include <utility>
#include <vector>

namespace dmn {

struct Dmn_IntervalRange {
  std::int64_t m_start{};
  std::int64_t m_end{};

  auto isValid() const noexcept -> bool;
  auto overlaps(const Dmn_IntervalRange &other) const noexcept -> bool;
}; // struct Dmn_IntervalRange

template <class T> class Dmn_IntervalBTree {
public:
  using value_type = T;
  using range_type = Dmn_IntervalRange;
  using duplicate_order_evaluator =
      std::function<bool(const value_type &, const value_type &)>;
  using canonical_comparator =
      std::function<bool(const range_type &, const range_type &)>;

  Dmn_IntervalBTree() noexcept = default;
  explicit Dmn_IntervalBTree(canonical_comparator comparator)
      : m_canonicalComparator(std::move(comparator)) {}

  // Rule of 5: delete copy/move
  Dmn_IntervalBTree(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree &operator=(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree(Dmn_IntervalBTree &&) = delete;
  Dmn_IntervalBTree &operator=(Dmn_IntervalBTree &&) = delete;

  ~Dmn_IntervalBTree() noexcept;

  bool add(range_type range, const value_type &value);
  bool empty() const noexcept;
  std::size_t size() const noexcept;

  std::vector<std::pair<range_type, value_type>>
  enumerateCanonical(duplicate_order_evaluator duplicateOrder = {}) const;

private:
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
  };

  canonical_comparator m_canonicalComparator;
  std::vector<Entry> m_entries;
  std::size_t m_nextOrdinal{};
};

template <class T> Dmn_IntervalBTree<T>::~Dmn_IntervalBTree() noexcept {}

template <class T>
bool Dmn_IntervalBTree<T>::add(range_type range, const value_type &value) {
  if (!range.isValid()) {
    return false;
  }

  m_entries.push_back(Entry{range, value, m_nextOrdinal});
  m_nextOrdinal++;

  return true;
}

template <class T> bool Dmn_IntervalBTree<T>::empty() const noexcept {
  return m_entries.empty();
}

template <class T> std::size_t Dmn_IntervalBTree<T>::size() const noexcept {
  return m_entries.size();
}

template <class T>
std::vector<std::pair<typename Dmn_IntervalBTree<T>::range_type,
                      typename Dmn_IntervalBTree<T>::value_type>>
Dmn_IntervalBTree<T>::enumerateCanonical(
    duplicate_order_evaluator duplicateOrder) const {
  std::vector<const Entry *> orderedEntries;
  orderedEntries.reserve(m_entries.size());

  for (const auto &entry : m_entries) {
    orderedEntries.push_back(&entry);
  }

  std::stable_sort(orderedEntries.begin(), orderedEntries.end(),
                   [this, &duplicateOrder](const Entry *lhs, const Entry *rhs) {
                     const bool identicalRanges =
                         lhs->m_range.m_start == rhs->m_range.m_start &&
                         lhs->m_range.m_end == rhs->m_range.m_end;

                     if (identicalRanges) {
                       if (duplicateOrder) {
                         return duplicateOrder(lhs->m_value, rhs->m_value);
                       }
                       return lhs->m_ordinal < rhs->m_ordinal;
                     }

                     if (m_canonicalComparator) {
                       if (m_canonicalComparator(lhs->m_range, rhs->m_range)) {
                         return true;
                       }
                       if (m_canonicalComparator(rhs->m_range, lhs->m_range)) {
                         return false;
                       }
                     }

                     return defaultRangeLess(lhs->m_range, rhs->m_range);
                   });

  std::vector<std::pair<range_type, value_type>> result;
  result.reserve(orderedEntries.size());

  for (const Entry *entry : orderedEntries) {
    result.emplace_back(entry->m_range, entry->m_value);
  }

  return result;
}

} // namespace dmn

#endif /* DMN_INTERVAL_BTREE_HPP_ */
