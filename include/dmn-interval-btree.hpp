/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-interval-btree.hpp
 * @brief Provides a template-based B-tree for indexing inclusive intervals
 *        and querying overlapping entries.
 */

#ifndef DMN_INTERVAL_BTREE_HPP_
#define DMN_INTERVAL_BTREE_HPP_

#include <cstddef>
#include <cstdint>
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

  Dmn_IntervalBTree() noexcept = default;

  // Rule of 5: delete copy/move
  Dmn_IntervalBTree(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree &operator=(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree(Dmn_IntervalBTree &&) = delete;
  Dmn_IntervalBTree &operator=(Dmn_IntervalBTree &&) = delete;

  ~Dmn_IntervalBTree() noexcept;

  bool add(range_type range, const value_type &value);
  bool empty() const noexcept;
  std::size_t size() const noexcept;

private:
  struct Entry {
    range_type range;
    value_type value;
  };

  std::vector<Entry> entries_;
};

template <class T> Dmn_IntervalBTree<T>::~Dmn_IntervalBTree() noexcept {}

template <class T>
bool Dmn_IntervalBTree<T>::add(range_type range, const value_type &value) {
  if (!range.isValid()) {
    return false;
  }

  entries_.push_back(Entry{range, value});
  return true;
}

template <class T> bool Dmn_IntervalBTree<T>::empty() const noexcept {
  return entries_.empty();
}

template <class T> std::size_t Dmn_IntervalBTree<T>::size() const noexcept {
  return entries_.size();
}

} // namespace dmn

#endif /* DMN_INTERVAL_BTREE_HPP_ */
