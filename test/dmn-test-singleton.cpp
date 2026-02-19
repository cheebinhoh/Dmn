/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 */

#include <gtest/gtest.h>

#include <iostream>
#include <thread>

#include "dmn-singleton.hpp"

class Dmn_A : public dmn::Dmn_Singleton<Dmn_A> {
public:
  Dmn_A(int int1, int int2) : m_int1{int1}, m_int2{int2} {}

  int getValue() { return m_int1 + m_int2; }

  static int s_priorCreateInstance;
  static void runPriorToCreateInstance();

private:
  static std::shared_ptr<Dmn_A> s_instances;
  static std::once_flag s_init_once;

  int m_int1{};
  int m_int2{};
};

void Dmn_A::runPriorToCreateInstance() {
  std::cout << "run priorToCreateInstance()\n";

  s_priorCreateInstance++;
}

int Dmn_A::s_priorCreateInstance{};
std::once_flag Dmn_A::s_init_once{};
std::shared_ptr<Dmn_A> Dmn_A::s_instances{};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  auto inst1 = Dmn_A::createInstance(1, 2);
  std::cout << "Value: " << inst1->getValue() << ", :" << inst1 << "\n";

  EXPECT_TRUE(1 == Dmn_A::s_priorCreateInstance);

  auto inst2 = Dmn_A::createInstance(1, 2);
  std::cout << "Value: " << inst2->getValue() << ", :" << inst2 << "\n";

  EXPECT_TRUE(1 == Dmn_A::s_priorCreateInstance);
  EXPECT_TRUE(inst1 == inst2);

  return RUN_ALL_TESTS();
}
