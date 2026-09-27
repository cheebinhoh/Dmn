/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-interval-btree.cpp
 * @brief Unit test for Dmn_Interval_Btree.
 */

#include <gtest/gtest.h>

#include "dmn-interval-btree.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace dmn::detail {

template <class T> struct Dmn_IntervalBTreeTestAccess {
  using Tree = Dmn_IntervalBTree<T>;

  static auto rootKeyCount(const Tree &tree) -> std::size_t {
    return tree.m_root ? tree.m_root->m_entries.size() : 0;
  }

  static auto rootChildCount(const Tree &tree) -> std::size_t {
    return tree.m_root ? tree.m_root->m_children.size() : 0;
  }

  static auto rootIsLeaf(const Tree &tree) -> bool {
    return !tree.m_root || tree.m_root->m_isLeaf;
  }

  static auto validate(const Tree &tree) -> bool {
    if (!tree.m_root) {
      return tree.m_size == 0;
    }

    std::size_t entryCount = 0;
    std::size_t leafDepth = 0;
    bool sawLeaf = false;
    std::vector<const typename Tree::Entry *> orderedEntries;

    if (!validateNode(tree.m_root.get(), true, 0, leafDepth, sawLeaf,
                      entryCount, orderedEntries) ||
        entryCount != tree.m_size ||
        !validateSubtreeMaxEnd(tree.m_root.get())) {
      return false;
    }

    for (std::size_t index = 1; index < orderedEntries.size(); ++index) {
      if (!tree.entryLess(*orderedEntries[index - 1], *orderedEntries[index])) {
        return false;
      }
    }

    return true;
  }

private:
  static auto validateSubtreeMaxEnd(const typename Tree::Node *node) -> bool {
    auto maximumEnd = std::numeric_limits<std::int64_t>::min();

    for (const auto &entry : node->m_entries) {
      maximumEnd = std::max(maximumEnd, entry->m_range.m_end);
    }

    for (const auto &child : node->m_children) {
      if (!validateSubtreeMaxEnd(child.get())) {
        return false;
      }

      maximumEnd = std::max(maximumEnd, child->m_subtreeMaxEnd);
    }

    return node->m_subtreeMaxEnd == maximumEnd;
  }

  static auto validateNode(
      const typename Tree::Node *node, bool isRoot, std::size_t depth,
      std::size_t &leafDepth, bool &sawLeaf, std::size_t &entryCount,
      std::vector<const typename Tree::Entry *> &orderedEntries) -> bool {
    constexpr std::size_t minimumDegree = 2;
    constexpr std::size_t maximumKeys = 2 * minimumDegree - 1;

    if (!node || node->m_entries.size() > maximumKeys ||
        (!isRoot && node->m_entries.size() < minimumDegree - 1)) {
      return false;
    }

    if (node->m_isLeaf) {
      if (!node->m_children.empty()) {
        return false;
      }

      if (sawLeaf && leafDepth != depth) {
        return false;
      }

      leafDepth = depth;
      sawLeaf = true;

      for (const auto &entry : node->m_entries) {
        orderedEntries.push_back(entry.get());
        ++entryCount;
      }

      return true;
    }

    if (node->m_entries.empty() ||
        node->m_children.size() != node->m_entries.size() + 1) {
      return false;
    }

    for (std::size_t index = 0; index < node->m_entries.size(); ++index) {
      if (!validateNode(node->m_children[index].get(), false, depth + 1,
                        leafDepth, sawLeaf, entryCount, orderedEntries)) {
        return false;
      }

      orderedEntries.push_back(node->m_entries[index].get());
      ++entryCount;
    }

    return validateNode(node->m_children.back().get(), false, depth + 1,
                        leafDepth, sawLeaf, entryCount, orderedEntries);
  }
};

} // namespace dmn::detail

struct IntervalBTreeCopyTracked {
  static inline std::size_t copies = 0;
  int m_value{};

  explicit IntervalBTreeCopyTracked(int value) : m_value(value) {}

  IntervalBTreeCopyTracked(const IntervalBTreeCopyTracked &other)
      : m_value(other.m_value) {
    ++copies;
  }

  IntervalBTreeCopyTracked(IntervalBTreeCopyTracked &&) noexcept = default;
};

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

  EXPECT_THROW(tree.add({5, 2}, 7), std::invalid_argument);

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

  EXPECT_THROW(tree.add({5, 2}, 99), std::invalid_argument);

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

TEST(IntervalBTree, DuplicateOrderingCallbackCannotMutateTreeReentrantly) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({1, 5}, 20));

  EXPECT_THROW(tree.enumerateCanonical([&tree](int lhs, int rhs) {
    tree.add({10, 15}, 30);
    return lhs < rhs;
  }),
    std::logic_error);

  EXPECT_EQ(tree.size(), 2U);
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

TEST(IntervalBTree, SupportsStringPayload) {
  dmn::Dmn_IntervalBTree<std::string> tree;

  const std::string payload = "stored";
  ASSERT_TRUE(tree.add({1, 3}, payload));

  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].second, "stored");
  EXPECT_EQ(payload, "stored");
}

TEST(IntervalBTree, SupportsStructPayload) {
  struct Payload {
    std::string m_name;
    int m_priority;
  };

  dmn::Dmn_IntervalBTree<Payload> tree;
  const Payload payload{"worker", 4};

  ASSERT_TRUE(tree.add({1, 3}, payload));
  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].second.m_name, "worker");
  EXPECT_EQ(entries[0].second.m_priority, 4);
}

TEST(IntervalBTree, PayloadDoesNotAffectDefaultRangeOrdering) {
  dmn::Dmn_IntervalBTree<std::string> tree;

  ASSERT_TRUE(tree.add({5, 7}, "a-first-payload"));
  ASSERT_TRUE(tree.add({1, 3}, "z-second-payload"));

  const auto entries = tree.enumerateCanonical();

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].first.m_start, 1);
  EXPECT_EQ(entries[0].second, "z-second-payload");
  EXPECT_EQ(entries[1].first.m_start, 5);
  EXPECT_EQ(entries[1].second, "a-first-payload");
}

TEST(IntervalBTree, MoveOnlyPayloadCanBeInserted) {
  dmn::Dmn_IntervalBTree<std::unique_ptr<int>> tree;
  auto payload = std::make_unique<int>(42);

  ASSERT_TRUE(tree.add({1, 3}, std::move(payload)));

  EXPECT_FALSE(payload);
  EXPECT_FALSE(tree.empty());
  EXPECT_EQ(tree.size(), 1U);
}

TEST(IntervalBTree, InvalidMoveInsertionDoesNotConsumePayload) {
  dmn::Dmn_IntervalBTree<std::unique_ptr<int>> tree;
  auto payload = std::make_unique<int>(42);

  EXPECT_THROW(tree.add({5, 2}, std::move(payload)), std::invalid_argument);

  ASSERT_TRUE(payload);
  EXPECT_EQ(*payload, 42);
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(tree.size(), 0U);
}

TEST(IntervalBTree, CopyAndMoveInsertionOverloads) {
  dmn::Dmn_IntervalBTree<std::string> tree;

  std::string copiedPayload = "copy";
  ASSERT_TRUE(tree.add({1, 2}, copiedPayload));
  copiedPayload = "changed";

  std::string movedPayload = "move";
  ASSERT_TRUE(tree.add({3, 4}, std::move(movedPayload)));

  const auto entries = tree.enumerateCanonical();
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].second, "copy");
  EXPECT_EQ(entries[1].second, "move");
}

TEST(IntervalBTree, RootStoragePreservesMultipleEntries) {
  dmn::Dmn_IntervalBTree<int> tree;

  for (std::int64_t start = 8; start > 0; --start) {
    ASSERT_TRUE(tree.add({start, start + 2}, static_cast<int>(start)));
  }

  EXPECT_EQ(tree.size(), 8U);
  EXPECT_FALSE(tree.empty());

  const auto entries = tree.enumerateCanonical();
  ASSERT_EQ(entries.size(), 8U);

  for (std::size_t index = 0; index < entries.size(); ++index) {
    EXPECT_EQ(entries[index].first.m_start, index + 1);
    EXPECT_EQ(entries[index].second, static_cast<int>(index + 1));
  }
}

TEST(IntervalBTree, NodeStoresMultipleKeys) {
  dmn::Dmn_IntervalBTree<int> tree;

  ASSERT_TRUE(tree.add({3, 3}, 3));
  ASSERT_TRUE(tree.add({1, 1}, 1));
  ASSERT_TRUE(tree.add({2, 2}, 2));

  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  EXPECT_TRUE(Access::rootIsLeaf(tree));
  EXPECT_EQ(Access::rootKeyCount(tree), 3U);
  EXPECT_TRUE(Access::validate(tree));
}

TEST(IntervalBTree, RootSplitCreatesOwnedRoot) {
  dmn::Dmn_IntervalBTree<int> tree;

  for (std::int64_t start = 1; start <= 4; ++start) {
    ASSERT_TRUE(tree.add({start, start}, static_cast<int>(start)));
  }

  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  EXPECT_FALSE(Access::rootIsLeaf(tree));
  EXPECT_EQ(Access::rootChildCount(tree), 2U);
  EXPECT_TRUE(Access::validate(tree));
}

TEST(IntervalBTree, NodeChildrenPartitionCorrectly) {
  dmn::Dmn_IntervalBTree<int> tree;

  for (std::int64_t start = 80; start >= 1; --start) {
    ASSERT_TRUE(tree.add({start, start + 2}, static_cast<int>(start)));
  }

  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  EXPECT_TRUE(Access::validate(tree));

  const auto entries = tree.enumerateCanonical();
  ASSERT_EQ(entries.size(), 80U);

  for (std::size_t index = 0; index < entries.size(); ++index) {
    EXPECT_EQ(entries[index].first.m_start, index + 1);
  }
}

TEST(IntervalBTree, InsertionSplitsNodes) {
  dmn::Dmn_IntervalBTree<int> tree;

  for (std::int64_t start = 1; start <= 100; ++start) {
    ASSERT_TRUE(tree.add({start, start}, static_cast<int>(start)));
  }

  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  EXPECT_FALSE(Access::rootIsLeaf(tree));
  EXPECT_TRUE(Access::validate(tree));
}

TEST(IntervalBTree, InsertionDeterministicAcrossOrders) {
  dmn::Dmn_IntervalBTree<int> ascendingTree;
  dmn::Dmn_IntervalBTree<int> descendingTree;

  for (std::int64_t start = 1; start <= 40; ++start) {
    ASSERT_TRUE(
        ascendingTree.add({start, start + 1}, static_cast<int>(start * 10)));
  }

  for (std::int64_t start = 40; start >= 1; --start) {
    ASSERT_TRUE(
        descendingTree.add({start, start + 1}, static_cast<int>(start * 10)));
  }

  const auto ascendingEntries = ascendingTree.enumerateCanonical();
  const auto descendingEntries = descendingTree.enumerateCanonical();
  ASSERT_EQ(ascendingEntries.size(), descendingEntries.size());

  for (std::size_t index = 0; index < ascendingEntries.size(); ++index) {
    EXPECT_EQ(ascendingEntries[index].first.m_start,
              descendingEntries[index].first.m_start);
    EXPECT_EQ(ascendingEntries[index].first.m_end,
              descendingEntries[index].first.m_end);
    EXPECT_EQ(ascendingEntries[index].second, descendingEntries[index].second);
  }
}

TEST(IntervalBTree, EnumerationIsIndependentOfNodeSplits) {
  dmn::Dmn_IntervalBTree<int> insertionOrderTree;
  dmn::Dmn_IntervalBTree<int> splitOrderTree;

  for (std::int64_t start = 1; start <= 25; ++start) {
    ASSERT_TRUE(
        insertionOrderTree.add({start, start + 2}, static_cast<int>(start)));
  }

  for (std::int64_t start = 25; start >= 1; --start) {
    ASSERT_TRUE(
        splitOrderTree.add({start, start + 2}, static_cast<int>(start)));
  }

  const auto first = insertionOrderTree.enumerateCanonical();
  const auto second = splitOrderTree.enumerateCanonical();
  ASSERT_EQ(first.size(), second.size());

  for (std::size_t index = 0; index < first.size(); ++index) {
    EXPECT_EQ(first[index].first.m_start, second[index].first.m_start);
    EXPECT_EQ(first[index].first.m_end, second[index].first.m_end);
    EXPECT_EQ(first[index].second, second[index].second);
  }
}

TEST(IntervalBTree, SplitPreservesEntryValuesAndOrdinals) {
  dmn::Dmn_IntervalBTree<int> tree;

  ASSERT_TRUE(tree.add({5, 5}, 100));

  for (std::int64_t start = 1; start <= 20; ++start) {
    ASSERT_TRUE(tree.add({start, start}, static_cast<int>(start)));
  }

  ASSERT_TRUE(tree.add({5, 5}, 200));

  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  EXPECT_TRUE(Access::validate(tree));

  const auto entries = tree.enumerateCanonical();
  std::vector<int> duplicateValues;

  for (const auto &entry : entries) {
    if (entry.first.m_start == 5 && entry.first.m_end == 5) {
      duplicateValues.push_back(entry.second);
    }
  }

  ASSERT_EQ(duplicateValues.size(), 3U);
  EXPECT_EQ(duplicateValues[0], 100);
  EXPECT_EQ(duplicateValues[1], 5);
  EXPECT_EQ(duplicateValues[2], 200);
}

TEST(IntervalBTree, FindOverlappingSingle) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({8, 12}, 20));

  const auto entries = tree.findOverlapping({3, 4});

  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].first.m_start, 1);
  EXPECT_EQ(entries[0].first.m_end, 5);
  EXPECT_EQ(entries[0].second, 10);
}

TEST(IntervalBTree, FindOverlappingMultipleInCanonicalOrder) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({10, 20}, 3));
  ASSERT_TRUE(tree.add({1, 8}, 1));
  ASSERT_TRUE(tree.add({5, 12}, 2));
  ASSERT_TRUE(tree.add({30, 40}, 4));

  const auto entries = tree.findOverlapping({7, 11});

  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].second, 1);
  EXPECT_EQ(entries[1].second, 2);
  EXPECT_EQ(entries[2].second, 3);
}

TEST(IntervalBTree, FindOverlappingIncludesSharedEndpoints) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 2}, 1));
  ASSERT_TRUE(tree.add({3, 4}, 3));

  const auto entries = tree.findOverlapping({2, 3});

  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].second, 1);
  EXPECT_EQ(entries[1].second, 3);
}

TEST(IntervalBTree, FindOverlappingRejectsInvalidQuery) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  EXPECT_THROW(tree.findOverlapping({5, 1}), std::invalid_argument);

  EXPECT_TRUE(tree.findOverlapping({6, 7}).empty());
}

TEST(IntervalBTree, HasOverlapRejectsInvalidQuery) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  EXPECT_FALSE(tree.hasOverlap({5, 1}));
}

TEST(IntervalBTree, HasOverlapFindsAndRejectsNonOverlappingRanges) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  EXPECT_TRUE(tree.hasOverlap({5, 8}));
  EXPECT_FALSE(tree.hasOverlap({6, 8}));
}

TEST(IntervalBTree, OverlapVisitorAvoidsPayloadCopies) {
  dmn::Dmn_IntervalBTree<IntervalBTreeCopyTracked> tree;
  const IntervalBTreeCopyTracked first(10);
  const IntervalBTreeCopyTracked second(20);
  ASSERT_TRUE(tree.add({1, 5}, first));
  ASSERT_TRUE(tree.add({3, 8}, second));
  IntervalBTreeCopyTracked::copies = 0;

  std::vector<int> visited;
  tree.forEachOverlapping({4, 4}, [&visited](const auto &, const auto &value) {
    visited.push_back(value.m_value);
  });

  EXPECT_EQ(IntervalBTreeCopyTracked::copies, 0U);
  EXPECT_EQ(visited, (std::vector<int>{10, 20}));
}

TEST(IntervalBTree, OverlapVisitorRejectsInvalidQueryWithoutInvocation) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  std::size_t calls = 0;
  EXPECT_THROW(tree.forEachOverlapping(
                   {5, 1}, [&calls](const auto &, const auto &) { ++calls; }),
               std::invalid_argument);

  EXPECT_EQ(calls, 0U);
}

TEST(IntervalBTree, OverlapVisitorRequiresCallableVisitor) {
  dmn::Dmn_IntervalBTree<int> tree;

  EXPECT_THROW(tree.forEachOverlapping({1, 5}, {}), std::invalid_argument);
}

TEST(IntervalBTree, OverlapVisitorSupportsMoveOnlyPayload) {
  dmn::Dmn_IntervalBTree<std::unique_ptr<int>> tree;
  ASSERT_TRUE(tree.add({1, 5}, std::make_unique<int>(42)));

  int visitedValue = 0;
  tree.forEachOverlapping({2, 3},
                          [&visitedValue](const auto &, const auto &value) {
                            visitedValue = *value;
                          });

  EXPECT_EQ(visitedValue, 42);
}

TEST(IntervalBTree, OverlapVisitorCannotMutateTreeReentrantly) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  EXPECT_THROW(
      tree.forEachOverlapping(
          {2, 3}, [&tree](const auto &, const auto &) { tree.clear(); }),
      std::logic_error);
  EXPECT_EQ(tree.size(), 1U);
}

TEST(IntervalBTree, TopologyReturnsClearWhenEmpty) {
  dmn::Dmn_IntervalBTree<int> tree;

  const auto result = tree.queryTopology({1, 5}, 10);

  EXPECT_EQ(result.m_status, dmn::Dmn_OverlayTopology::Clear);
  EXPECT_TRUE(result.m_isTop);
  EXPECT_TRUE(result.m_overlappingEntries.empty());
}

TEST(IntervalBTree, TopologyIdentifiesOverlaidLeftAndRight) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({10, 15}, 1));

  EXPECT_EQ(tree.queryTopology({13, 17}, 2).m_status,
            dmn::Dmn_OverlayTopology::OverlaidLeft);
  EXPECT_EQ(tree.queryTopology({8, 12}, 3).m_status,
            dmn::Dmn_OverlayTopology::OverlaidRight);
}

TEST(IntervalBTree, TopologyIdentifiesFullyCovered) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({10, 20}, 1));

  EXPECT_EQ(tree.queryTopology({12, 18}, 2).m_status,
            dmn::Dmn_OverlayTopology::FullyCovered);
}

TEST(IntervalBTree, TopologyPrecedenceIsDeterministic) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({10, 15}, 1));
  ASSERT_TRUE(tree.add({15, 20}, 2));

  EXPECT_EQ(tree.queryTopology({10, 20}, 3).m_status,
            dmn::Dmn_OverlayTopology::FullyCovered);
}

TEST(IntervalBTree, TopologyExactMatchIsFullyCovered) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({10, 20}, 1));

  EXPECT_EQ(tree.queryTopology({10, 20}, 2).m_status,
            dmn::Dmn_OverlayTopology::FullyCovered);
}

TEST(IntervalBTree, TopologyIdentifiesCoveringExisting) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({10, 15}, 1));

  EXPECT_EQ(tree.queryTopology({5, 20}, 2).m_status,
            dmn::Dmn_OverlayTopology::CoveringExisting);
}

TEST(IntervalBTree, TopologyHandlesContiguousCoverageAndCoverageGaps) {
  dmn::Dmn_IntervalBTree<int> contiguousTree;
  ASSERT_TRUE(contiguousTree.add({10, 12}, 1));
  ASSERT_TRUE(contiguousTree.add({13, 15}, 2));
  EXPECT_EQ(contiguousTree.queryTopology({11, 14}, 3).m_status,
            dmn::Dmn_OverlayTopology::FullyCovered);

  dmn::Dmn_IntervalBTree<int> gapTree;
  ASSERT_TRUE(gapTree.add({5, 10}, 1));
  ASSERT_TRUE(gapTree.add({20, 25}, 2));
  EXPECT_EQ(gapTree.queryTopology({8, 22}, 3).m_status,
            dmn::Dmn_OverlayTopology::OverlaidBoth);
}

TEST(IntervalBTree, TopologyHandlesInt64BoundariesWithoutOverflow) {
  using Limit = std::numeric_limits<std::int64_t>;
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({Limit::min(), Limit::min() + 2}, 1));
  ASSERT_TRUE(tree.add({Limit::min() + 3, Limit::max()}, 2));

  EXPECT_EQ(tree.queryTopology({Limit::min(), Limit::max()}, 3).m_status,
            dmn::Dmn_OverlayTopology::FullyCovered);
}

TEST(IntervalBTree, QueryTopologyIsHypothetical) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  const auto result = tree.queryTopology({3, 8}, 20);

  EXPECT_EQ(result.m_status, dmn::Dmn_OverlayTopology::OverlaidLeft);
  ASSERT_EQ(result.m_overlappingEntries.size(), 1U);
  EXPECT_EQ(result.m_overlappingEntries[0].second, 10);
  EXPECT_EQ(tree.size(), 1U);
}

TEST(IntervalBTree, TopologyPriorityUsesEvaluatorAndDefaultsToTop) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree noEvaluator;
  ASSERT_TRUE(noEvaluator.add({1, 5}, 10));
  EXPECT_TRUE(noEvaluator.queryTopology({3, 7}, 5).m_isTop);

  Tree priorityTree({}, [](int lhs, int rhs) { return lhs > rhs; });
  ASSERT_TRUE(priorityTree.add({1, 5}, 10));
  EXPECT_FALSE(priorityTree.queryTopology({3, 7}, 5).m_isTop);
  EXPECT_TRUE(priorityTree.queryTopology({3, 7}, 15).m_isTop);
  EXPECT_TRUE(priorityTree.queryTopology({3, 7}, 10).m_isTop);
}

TEST(IntervalBTree, InvalidTopologyQueryReturnsClearWithoutMutation) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  auto result = tree.queryTopology({6, 7}, 20);

  EXPECT_EQ(result.m_status, dmn::Dmn_OverlayTopology::Clear);
  EXPECT_TRUE(result.m_overlappingEntries.empty());
  EXPECT_EQ(tree.size(), 1U);

  EXPECT_THROW(result = tree.queryTopology({6, 3}, 20), std::invalid_argument);
}

TEST(IntervalBTree, AddWithTopologyMatchesHypotheticalQuery) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  const auto queried = tree.queryTopology({3, 8}, 20);

  EXPECT_THROW(tree.addWithTopology({3, 2}, 20), std::invalid_argument);

  const auto [added, inserted] = tree.addWithTopology({3, 8}, 20);

  ASSERT_TRUE(added);
  EXPECT_EQ(inserted.m_status, queried.m_status);
  EXPECT_EQ(inserted.m_isTop, queried.m_isTop);
  ASSERT_EQ(inserted.m_overlappingEntries.size(), 1U);
  EXPECT_EQ(inserted.m_overlappingEntries[0].second, 10);
  EXPECT_EQ(tree.size(), 2U);
}

TEST(IntervalBTree, NewEntryCallbackIsNotInvokedOnInsertion) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::size_t calls = 0;

  const auto [added, result] =
      tree.addWithTopology({1, 5}, 10,
                           [&calls](const auto &, const auto &, const auto &,
                                    const auto &) { ++calls; });

  EXPECT_TRUE(added);
  EXPECT_EQ(result.m_status, dmn::Dmn_OverlayTopology::Clear);
  EXPECT_EQ(calls, 0U);
}

TEST(IntervalBTree, StateCallbackReceivesTopologyTransition) {
  dmn::Dmn_IntervalBTree<int> tree;
  dmn::Dmn_OverlayState oldState;
  dmn::Dmn_OverlayState newState;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&calls, &oldState,
                        &newState](const auto &, const auto &,
                                   const auto &oldValue, const auto &newValue) {
                         ++calls;
                         oldState = oldValue;
                         newState = newValue;
                       }));

  ASSERT_TRUE(tree.add({3, 8}, 20));

  ASSERT_EQ(calls, 1U);
  EXPECT_EQ(oldState.m_topology, dmn::Dmn_OverlayTopology::Clear);
  EXPECT_EQ(newState.m_topology, dmn::Dmn_OverlayTopology::OverlaidRight);
}

TEST(IntervalBTree, StateCallbackReceivesPriorityTransition) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree({}, [](int lhs, int rhs) { return lhs > rhs; });
  dmn::Dmn_OverlayState oldState;
  dmn::Dmn_OverlayState newState;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&calls, &oldState,
                        &newState](const auto &, const auto &,
                                   const auto &oldValue, const auto &newValue) {
                         ++calls;
                         oldState = oldValue;
                         newState = newValue;
                       }));

  ASSERT_TRUE(tree.add({3, 8}, 20));

  ASSERT_EQ(calls, 1U);
  EXPECT_TRUE(oldState.m_isTop);
  EXPECT_FALSE(newState.m_isTop);
}

TEST(IntervalBTree, MoveOnlyPayloadSupportsStateCallback) {
  dmn::Dmn_IntervalBTree<std::unique_ptr<int>> tree;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add({1, 5}, std::make_unique<int>(10),
                       [&calls](const auto &, const auto &, const auto &,
                                const auto &) { ++calls; }));
  ASSERT_TRUE(tree.add({3, 8}, std::make_unique<int>(20)));

  EXPECT_EQ(calls, 1U);
}

TEST(IntervalBTree, RangeEndpointInsertionOverloadsForwardCallbacks) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add(1, 5, 10,
                       [&calls](const auto &, const auto &, const auto &,
                                const auto &) { ++calls; }));
  ASSERT_TRUE(tree.add(3, 8, 20));

  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(tree.size(), 2U);
}

TEST(IntervalBTree, InsertionNotifiesNonAdjacentAffectedEntries) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree({}, [](int lhs, int rhs) { return lhs > rhs; });
  std::vector<int> notified;
  ASSERT_TRUE(
      tree.add({1, 4}, 1,
               [&notified](const auto &, const auto &value, const auto &,
                           const auto &) { notified.push_back(value); }));
  ASSERT_TRUE(
      tree.add({5, 8}, 2,
               [&notified](const auto &, const auto &value, const auto &,
                           const auto &) { notified.push_back(value); }));
  ASSERT_TRUE(
      tree.add({9, 12}, 3,
               [&notified](const auto &, const auto &value, const auto &,
                           const auto &) { notified.push_back(value); }));
  ASSERT_TRUE(
      tree.add({13, 16}, 4,
               [&notified](const auto &, const auto &value, const auto &,
                           const auto &) { notified.push_back(value); }));

  ASSERT_TRUE(tree.add({2, 15}, 20));

  EXPECT_EQ(notified, (std::vector<int>{1, 2, 3, 4}));
}

TEST(IntervalBTree, CallbackExceptionLeavesTreeConsistentAndReusable) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add(
      {1, 5}, 10, [](const auto &, const auto &, const auto &, const auto &) {
        throw std::runtime_error("callback failure");
      }));

  EXPECT_THROW(tree.add({3, 8}, 20), std::runtime_error);
  EXPECT_EQ(tree.size(), 2U);
  EXPECT_TRUE(tree.hasOverlap({4, 4}));

  EXPECT_TRUE(tree.add({20, 25}, 30));
  EXPECT_EQ(tree.size(), 3U);
}

TEST(IntervalBTree, ReentrantMoveEnumerationFromCallbackIsRejected) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(
      tree.add({1, 5}, 10,
               [&tree](const auto &, const auto &, const auto &, const auto &) {
                 tree.enumerateCanonicalMove();
               }));

  EXPECT_THROW(tree.add({3, 8}, 20), std::logic_error);
  EXPECT_EQ(tree.size(), 2U);
  EXPECT_TRUE(tree.add({20, 25}, 30));
}

TEST(IntervalBTree, ReentrantInsertionFromCallbackIsRejected) {
  dmn::Dmn_IntervalBTree<int> tree;
  bool tryReentrantInsert = true;
  ASSERT_TRUE(
      tree.add({1, 5}, 10,
               [&tree, &tryReentrantInsert](const auto &, const auto &,
                                            const auto &, const auto &) {
                 if (tryReentrantInsert) {
                   tryReentrantInsert = false;
                   tree.add({30, 35}, 30);
                 }
               }));

  EXPECT_THROW(tree.add({3, 8}, 20), std::logic_error);
  EXPECT_EQ(tree.size(), 2U);
  EXPECT_TRUE(tree.add({20, 25}, 40));
  EXPECT_EQ(tree.size(), 3U);
}

TEST(IntervalBTree, InsertionCallbacksFollowCanonicalOrder) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree([](const auto &lhs, const auto &rhs) {
    return lhs.m_start > rhs.m_start;
  });
  std::vector<int> notified;
  for (int index = 1; index <= 4; ++index) {
    ASSERT_TRUE(
        tree.add({index * 10, index * 10 + 4}, index,
                 [&notified](const auto &, const auto &value, const auto &,
                             const auto &) { notified.push_back(value); }));
  }

  ASSERT_TRUE(tree.add({5, 45}, 20));

  EXPECT_EQ(notified, (std::vector<int>{4, 3, 2, 1}));
}

TEST(IntervalBTree, TopologyUsesInsertionLevelForContainment) {
  dmn::Dmn_IntervalBTree<int> tree;
  dmn::Dmn_OverlayTopology oldEntryTopology = dmn::Dmn_OverlayTopology::Clear;
  ASSERT_TRUE(tree.add({5, 10}, 1,
                       [&oldEntryTopology](const auto &, const auto &,
                                           const auto &, const auto &newState) {
                         oldEntryTopology = newState.m_topology;
                       }));

  const auto result = tree.queryTopology({1, 15}, 2);
  ASSERT_EQ(result.m_status, dmn::Dmn_OverlayTopology::CoveringExisting);
  ASSERT_TRUE(tree.add({1, 15}, 2));

  EXPECT_EQ(oldEntryTopology, dmn::Dmn_OverlayTopology::FullyCovered);
}

TEST(IntervalBTree, ExactDuplicateInsertionKeepsBothEntriesFullyCovered) {
  dmn::Dmn_IntervalBTree<int> tree;
  dmn::Dmn_OverlayTopology oldEntryTopology = dmn::Dmn_OverlayTopology::Clear;
  ASSERT_TRUE(tree.add({5, 10}, 1,
                       [&oldEntryTopology](const auto &, const auto &,
                                           const auto &, const auto &newState) {
                         oldEntryTopology = newState.m_topology;
                       }));

  const auto [added, newEntry] = tree.addWithTopology({5, 10}, 2);

  ASSERT_TRUE(added);
  EXPECT_EQ(newEntry.m_status, dmn::Dmn_OverlayTopology::FullyCovered);
  EXPECT_EQ(oldEntryTopology, dmn::Dmn_OverlayTopology::FullyCovered);
}

TEST(IntervalBTree, RemoveByRangeRemovesOneExactEntry) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({6, 9}, 20));

  EXPECT_TRUE(tree.removeByRange({1, 5}));
  EXPECT_EQ(tree.size(), 1U);
  const auto entries = tree.enumerateCanonical();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].second, 20);
}

TEST(IntervalBTree, InvalidAndMissingRemovalLeaveTreeUnchanged) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  EXPECT_THROW(tree.removeByRange({5, 1}), std::invalid_argument);

  EXPECT_FALSE(tree.removeByRange({2, 5}));
  EXPECT_EQ(tree.size(), 1U);
  EXPECT_EQ(tree.enumerateCanonical()[0].second, 10);
}

TEST(IntervalBTree, RemoveByRangeMatchesDuplicateByOpaqueValue) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({1, 5}, 20));

  EXPECT_TRUE(
      tree.removeByRange({1, 5}, [](const int &value) { return value == 20; }));
  const auto entries = tree.enumerateCanonical();
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].second, 10);
}

TEST(IntervalBTree, FailedRemovalPredicateLeavesTreeUnchanged) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({1, 5}, 20));

  EXPECT_FALSE(
      tree.removeByRange({1, 5}, [](const int &value) { return value == 30; }));
  EXPECT_EQ(tree.size(), 2U);
  EXPECT_EQ(tree.enumerateCanonical()[0].second, 10);
  EXPECT_EQ(tree.enumerateCanonical()[1].second, 20);
}

TEST(IntervalBTree, RemoveAliasMatchesRemoveByRange) {
  dmn::Dmn_IntervalBTree<int> removeTree;
  dmn::Dmn_IntervalBTree<int> removeByRangeTree;
  ASSERT_TRUE(removeTree.add({1, 5}, 10));
  ASSERT_TRUE(removeByRangeTree.add({1, 5}, 10));

  EXPECT_EQ(removeTree.remove({1, 5}), removeByRangeTree.removeByRange({1, 5}));
  EXPECT_EQ(removeTree.empty(), removeByRangeTree.empty());
}

TEST(IntervalBTree, RemoveAllOverlappingReturnsExactCount) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({5, 10}, 20));
  ASSERT_TRUE(tree.add({11, 15}, 30));

  EXPECT_EQ(tree.removeAllOverlapping({5, 11}), 3U);
  EXPECT_TRUE(tree.empty());

  EXPECT_THROW(tree.removeAllOverlapping({5, 2}), std::invalid_argument);
}

TEST(IntervalBTree, RemovalRebalancesBTree) {
  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  dmn::Dmn_IntervalBTree<int> tree;

  for (int index = 0; index < 40; ++index) {
    ASSERT_TRUE(tree.add({index * 2, index * 2}, index));
  }

  ASSERT_TRUE(Access::validate(tree));

  for (int index = 0; index < 39; ++index) {
    ASSERT_TRUE(tree.removeByRange({index * 2, index * 2}));
    ASSERT_TRUE(Access::validate(tree));
  }

  EXPECT_TRUE(tree.removeByRange({78, 78}));
  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(Access::rootChildCount(tree), 0U);
  EXPECT_TRUE(Access::validate(tree));
}

TEST(IntervalBTree, RemovalMaintainsInvariantsAcrossMixedOrders) {
  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  dmn::Dmn_IntervalBTree<int> tree;
  constexpr int entryCount = 64;

  for (int index = 0; index < entryCount; ++index) {
    ASSERT_TRUE(tree.add({index * 3, index * 3}, index));
  }

  for (int step = 0; step < entryCount; ++step) {
    const int index = (step * 37) % entryCount;
    ASSERT_TRUE(tree.removeByRange({index * 3, index * 3}));
    ASSERT_TRUE(Access::validate(tree));
  }

  EXPECT_TRUE(tree.empty());
}

TEST(IntervalBTree, RemovalSupportsMoveOnlyPayloads) {
  dmn::Dmn_IntervalBTree<std::unique_ptr<int>> tree;
  ASSERT_TRUE(tree.add({1, 5}, std::make_unique<int>(10)));
  ASSERT_TRUE(tree.add({3, 8}, std::make_unique<int>(20)));

  EXPECT_EQ(tree.removeAllOverlapping({4, 4}), 2U);
  EXPECT_TRUE(tree.empty());
}

TEST(IntervalBTree, RemovalRecomputesSurvivorTopologyAndPriority) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree({}, [](int lhs, int rhs) { return lhs > rhs; });
  dmn::Dmn_OverlayState latestState;
  std::size_t callbacks = 0;
  ASSERT_TRUE(tree.add({10, 12}, 1));
  ASSERT_TRUE(tree.add({13, 15}, 2));
  ASSERT_TRUE(
      tree.add({11, 14}, 3,
               [&latestState, &callbacks](const auto &, const auto &,
                                          const auto &, const auto &newState) {
                 ++callbacks;
                 latestState = newState;
               }));

  ASSERT_TRUE(tree.add({11, 14}, 4));
  ASSERT_EQ(latestState.m_topology, dmn::Dmn_OverlayTopology::FullyCovered);
  EXPECT_FALSE(latestState.m_isTop);

  ASSERT_TRUE(tree.removeByRange({11, 14},
                                 [](const int &value) { return value == 4; }));

  EXPECT_TRUE(latestState.m_isTop);
  EXPECT_EQ(callbacks, 2U);
  ASSERT_TRUE(tree.removeByRange({10, 12}));
  EXPECT_EQ(latestState.m_topology, dmn::Dmn_OverlayTopology::OverlaidRight);
}

TEST(IntervalBTree, RemovalNotifiesFormerlyOverlaidEntry) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::vector<dmn::Dmn_OverlayState> transitions;
  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&transitions](const auto &, const auto &, const auto &,
                                      const auto &newState) {
                         transitions.push_back(newState);
                       }));
  ASSERT_TRUE(tree.add({3, 8}, 20));
  ASSERT_EQ(transitions.size(), 1U);
  EXPECT_EQ(transitions.back().m_topology,
            dmn::Dmn_OverlayTopology::OverlaidRight);

  ASSERT_TRUE(tree.removeByRange({3, 8}));

  ASSERT_EQ(transitions.size(), 2U);
  EXPECT_EQ(transitions.back().m_topology, dmn::Dmn_OverlayTopology::Clear);
}

TEST(IntervalBTree, BatchRemovalNotifiesEachSurvivorOnce) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add({0, 4}, 1,
                       [&calls](const auto &, const auto &, const auto &,
                                const auto &) { ++calls; }));
  ASSERT_TRUE(tree.add({0, 10}, 2));
  ASSERT_TRUE(tree.add({1, 10}, 3));
  calls = 0;

  EXPECT_EQ(tree.removeAllOverlapping({5, 6}), 2U);

  EXPECT_EQ(calls, 1U);
  EXPECT_EQ(tree.size(), 1U);
}

TEST(IntervalBTree, RemovalCallbackRunsAfterTreeMutation) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::size_t observedSize = 0;
  ASSERT_TRUE(tree.add(
      {1, 5}, 10,
      [&tree, &observedSize](const auto &, const auto &, const auto &,
                             const auto &) { observedSize = tree.size(); }));
  ASSERT_TRUE(tree.add({3, 8}, 20));

  ASSERT_TRUE(tree.removeByRange({3, 8}));

  EXPECT_EQ(observedSize, 1U);
}

TEST(IntervalBTree, RemovalCallbackExceptionLeavesTreeValid) {
  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  dmn::Dmn_IntervalBTree<int> tree;
  bool throwOnCallback = false;

  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&throwOnCallback](const auto &, const auto &,
                                          const auto &, const auto &) {
                         if (throwOnCallback) {
                           throwOnCallback = false;
                           throw std::runtime_error("callback failure");
                         }
                       }));

  ASSERT_TRUE(tree.add({3, 8}, 20));
  throwOnCallback = true;

  EXPECT_THROW(tree.removeByRange({3, 8}), std::runtime_error);
  EXPECT_EQ(tree.size(), 1U);
  EXPECT_TRUE(Access::validate(tree));
  EXPECT_TRUE(tree.add({20, 25}, 30));
}

TEST(IntervalBTree, RemovalCallbacksFollowCanonicalOrder) {
  using Tree = dmn::Dmn_IntervalBTree<int>;

  Tree tree([](const auto &lhs, const auto &rhs) {
    return lhs.m_start > rhs.m_start;
  });

  std::vector<int> notified;
  for (int index = 1; index <= 4; ++index) {
    ASSERT_TRUE(
        tree.add({index * 10, index * 10 + 4}, index,
                 [&notified](const auto &, const auto &value, const auto &,
                             const auto &) { notified.push_back(value); }));
  }

  ASSERT_TRUE(tree.add({5, 45}, 20));
  notified.clear();

  ASSERT_TRUE(tree.removeByRange({5, 45}));

  EXPECT_EQ(notified, (std::vector<int>{4, 3, 2, 1}));
}

TEST(IntervalBTree, RemovalPredicateCannotMutateTreeReentrantly) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));

  EXPECT_THROW(tree.removeByRange({1, 5},
                                  [&tree](const int &) {
                                    tree.add({10, 15}, 20);
                                    return true;
                                  }),
               std::logic_error);

  EXPECT_EQ(tree.size(), 1U);
  EXPECT_TRUE(tree.hasOverlap({1, 5}));
}

TEST(IntervalBTree, SubtreeMaxEndRemainsCorrectAcrossMutations) {
  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  dmn::Dmn_IntervalBTree<int> tree;

  for (int index = 0; index < 80; ++index) {
    const auto start = index * 7;
    ASSERT_TRUE(tree.add({start, start + (index % 9)}, index));
    ASSERT_TRUE(Access::validate(tree));
  }

  for (int index = 0; index < 80; index += 2) {
    ASSERT_TRUE(tree.removeByRange({index * 7, index * 7 + (index % 9)}));
    ASSERT_TRUE(Access::validate(tree));
  }
}

TEST(IntervalBTree, Int64BoundarySubtreeMaxEndRemainsCorrect) {
  using Access = dmn::detail::Dmn_IntervalBTreeTestAccess<int>;
  using Limit = std::numeric_limits<std::int64_t>;
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({Limit::min(), Limit::min()}, 1));
  ASSERT_TRUE(tree.add({Limit::max() - 1, Limit::max()}, 2));

  ASSERT_TRUE(Access::validate(tree));
  EXPECT_TRUE(tree.hasOverlap({Limit::max(), Limit::max()}));
  EXPECT_FALSE(tree.hasOverlap({0, 1}));
  ASSERT_TRUE(tree.removeByRange({Limit::min(), Limit::min()}));
  EXPECT_TRUE(Access::validate(tree));
  EXPECT_TRUE(tree.hasOverlap({Limit::max(), Limit::max()}));
}

TEST(IntervalBTree, PrunedOverlapQueriesMatchCanonicalBaseline) {
  dmn::Dmn_IntervalBTree<int> tree;

  for (int index = 0; index < 100; ++index) {
    const auto start = index * 10;
    ASSERT_TRUE(tree.add({start, start + 3}, index));
  }

  for (const dmn::Dmn_IntervalRange query :
       {dmn::Dmn_IntervalRange{1, 2}, {97, 113}, {495, 505}, {900, 1200}}) {
    std::vector<std::pair<std::int64_t, int>> expected;

    for (const auto &[range, value] : tree.enumerateCanonical()) {
      if (range.overlaps(query)) {
        expected.emplace_back(range.m_start, value);
      }
    }

    std::vector<std::pair<std::int64_t, int>> actual;
    for (const auto &[range, value] : tree.findOverlapping(query)) {
      actual.emplace_back(range.m_start, value);
    }

    EXPECT_EQ(actual, expected);
  }
}

TEST(IntervalBTree, ClearSuppressesCallbacksAndAllowsReuse) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&calls](const auto &, const auto &, const auto &,
                                const auto &) { ++calls; }));
  ASSERT_TRUE(tree.add({3, 8}, 20));
  calls = 0;

  tree.clear();

  EXPECT_TRUE(tree.empty());
  EXPECT_EQ(tree.size(), 0U);
  EXPECT_EQ(calls, 0U);
  ASSERT_TRUE(tree.add({1, 5}, 30));
  EXPECT_EQ(tree.size(), 1U);
}

TEST(IntervalBTree, ReentrantClearFromCallbackIsRejected) {
  dmn::Dmn_IntervalBTree<int> tree;
  bool tryReentrantClear = false;
  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&tree, &tryReentrantClear](const auto &, const auto &,
                                                   const auto &, const auto &) {
                         if (tryReentrantClear) {
                           tryReentrantClear = false;
                           tree.clear();
                         }
                       }));
  ASSERT_TRUE(tree.add({3, 8}, 20));
  tryReentrantClear = true;

  EXPECT_THROW(tree.add({0, 10}, 30), std::logic_error);
  EXPECT_EQ(tree.size(), 3U);
  EXPECT_TRUE(tree.add({20, 25}, 40));
}

TEST(IntervalBTree, MoveOnlyPayloadCanBeEnumeratedByMove) {
  dmn::Dmn_IntervalBTree<std::unique_ptr<int>> tree;
  ASSERT_TRUE(tree.add({3, 5}, std::make_unique<int>(30)));
  ASSERT_TRUE(tree.add({1, 2}, std::make_unique<int>(10)));

  auto entries = tree.enumerateCanonicalMove();

  ASSERT_TRUE(tree.empty());
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries[0].first.m_start, 1);
  EXPECT_EQ(*entries[0].second, 10);
  EXPECT_EQ(entries[1].first.m_start, 3);
  EXPECT_EQ(*entries[1].second, 30);
}

TEST(IntervalBTree, MoveEnumerationUsesDuplicateOrdering) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 30));
  ASSERT_TRUE(tree.add({1, 5}, 10));
  ASSERT_TRUE(tree.add({1, 5}, 20));

  const auto entries =
      tree.enumerateCanonicalMove([](int lhs, int rhs) { return lhs < rhs; });

  ASSERT_EQ(entries.size(), 3U);
  EXPECT_EQ(entries[0].second, 10);
  EXPECT_EQ(entries[1].second, 20);
  EXPECT_EQ(entries[2].second, 30);
  EXPECT_TRUE(tree.empty());
}

TEST(IntervalBTree, MoveEnumerationDoesNotDispatchCallbacks) {
  dmn::Dmn_IntervalBTree<int> tree;
  std::size_t calls = 0;
  ASSERT_TRUE(tree.add({1, 5}, 10,
                       [&calls](const auto &, const auto &, const auto &,
                                const auto &) { ++calls; }));
  ASSERT_TRUE(tree.add({3, 8}, 20));
  calls = 0;

  const auto entries = tree.enumerateCanonicalMove();

  EXPECT_EQ(entries.size(), 2U);
  EXPECT_EQ(calls, 0U);
}

TEST(IntervalBTree, CanonicalReconstructionPreservesTopology) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree source({}, [](int lhs, int rhs) { return lhs < rhs; });
  ASSERT_TRUE(source.add({1, 8}, 1));
  ASSERT_TRUE(source.add({4, 10}, 2));
  ASSERT_TRUE(source.add({12, 15}, 3));
  const auto canonical = source.enumerateCanonical();

  Tree destination({}, [](int lhs, int rhs) { return lhs < rhs; });
  destination.reconstructFromCanonical(canonical);

  const auto reconstructed = destination.enumerateCanonical();
  ASSERT_EQ(reconstructed.size(), canonical.size());

  for (std::size_t index = 0; index < canonical.size(); ++index) {
    EXPECT_EQ(reconstructed[index].first.m_start,
              canonical[index].first.m_start);
    EXPECT_EQ(reconstructed[index].first.m_end, canonical[index].first.m_end);
    EXPECT_EQ(reconstructed[index].second, canonical[index].second);
  }

  const auto reconstructedOverlaps = destination.findOverlapping({5, 12});
  const auto sourceOverlaps = source.findOverlapping({5, 12});
  ASSERT_EQ(reconstructedOverlaps.size(), sourceOverlaps.size());

  for (std::size_t index = 0; index < sourceOverlaps.size(); ++index) {
    EXPECT_EQ(reconstructedOverlaps[index].first.m_start,
              sourceOverlaps[index].first.m_start);
    EXPECT_EQ(reconstructedOverlaps[index].first.m_end,
              sourceOverlaps[index].first.m_end);
    EXPECT_EQ(reconstructedOverlaps[index].second,
              sourceOverlaps[index].second);
  }

  EXPECT_EQ(destination.queryTopology({5, 12}, 4).m_status,
            source.queryTopology({5, 12}, 4).m_status);
}

TEST(IntervalBTree, ReconstructionValidationFailureLeavesTreeUnchanged) {
  dmn::Dmn_IntervalBTree<int> tree;
  ASSERT_TRUE(tree.add({1, 5}, 10));
  const std::vector<std::pair<dmn::Dmn_IntervalRange, int>> invalidEntries{
      {{7, 3}, 20}};

  EXPECT_THROW(tree.reconstructFromCanonical(invalidEntries),
               std::invalid_argument);
  EXPECT_EQ(tree.size(), 1U);
  EXPECT_EQ(tree.enumerateCanonical()[0].second, 10);
}

TEST(IntervalBTree, ReconstructionReconnectsRegisteredCallbacks) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree;
  auto context = std::make_shared<int>(73);
  int observedContext = 0;
  int observedValue = 0;
  const auto registration = tree.registerStateCallback(
      [](const int &value) { return value == 10; }, context,
      [&observedContext,
       &observedValue](const Tree::callback_context &callbackContext,
                       const int &value, const auto &, const auto &) {
        observedContext = *std::static_pointer_cast<int>(callbackContext);
        observedValue = value;
      });
  const std::vector<std::pair<dmn::Dmn_IntervalRange, int>> entries{
      {{1, 5}, 10}};

  tree.reconstructFromCanonical(entries);
  EXPECT_EQ(observedValue, 0);
  ASSERT_TRUE(tree.add({3, 8}, 20));

  EXPECT_EQ(observedContext, 73);
  EXPECT_EQ(observedValue, 10);
  EXPECT_EQ(registration, 1U);
}

TEST(IntervalBTree, UnregisterCallbackStopsFutureDispatch) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree;
  std::size_t calls = 0;
  const auto registration = tree.registerStateCallback(
      [](const int &) { return true; }, {},
      [&calls](const auto &, const auto &, const auto &, const auto &) {
        ++calls;
      });

  tree.reconstructFromCanonical({{{1, 5}, 10}});
  tree.unregisterStateCallback(registration);

  ASSERT_TRUE(tree.add({3, 8}, 20));

  EXPECT_EQ(calls, 0U);
}

TEST(IntervalBTree, ReconstructionUsesFirstMatchingRegistration) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  Tree tree;
  std::vector<int> selected;
  tree.registerStateCallback(
      [](const int &value) { return value > 0; }, {},
      [&selected](const auto &, const int &, const auto &, const auto &) {
        selected.push_back(1);
      });
  tree.registerStateCallback(
      [](const int &) { return true; }, {},
      [&selected](const auto &, const int &, const auto &, const auto &) {
        selected.push_back(2);
      });
  tree.reconstructFromCanonical({{{1, 5}, 10}});

  ASSERT_TRUE(tree.add({3, 8}, 20));

  EXPECT_EQ(selected, (std::vector<int>{1}));
}

TEST(IntervalBTree, ReconstructionPreservesDuplicateCanonicalOrder) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  const auto order = [](int lhs, int rhs) { return lhs < rhs; };
  Tree source;
  ASSERT_TRUE(source.add({1, 5}, 30));
  ASSERT_TRUE(source.add({1, 5}, 10));
  ASSERT_TRUE(source.add({1, 5}, 20));
  const auto entries = source.enumerateCanonical(order);

  Tree destination;
  destination.reconstructFromCanonical(entries, order);
  const auto reconstructed = destination.enumerateCanonical(order);

  ASSERT_EQ(reconstructed.size(), entries.size());
  for (std::size_t index = 0; index < entries.size(); ++index) {
    EXPECT_EQ(reconstructed[index].second, entries[index].second);
  }
}

TEST(IntervalBTree, RebuildMatchesOriginalCanonicalOrder) {
  using Tree = dmn::Dmn_IntervalBTree<int>;
  const auto rangeOrder = [](const auto &lhs, const auto &rhs) {
    return lhs.m_start > rhs.m_start;
  };

  const auto duplicateOrder = [](int lhs, int rhs) { return lhs < rhs; };
  Tree source(rangeOrder);
  ASSERT_TRUE(source.add({1, 5}, 30));
  ASSERT_TRUE(source.add({10, 20}, 40));
  ASSERT_TRUE(source.add({1, 5}, 10));
  ASSERT_TRUE(source.add({5, 12}, 20));
  const auto snapshot = source.enumerateCanonical(duplicateOrder);

  Tree destination(rangeOrder);
  ASSERT_TRUE(destination.add({100, 110}, 99));
  destination.reconstructFromCanonical(snapshot, duplicateOrder);
  const auto rebuilt = destination.enumerateCanonical(duplicateOrder);

  ASSERT_EQ(rebuilt.size(), snapshot.size());

  for (std::size_t index = 0; index < snapshot.size(); ++index) {
    EXPECT_EQ(rebuilt[index].first.m_start, snapshot[index].first.m_start);
    EXPECT_EQ(rebuilt[index].first.m_end, snapshot[index].first.m_end);
    EXPECT_EQ(rebuilt[index].second, snapshot[index].second);
  }
}

TEST(IntervalBTree, DestructorReleasesAllUniqueOwnedEntries) {
  std::vector<std::weak_ptr<int>> values;
  {
    dmn::Dmn_IntervalBTree<std::shared_ptr<int>> tree;
    for (int index = 0; index < 256; ++index) {
      auto value = std::make_shared<int>(index);
      values.push_back(value);
      ASSERT_TRUE(tree.add({index * 2, index * 2}, std::move(value)));
    }
  }

  for (const auto &value : values) {
    EXPECT_TRUE(value.expired());
  }
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
