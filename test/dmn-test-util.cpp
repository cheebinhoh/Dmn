/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-util.cpp
 * @brief Unit tests for the general utility helpers.
 */

#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

#include "dmn-util.hpp"

template <typename T>
concept CanIncrementByOne = requires(T value) { dmn::incrementByOne(value); };

static_assert(CanIncrementByOne<int>);
static_assert(CanIncrementByOne<unsigned>);
static_assert(!CanIncrementByOne<bool>);

using NoopCleanup = decltype([]() noexcept {});
using ThrowingCleanup = decltype([]() {});
static_assert(!std::copy_constructible<dmn::ScopeGuard<NoopCleanup>>);
static_assert(std::move_constructible<dmn::ScopeGuard<NoopCleanup>>);
template <typename F>
concept CanMakeScopeGuard =
    requires(F &&cleanup) { dmn::make_scope_guard(std::forward<F>(cleanup)); };
static_assert(!CanMakeScopeGuard<ThrowingCleanup>);

TEST(DmnUtilTest, IncrementByOneKeepsValuesAtLeastOne) {
  EXPECT_EQ(dmn::incrementByOne(0), 1);
  EXPECT_EQ(dmn::incrementByOne(1), 2);
  EXPECT_EQ(dmn::incrementByOne(-5), 1);
}

TEST(DmnUtilTest, IncrementByOneSaturatesSignedMaximum) {
  EXPECT_EQ(dmn::incrementByOne(std::numeric_limits<int>::max()),
            std::numeric_limits<int>::max());
  EXPECT_EQ(dmn::incrementByOne(std::numeric_limits<signed char>::max()),
            std::numeric_limits<signed char>::max());
}

TEST(DmnUtilTest, IncrementByOneWrapsUnsignedMaximumToOne) {
  EXPECT_EQ(dmn::incrementByOne(std::numeric_limits<unsigned>::max()), 1u);
  EXPECT_EQ(dmn::incrementByOne(std::numeric_limits<unsigned char>::max()),
            static_cast<unsigned char>(1));
}

TEST(DmnUtilTest, StringComparePreservesAsciiBehavior) {
  EXPECT_TRUE(dmn::stringCompare("Hello", "hello"));
  EXPECT_FALSE(dmn::stringCompare("Foo", "Bar", false));
  EXPECT_FALSE(dmn::stringCompare("Hello", "hello", false));
}

TEST(DmnUtilTest, StringCompareUsesUnicodeCaseFolding) {
  EXPECT_TRUE(dmn::stringCompare("Stra\xC3\x9F"
                                 "e",
                                 "STRASSE"));
  EXPECT_TRUE(dmn::stringCompare("\xCE\x9F\xCE\xA3", "\xCE\xBF\xCF\x82"));
}

TEST(DmnUtilTest, StringCompareMatchesCanonicalEquivalents) {
  EXPECT_TRUE(dmn::stringCompare("\xC3\xA9", "e\xCC\x81"));
}

TEST(DmnUtilTest, StringCompareRejectsMalformedUtf8OnlyWhenFolding) {
  const std::string_view malformed{"\xC3", 1};
  EXPECT_THROW(dmn::stringCompare(malformed, malformed), std::invalid_argument);
  EXPECT_TRUE(dmn::stringCompare(malformed, malformed, false));
}

TEST(DmnUtilTest, ScopeGuardRunsCleanupOnce) {
  int calls = 0;

  {
    auto guard = dmn::make_scope_guard([&]() noexcept { ++calls; });
    static_cast<void>(guard);
  }

  EXPECT_EQ(calls, 1);
}

TEST(DmnUtilTest, ScopeGuardReleaseDismissesCleanup) {
  int calls = 0;

  {
    auto guard = dmn::make_scope_guard([&]() noexcept { ++calls; });
    guard.release();
  }

  EXPECT_EQ(calls, 0);
}

TEST(DmnUtilTest, ScopeGuardMoveTransfersCleanupResponsibility) {
  int calls = 0;

  {
    auto source = dmn::make_scope_guard([&]() noexcept { ++calls; });
    auto destination = std::move(source);
    static_cast<void>(destination);
  }

  EXPECT_EQ(calls, 1);
}
