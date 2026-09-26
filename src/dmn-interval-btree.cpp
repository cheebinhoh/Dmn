/**
 * @file dmn-interval-btree.cpp
 * @brief Provides a template-based B-tree for indexing inclusive intervals
 *        and querying overlapping entries.
 */

#include "dmn-interval-btree.hpp"

#include <cstdint>

namespace dmn {

auto Dmn_IntervalRange::isValid() const noexcept -> bool {
  return start <= end;
}

auto Dmn_IntervalRange::overlaps(const Dmn_IntervalRange &other) const noexcept
    -> bool {
  return isValid() && other.isValid() && start <= other.end &&
         other.start <= end;
}

} // namespace dmn
