/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-state.cpp
 * @brief Unit tests for Dmn_State lifecycle and user-state transitions.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <string>

#include "dmn-state.hpp"

TEST(DmnState, EmptyMachineFinalizesAndRemainsStopped) {
  dmn::Dmn_State state{"empty"};

  EXPECT_FALSE(state.runNext());
  EXPECT_TRUE(state.isInitialized());
  EXPECT_TRUE(state.isFinalized());
  EXPECT_FALSE(state.runNext());
}

TEST(DmnState, EndBeforeStartFinalizesWithoutInitialization) {
  dmn::Dmn_State state{"ended-before-start"};
  state.setEnd();

  EXPECT_FALSE(state.runNext());
  EXPECT_FALSE(state.isInitialized());
  EXPECT_TRUE(state.isFinalized());
  EXPECT_FALSE(state.runNext());
}

TEST(DmnState, ExplicitTransitionsCanJumpBackward) {
  dmn::Dmn_State state{"backward-jump"};
  int first_count{};
  int second_count{};
  int third_count{};

  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++first_count;
    current.setNext(3);
  });
  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++second_count;
    current.setEnd();
  });
  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++third_count;
    current.setNext(2);
  });

  EXPECT_TRUE(state.runNext());
  EXPECT_TRUE(state.runNext());
  EXPECT_FALSE(state.runNext());
  EXPECT_EQ(first_count, 1);
  EXPECT_EQ(second_count, 1);
  EXPECT_EQ(third_count, 1);
}

TEST(DmnState, ReplacingCallbackUsesReplacement) {
  dmn::Dmn_State state{"replacement"};
  int original_count{};
  int replacement_count{};

  state.setStateFnc([&](dmn::Dmn_State &) { ++original_count; });
  state.setStateFnc(
      [&](dmn::Dmn_State &current) {
        ++replacement_count;
        current.setEnd();
      },
      1);

  EXPECT_FALSE(state.runNext());
  EXPECT_EQ(original_count, 0);
  EXPECT_EQ(replacement_count, 1);
}

TEST(DmnState, RejectsEmptyCallbackWithoutReplacingExistingState) {
  dmn::Dmn_State state{"empty-callback"};
  int callback_count{};
  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++callback_count;
    current.setEnd();
  });

  EXPECT_THROW(state.setStateFnc({}, 1), std::invalid_argument);
  EXPECT_FALSE(state.runNext());
  EXPECT_EQ(callback_count, 1);
}

TEST(DmnState, CallbackExceptionPropagatesWithoutAdvancingState) {
  dmn::Dmn_State state{"callback-exception"};
  int callback_count{};
  state.setStateFnc([&](dmn::Dmn_State &current) {
    if (++callback_count == 1) {
      throw std::runtime_error{"expected callback failure"};
    }

    current.setEnd();
  });

  EXPECT_THROW(state.runNext(), std::runtime_error);
  EXPECT_TRUE(state.isInitialized());
  EXPECT_FALSE(state.isFinalized());
  EXPECT_FALSE(state.runNext());
  EXPECT_EQ(callback_count, 2);
}

TEST(DmnState, CallbackExceptionPreservesSelectedTransition) {
  dmn::Dmn_State state{"callback-exception-after-transition"};
  int first_count{};
  int second_count{};
  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++first_count;
    current.setNext(2);

    throw std::runtime_error{"expected callback failure"};
  });
  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++second_count;
    current.setEnd();
  });

  EXPECT_THROW(state.runNext(), std::runtime_error);
  EXPECT_FALSE(state.runNext());
  EXPECT_EQ(first_count, 1);
  EXPECT_EQ(second_count, 1);
}

TEST(DmnState, InitializationGuardFailureCanBeRetried) {
  class ThrowOnceOnTransition final : public dmn::Dmn_State {
  public:
    using Dmn_State::Dmn_State;

  protected:
    void beforeSetNext() override {
      if (m_shouldThrow) {
        m_shouldThrow = false;

        throw std::runtime_error{"expected initialization failure"};
      }
    }

  private:
    bool m_shouldThrow{true};
  };

  ThrowOnceOnTransition state{"initialization-exception"};
  int callback_count{};
  state.setStateFnc([&](dmn::Dmn_State &current) {
    ++callback_count;
    current.setEnd();
  });

  EXPECT_THROW(state.runNext(), std::runtime_error);
  EXPECT_FALSE(state.isInitialized());
  EXPECT_EQ(callback_count, 0);
  EXPECT_FALSE(state.runNext());
  EXPECT_TRUE(state.isInitialized());
  EXPECT_TRUE(state.isFinalized());
  EXPECT_EQ(callback_count, 1);
}

TEST(DmnState, RejectsCallbackRegistrationDuringCallback) {
  dmn::Dmn_State state{"callback-registration"};
  bool rejected{};
  state.setStateFnc([&](dmn::Dmn_State &current) {
    try {
      current.setStateFnc([](dmn::Dmn_State &) {});
    } catch (const std::logic_error &) {
      rejected = true;
    }

    current.setEnd();
  });

  EXPECT_FALSE(state.runNext());
  EXPECT_TRUE(rejected);
}

TEST(DmnState, RejectsRecursiveExecutionFromCallback) {
  dmn::Dmn_State state{"recursive-execution"};
  bool rejected{};
  state.setStateFnc([&](dmn::Dmn_State &current) {
    try {
      current.runNext();
    } catch (const std::logic_error &) {
      rejected = true;
    }

    current.setEnd();
  });

  EXPECT_FALSE(state.runNext());
  EXPECT_TRUE(rejected);
}

static std::mutex log_mutex{};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_State s1{"default"};

  EXPECT_FALSE(s1);
  EXPECT_TRUE(!s1);
  EXPECT_TRUE(!s1.isInitialized());
  EXPECT_TRUE(!s1.isFinalized());
  EXPECT_FALSE(s1.hasStateFncs());

  EXPECT_FALSE(s1.runNext());
  EXPECT_FALSE(s1);
  EXPECT_TRUE(s1.isInitialized());
  EXPECT_TRUE(s1.isFinalized());

  dmn::Dmn_State s2{"default2"};
  int s2_count = 0;
  s2.setStateFnc([&s2_count](dmn::Dmn_State &s) {
    ++s2_count;
    s.setEnd();
  });

  EXPECT_TRUE(s2);
  EXPECT_TRUE(!s2.isInitialized());
  EXPECT_TRUE(!s2.isFinalized());

  EXPECT_FALSE(s2.runNext());
  EXPECT_TRUE(!s2);
  EXPECT_TRUE(s2.isInitialized());
  EXPECT_TRUE(s2.isFinalized());
  EXPECT_EQ(s2_count, 1);
  EXPECT_THROW(s2.setNext(0), std::out_of_range);
  EXPECT_THROW(s2.setNext(-1), std::out_of_range);
  EXPECT_THROW(s2.setNext(3), std::out_of_range);

  int s3_count = 0;
  dmn::Dmn_State s3{"count up to 3"};
  EXPECT_TRUE(!s3.isInitialized());
  EXPECT_TRUE(!s3.isFinalized());

  s3.setStateFnc([&s3_count](dmn::Dmn_State &s) {
    s3_count++;

    if (3 <= s3_count) {
      s.setEnd();
    }
  });

  EXPECT_TRUE(s3.hasStateFncs());

  EXPECT_TRUE(s3.runNext());
  EXPECT_EQ(s3_count, 1);
  EXPECT_TRUE(s3.isInitialized());
  EXPECT_FALSE(s3.isFinalized());

  while (s3.runNext()) {
  }

  EXPECT_TRUE(!s3);
  EXPECT_TRUE(s3.isInitialized());
  EXPECT_TRUE(s3.isFinalized());
  EXPECT_TRUE(3 == s3_count);

  int s4_state_count = 0;
  int s4_count = 0;
  dmn::Dmn_State s4{"two states: step to 4 (by 1), step to 10 (by 2)"};
  EXPECT_TRUE(!s4.isInitialized());
  EXPECT_TRUE(!s4.isFinalized());

  s4.setStateFnc([&s4_count, &s4_state_count](dmn::Dmn_State &s) {
    s4_count++;
    s4_state_count++;

    if (4 <= s4_count) {
      s.setNext();
    }
  });

  s4.setStateFnc([&s4_count, &s4_state_count](dmn::Dmn_State &s) {
    s4_count += 2;
    s4_state_count++;

    if (10 <= s4_count) {
      s.setNext();
    }
  });

  while (s4) {
    s4.runNext();
  }

  EXPECT_TRUE(!s4);
  EXPECT_TRUE(s4.isInitialized());
  EXPECT_TRUE(s4.isFinalized());
  EXPECT_TRUE(10 == s4_count);
  EXPECT_TRUE(7 == s4_state_count);

  return RUN_ALL_TESTS();
}
