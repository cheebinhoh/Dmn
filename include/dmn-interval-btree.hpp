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
  std::int64_t start{};
  std::int64_t end{};

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
      : canonicalComparator_(std::move(comparator)) {}

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
    if (lhs.start != rhs.start) {
      return lhs.start < rhs.start;
    }
    return lhs.end < rhs.end;
  }

  struct Entry {
    range_type range;
    value_type value;
    std::size_t ordinal;
  };

  canonical_comparator canonicalComparator_;
  std::vector<Entry> entries_;
  std::size_t nextOrdinal_{};
};

template <class T> Dmn_IntervalBTree<T>::~Dmn_IntervalBTree() noexcept {}

template <class T>
bool Dmn_IntervalBTree<T>::add(range_type range, const value_type &value) {
  if (!range.isValid()) {
    return false;
  }

  entries_.push_back(Entry{range, value, nextOrdinal_});
  nextOrdinal_++;

  return true;
}

template <class T> bool Dmn_IntervalBTree<T>::empty() const noexcept {
  return entries_.empty();
}

template <class T> std::size_t Dmn_IntervalBTree<T>::size() const noexcept {
  return entries_.size();
}

template <class T>
std::vector<std::pair<typename Dmn_IntervalBTree<T>::range_type,
                      typename Dmn_IntervalBTree<T>::value_type>>
Dmn_IntervalBTree<T>::enumerateCanonical(
    duplicate_order_evaluator duplicateOrder) const {
  std::vector<const Entry *> orderedEntries;
  orderedEntries.reserve(entries_.size());

  for (const auto &entry : entries_) {
    orderedEntries.push_back(&entry);
  }

  std::stable_sort(orderedEntries.begin(), orderedEntries.end(),
                   [this, &duplicateOrder](const Entry *lhs, const Entry *rhs) {
                     const bool identicalRanges =
                         lhs->range.start == rhs->range.start &&
                         lhs->range.end == rhs->range.end;

                     if (identicalRanges) {
                       if (duplicateOrder) {
                         return duplicateOrder(lhs->value, rhs->value);
                       }
                       return lhs->ordinal < rhs->ordinal;
                     }

                     if (canonicalComparator_) {
                       if (canonicalComparator_(lhs->range, rhs->range)) {
                         return true;
                       }
                       if (canonicalComparator_(rhs->range, lhs->range)) {
                         return false;
                       }
                     }

                     return defaultRangeLess(lhs->range, rhs->range);
                   });

  std::vector<std::pair<range_type, value_type>> result;
  result.reserve(orderedEntries.size());

  for (const Entry *entry : orderedEntries) {
    result.emplace_back(entry->range, entry->value);
  }

  return result;
}

} // namespace dmn

#endif /* DMN_INTERVAL_BTREE_HPP_ */
