/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-interval-btree.hpp
 * @brief Provides a template-based B-tree for indexing inclusive intervals
 *        and querying overlapping entries.
 */

#ifndef DMN_INTERVAL_BTREE_HPP_
#define DMN_INTERVAL_BTREE_HPP_

#include <cstdint>

namespace dmn {

struct Dmn_IntervalRange {
  std::int64_t start{};
  std::int64_t end{};

  auto isValid() const noexcept -> bool;
  auto overlaps(const Dmn_IntervalRange &other) const noexcept -> bool;
}; // struct Dmn_IntervalRange

template <class T> class Dmn_IntervalBTree {
public:
  Dmn_IntervalBTree() noexcept = default;

  // Rule of 5: delete copy/move
  Dmn_IntervalBTree(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree &operator=(const Dmn_IntervalBTree &) = delete;
  Dmn_IntervalBTree(Dmn_IntervalBTree &&) = delete;
  Dmn_IntervalBTree &operator=(Dmn_IntervalBTree &&) = delete;

  ~Dmn_IntervalBTree() noexcept;
};

template <class T> Dmn_IntervalBTree<T>::~Dmn_IntervalBTree() noexcept {}

} // namespace dmn

#endif /* DMN_INTERVAL_BTREE_HPP_ */
