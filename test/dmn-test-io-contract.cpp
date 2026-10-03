/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-io-contract.cpp
 * @brief Focused contract tests for Dmn_Io and synchronous Dmn_Pipe APIs.
 */

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "dmn-io.hpp"
#include "dmn-pipe.hpp"

namespace {

class TestIo : public dmn::Dmn_Io<int> {
public:
  auto read() -> std::optional<int> override { return {}; }
  void write(const int &) override {}
  void write(int &&) override {}
};

} // namespace

TEST(DmnIoContract, DefaultBulkReadReturnsEmptyVector) {
  TestIo io;
  dmn::Dmn_Io<int> &interface = io;

  EXPECT_TRUE(interface.read(3).empty());
}

TEST(DmnPipeContract, SynchronousReadPreservesFIFOAndCountsCompletedItems) {
  dmn::Dmn_Pipe<std::string> pipe{"synchronous"};
  std::string copied{"copy"};
  std::string moved{"move"};

  pipe.write(copied);
  pipe.write(std::move(moved));

  const auto first = pipe.read();
  const auto second = pipe.read();

  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, "copy");
  EXPECT_EQ(*second, "move");
  EXPECT_EQ(pipe.waitForEmpty(), 2);
}

TEST(DmnPipeContract, BulkReadReturnsAvailablePartialBatchAfterTimeout) {
  dmn::Dmn_Pipe<int> pipe{"bulk-timeout"};
  pipe.write(1);

  const auto partial = pipe.read(2, 1000);
  const auto empty = pipe.read(1, 1000);

  ASSERT_EQ(partial.size(), 1);
  EXPECT_EQ(partial.front(), 1);
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(pipe.waitForEmpty(), 1);
}

TEST(DmnPipeContract, BulkReadReturnsRequestedBatchWhenAlreadyAvailable) {
  dmn::Dmn_Pipe<int> pipe{"bulk-ready"};
  pipe.write(1);
  pipe.write(2);

  const auto batch = pipe.read(2, 1000);

  ASSERT_EQ(batch.size(), 2);
  EXPECT_EQ(batch[0], 1);
  EXPECT_EQ(batch[1], 2);
  EXPECT_EQ(pipe.waitForEmpty(), 2);
}

TEST(DmnPipeContract, ShutdownRejectsSubsequentReadAndWrite) {
  dmn::Dmn_Pipe<int> pipe{"closed"};
  dmn::Dmn_Io<int> &interface = pipe;
  interface.shutdown();

  EXPECT_THROW(static_cast<void>(pipe.read()), std::runtime_error);
  EXPECT_THROW(pipe.write(1), std::runtime_error);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
