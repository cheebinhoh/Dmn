/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-proc.cpp
 * @brief Unit tests for Dmn_Proc exception reporting and cancellation.
 */

#include <gtest/gtest.h>

#include <stdexcept>

#include "dmn-proc.hpp"

namespace {

struct NonStandardFailure {};

class TestDmnProc : public dmn::Dmn_Proc {
public:
  using Dmn_Proc::Dmn_Proc;
  using Dmn_Proc::stopExec;
};

void runDefaultPolicyFailure() {
  dmn::Dmn_Proc proc{"legacy-terminate",
                     [] { throw std::runtime_error("legacy task failure"); }};
  proc.exec();
  proc.wait();
}

} // namespace

TEST(DmnProc, CapturesStandardTaskExceptionForJoiner) {
  dmn::Dmn_Proc proc{
      "throw-standard", [] { throw std::runtime_error("task failure"); },
      dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());
  EXPECT_THROW(proc.wait(), std::runtime_error);

  ASSERT_TRUE(proc.exec([] {}));
  EXPECT_TRUE(proc.wait());
}

TEST(DmnProc, CapturesNonStandardTaskExceptionForJoiner) {
  dmn::Dmn_Proc proc{
      "throw-non-standard", [] { throw NonStandardFailure{}; },
      dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());
  EXPECT_THROW(proc.wait(), NonStandardFailure);
}

TEST(DmnProc, StopExecPreservesDeferredCancellation) {
  TestDmnProc proc{"cancel",
                   [] {
                     while (true) {
                       dmn::Dmn_Proc::yield();
                     }
                   },
                   dmn::Dmn_Proc::ExceptionPolicy::kCaptureAndRethrowFromWait};

  ASSERT_TRUE(proc.exec());
  EXPECT_TRUE(proc.stopExec());
}

TEST(DmnProc, DefaultPolicyPreservesTerminateBehavior) {
  EXPECT_DEATH(runDefaultPolicyFailure(), "");
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
