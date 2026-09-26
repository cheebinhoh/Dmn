/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-interval-btree.cpp
 * @brief Unit test for Dmn_Interval_Btree.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "dmn-interval-btree.hpp"

TEST(IntervalRange, RejectsReversed) {
  using dmn::Dmn_IntervalRange;

  EXPECT_FALSE((Dmn_IntervalRange{2, 1}).isValid());
  EXPECT_FALSE((Dmn_IntervalRange{0, -1}).isValid());

  // A valid range with negative endpoints is not invalid merely because
  // its values are negative.
  EXPECT_TRUE((Dmn_IntervalRange{-5, -2}).isValid());
}

TEST(IntervalRange, AcceptsSinglePoint) {
  using dmn::Dmn_IntervalRange;

  EXPECT_TRUE((Dmn_IntervalRange{4, 4}).isValid());
  EXPECT_TRUE((Dmn_IntervalRange{4, 4}).overlaps({4, 4}));
}

TEST(IntervalRange, AcceptsNegativeValues) {
  using dmn::Dmn_IntervalRange;

  EXPECT_TRUE((Dmn_IntervalRange{-5, -2}).isValid());
  EXPECT_TRUE((Dmn_IntervalRange{-5, -2}).overlaps({-3, 0}));
  EXPECT_FALSE((Dmn_IntervalRange{-5, -2}).overlaps({-1, 0}));
}

TEST(IntervalRange, InclusiveSharedEndpointConflicts) {
  using dmn::Dmn_IntervalRange;

  // Both ranges include the point 2, so they overlap.
  EXPECT_TRUE((Dmn_IntervalRange{1, 2}).overlaps({2, 3}));
}

TEST(IntervalRange, AdjacentRangesDoNotConflict) {
  using dmn::Dmn_IntervalRange;

  // There is no shared endpoint: the first range ends at 2, the next starts
  // at 3.
  EXPECT_FALSE((Dmn_IntervalRange{1, 2}).overlaps({3, 4}));
}

TEST(IntervalRange, HandlesInt64BoundariesWithoutOverflow) {
  using dmn::Dmn_IntervalRange;

  constexpr auto min = std::numeric_limits<std::int64_t>::min();
  constexpr auto max = std::numeric_limits<std::int64_t>::max();

  EXPECT_TRUE((Dmn_IntervalRange{min, min}).isValid());
  EXPECT_TRUE((Dmn_IntervalRange{max, max}).isValid());

  // Shared endpoint at the maximum signed value still overlaps.
  EXPECT_TRUE((Dmn_IntervalRange{max - 1, max}).overlaps({max, max}));

  // The minimum-value point does not overlap a range starting one point later.
  EXPECT_FALSE((Dmn_IntervalRange{min, min}).overlaps({min + 1, max}));

  // A range spanning the full int64_t domain is valid and overlaps both ends.
  const Dmn_IntervalRange fullRange{min, max};
  EXPECT_TRUE(fullRange.isValid());
  EXPECT_TRUE(fullRange.overlaps({min, min}));
  EXPECT_TRUE(fullRange.overlaps({max, max}));
}

TEST(IntervalRange, OverlapRejectsInvalidOperands) {
  using dmn::Dmn_IntervalRange;

  const Dmn_IntervalRange invalid{2, 1};
  const Dmn_IntervalRange valid{1, 3};

  // The invalid range must not report overlap in either operand position.
  EXPECT_FALSE(invalid.overlaps(valid));
  EXPECT_FALSE(valid.overlaps(invalid));

  // Two invalid ranges must not report overlap either.
  EXPECT_FALSE(invalid.overlaps(Dmn_IntervalRange{4, 0}));
}

TEST(IntervalBTree, DefaultConstructs) {
  [[maybe_unused]] dmn::Dmn_IntervalBTree<int> tree;
  SUCCEED();
}

TEST(IntervalBTree, StoresOneEntry) {
  dmn::Dmn_IntervalBTree<int> tree;

  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(tree.size(), 0U);

  EXPECT_TRUE(tree.add({2, 5}, 42));

  EXPECT_FALSE(tree.empty());
  EXPECT_EQ(tree.size(), 1U);
}

TEST(IntervalBTree, RejectsInvalidInsertionWithoutMutation) {
  dmn::Dmn_IntervalBTree<int> tree;

  EXPECT_TRUE(tree.add({2, 5}, 42));
  EXPECT_FALSE(tree.add({5, 2}, 7));

  EXPECT_FALSE(tree.empty());
  EXPECT_EQ(tree.size(), 1U);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
