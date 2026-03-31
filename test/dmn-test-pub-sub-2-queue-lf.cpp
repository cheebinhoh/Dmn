/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-pub-sub.cpp
 * @brief Unit test for the Dmn_Pub/Dmn_Sub publish-subscribe model.
 *
 * This test program asserts that the Dmn_Pub and Dmn_Pub::Sub model.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "dmn-blockingqueue-lf.hpp"
#include "dmn-pub-sub.hpp"

using namespace std::string_view_literals;

void *g_pub = nullptr;
void *g_pub2 = nullptr;

class Dmn_Msg_Receiver
    : public dmn::Dmn_Pub<std::string, dmn::Dmn_BlockingQueue_Lf>::Dmn_Sub {
public:
  Dmn_Msg_Receiver(std::string_view name, ssize_t replay = -1)
      : dmn::Dmn_Pub<std::string, dmn::Dmn_BlockingQueue_Lf>::Dmn_Sub{replay},
        m_name{name} {}

  ~Dmn_Msg_Receiver() {}

  void
  notify(const std::string &item,
         dmn::Dmn_Pub<std::string, dmn::Dmn_BlockingQueue_Lf> *pub) override {
    std::cout << m_name << " is notified: " << item << "\n";

    m_notifiedList.push_back(item);
    EXPECT_TRUE(pub == g_pub || pub == g_pub2);
  }

  std::vector<std::string> m_notifiedList{};
  std::string m_name{};
};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_Pub<std::string, dmn::Dmn_BlockingQueue_Lf> pub{"radio", 3};
  g_pub = &pub;

  dmn::Dmn_Pub<std::string, dmn::Dmn_BlockingQueue_Lf> pub2{"radio", 3};
  g_pub2 = &pub2;

  // auto rec1 = pub.registerSubscriber<Dmn_Msg_Receiver>(new
  // Dmn_Msg_Receiver{"receiver 1"});
  auto rec1 = pub.registerSubscriber<Dmn_Msg_Receiver>("receiver 1"sv);
  pub2.registerSubscriber(rec1);

  pub.publish("hello pub sub");
  std::this_thread::sleep_for(std::chrono::seconds(3));

  EXPECT_TRUE(rec1->m_notifiedList.size() == 1);
  EXPECT_TRUE(rec1->m_notifiedList[0] == "hello pub sub");

  pub2.publish("hello pub2 sub");
  std::this_thread::sleep_for(std::chrono::seconds(3));

  EXPECT_TRUE(rec1->m_notifiedList.size() == 2);
  EXPECT_TRUE(rec1->m_notifiedList[0] == "hello pub sub");
  EXPECT_TRUE(rec1->m_notifiedList[1] == "hello pub2 sub");

  pub.publish("hello pub sub again");
  std::this_thread::sleep_for(std::chrono::seconds(3));

  EXPECT_TRUE(rec1->m_notifiedList.size() == 3);
  EXPECT_TRUE(rec1->m_notifiedList[2] == "hello pub sub again");

  pub.unregisterSubscriber(rec1.get());
  pub2.publish("hello pub2 sub again");

  std::this_thread::sleep_for(std::chrono::seconds(3));
  EXPECT_TRUE(rec1->m_notifiedList.size() == 4);
  EXPECT_TRUE(rec1->m_notifiedList[3] == "hello pub2 sub again");

  return RUN_ALL_TESTS();
}
