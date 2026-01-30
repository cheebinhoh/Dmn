/**
 * Copyright © 2024 - 2025 Chee Bin HOH. All rights reserved.
 *
 * This test program asserts that the Dmn_Pub and Dmn_Pub::Sub model.
 */

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "dmn-pub-sub.hpp"

using namespace std::string_view_literals;

class Dmn_Msg_Receiver : public dmn::Dmn_Pub<std::string>::Dmn_Sub {
public:
  Dmn_Msg_Receiver(std::string_view name, ssize_t replay = -1)
      : dmn::Dmn_Pub<std::string>::Dmn_Sub{replay}, m_name{name} {}

  ~Dmn_Msg_Receiver() {}

  void notify(const std::string &item) override {
    std::cout << m_name << " is notified: " << item << "\n";

    m_notifiedList.push_back(item);
  }

  std::vector<std::string> m_notifiedList{};
  std::string m_name{};
};

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);

  dmn::Dmn_Pub<std::string> pub{"radio", 3};

  // auto rec1 = pub.registerSubscriber<Dmn_Msg_Receiver>(new
  // Dmn_Msg_Receiver{"receiver 1"});
  auto rec1 = pub.registerSubscriber<Dmn_Msg_Receiver>("receiver 1"sv);
  pub.publish("hello pub sub");
  std::this_thread::sleep_for(std::chrono::seconds(3));

  EXPECT_TRUE(rec1->m_notifiedList.size() == 1);
  EXPECT_TRUE(rec1->m_notifiedList[0] == "hello pub sub");

  pub.publish("hello world");
  pub.publish("hello world 3");
  std::string str1{"string 1"};
  pub.publish(str1);

  std::this_thread::sleep_for(std::chrono::seconds(3));

  EXPECT_TRUE(rec1->m_notifiedList.size() == 4);
  EXPECT_TRUE(rec1->m_notifiedList[1] == "hello world");
  EXPECT_TRUE(rec1->m_notifiedList[2] == "hello world 3");
  EXPECT_TRUE(rec1->m_notifiedList[3] == "string 1");

  std::this_thread::sleep_for(std::chrono::seconds(5));
  pub.unregisterSubscriber(rec1.get());

  auto rec2 = pub.registerSubscriber<Dmn_Msg_Receiver>("receiver 2"sv, 2);
  std::this_thread::sleep_for(std::chrono::seconds(5));
  pub.unregisterSubscriber(rec2.get());

  EXPECT_TRUE(rec2->m_notifiedList.size() == 2);
  EXPECT_TRUE(rec2->m_notifiedList[0] == "hello world 3");
  EXPECT_TRUE(rec2->m_notifiedList[1] == "string 1");

  auto rec3 = pub.registerSubscriber<Dmn_Msg_Receiver>("receiver 3"sv);
  std::this_thread::sleep_for(std::chrono::seconds(5));
  pub.unregisterSubscriber(rec3.get());

  EXPECT_TRUE(rec3->m_notifiedList.size() == 3);
  EXPECT_TRUE(rec3->m_notifiedList[0] == "hello world");
  EXPECT_TRUE(rec3->m_notifiedList[1] == "hello world 3");
  EXPECT_TRUE(rec3->m_notifiedList[2] == "string 1");

  std::this_thread::sleep_for(std::chrono::seconds(3));

  return RUN_ALL_TESTS();
}
