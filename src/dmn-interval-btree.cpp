/**
 * @file dmn-interval-btree.cpp
 * @brief Provides a template-based B-tree for indexing inclusive intervals
 *        and querying overlapping entries.
 */

#include "dmn-interval-btree.hpp"

#include <cstdint>

namespace dmn {

auto Dmn_IntervalRange::isValid() const noexcept -> bool {
  return m_start <= m_end;
}

auto Dmn_IntervalRange::overlaps(const Dmn_IntervalRange &other) const noexcept
    -> bool {
  return isValid() && other.isValid() && m_start <= other.m_end &&
         other.m_start <= m_end;
}

} // namespace dmn
