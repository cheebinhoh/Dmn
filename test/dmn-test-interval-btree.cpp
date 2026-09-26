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

TEST(IntervalBTree, CanonicalOrderSimple) {
  dmn::Dmn_IntervalBTree<int> tree;

  ASSERT_TRUE(tree.add({5, 7}, 50));
  ASSERT_TRUE(tree.add({1, 3}, 10));
  ASSERT_TRUE(tree.add({1, 2}, 11));

  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].first.m_start, 1);
  EXPECT_EQ(entries[0].first.m_end, 2);
  EXPECT_EQ(entries[0].second, 11);
  EXPECT_EQ(entries[1].first.m_start, 1);
  EXPECT_EQ(entries[1].first.m_end, 3);
  EXPECT_EQ(entries[1].second, 10);
  EXPECT_EQ(entries[2].first.m_start, 5);
  EXPECT_EQ(entries[2].first.m_end, 7);
  EXPECT_EQ(entries[2].second, 50);
}

TEST(IntervalBTree, DuplicateRangesUseStableTieBreak) {
  dmn::Dmn_IntervalBTree<int> tree;

  ASSERT_TRUE(tree.add({2, 5}, 10));
  EXPECT_FALSE(tree.add({5, 2}, 99));
  ASSERT_TRUE(tree.add({2, 5}, 20));

  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].second, 10);
  EXPECT_EQ(entries[1].second, 20);
}

TEST(IntervalBTree, DuplicateRangesUseCallerOrdering) {
  dmn::Dmn_IntervalBTree<int> tree;

  ASSERT_TRUE(tree.add({2, 5}, 30));
  ASSERT_TRUE(tree.add({2, 5}, 10));
  ASSERT_TRUE(tree.add({2, 5}, 20));

  const auto entries = tree.enumerateCanonical(
      [](const int &lhs, const int &rhs) { return lhs < rhs; });

  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].second, 10);
  EXPECT_EQ(entries[1].second, 20);
  EXPECT_EQ(entries[2].second, 30);
}

TEST(IntervalBTree, DuplicateOrderingCallbackNotCalledForDistinctRanges) {
  dmn::Dmn_IntervalBTree<int> tree;

  ASSERT_TRUE(tree.add({3, 5}, 30));
  ASSERT_TRUE(tree.add({1, 4}, 10));
  ASSERT_TRUE(tree.add({2, 6}, 20));

  std::size_t callbackCalls = 0;
  const auto entries =
      tree.enumerateCanonical([&callbackCalls](const int &, const int &) {
        ++callbackCalls;
        return false;
      });

  EXPECT_EQ(callbackCalls, 0U);
  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].second, 10);
  EXPECT_EQ(entries[1].second, 20);
  EXPECT_EQ(entries[2].second, 30);
}

TEST(IntervalBTree, CustomComparatorIsUsed) {
  using Range = dmn::Dmn_IntervalBTree<int>::range_type;
  const auto reverseStart = [](const Range &lhs, const Range &rhs) {
    return lhs.m_start > rhs.m_start;
  };
  dmn::Dmn_IntervalBTree<int> tree(reverseStart);

  ASSERT_TRUE(tree.add({1, 3}, 10));
  ASSERT_TRUE(tree.add({5, 7}, 50));
  ASSERT_TRUE(tree.add({3, 4}, 30));

  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].second, 50);
  EXPECT_EQ(entries[1].second, 30);
  EXPECT_EQ(entries[2].second, 10);
}

TEST(IntervalBTree, CustomComparatorEquivalentRangesUseDefaultTieBreak) {
  using Range = dmn::Dmn_IntervalBTree<int>::range_type;
  const auto compareStartOnly = [](const Range &lhs, const Range &rhs) {
    return lhs.m_start < rhs.m_start;
  };
  dmn::Dmn_IntervalBTree<int> tree(compareStartOnly);

  ASSERT_TRUE(tree.add({2, 5}, 25));
  ASSERT_TRUE(tree.add({2, 3}, 23));
  ASSERT_TRUE(tree.add({1, 8}, 18));

  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].second, 18);
  EXPECT_EQ(entries[1].second, 23);
  EXPECT_EQ(entries[2].second, 25);
}

TEST(IntervalBTree, ConstructorsUseDefaultAndCustomComparators) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  using Range = Tree::range_type;

  Tree defaultTree;
  ASSERT_TRUE(defaultTree.add({5, 7}, 50));
  ASSERT_TRUE(defaultTree.add({1, 3}, 10));
  const auto defaultEntries = defaultTree.enumerateCanonical();
  ASSERT_EQ(defaultEntries.size(), 2U);
  EXPECT_EQ(defaultEntries[0].second, 10);
  EXPECT_EQ(defaultEntries[1].second, 50);

  Tree customTree([](const Range &lhs, const Range &rhs) {
    return lhs.m_start > rhs.m_start;
  });
  ASSERT_TRUE(customTree.add({5, 7}, 50));
  ASSERT_TRUE(customTree.add({1, 3}, 10));
  const auto customEntries = customTree.enumerateCanonical();
  ASSERT_EQ(customEntries.size(), 2U);
  EXPECT_EQ(customEntries[0].second, 50);
  EXPECT_EQ(customEntries[1].second, 10);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
