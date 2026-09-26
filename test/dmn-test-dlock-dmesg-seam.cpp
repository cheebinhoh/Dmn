/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dlock-dmesg-seam.cpp
 * @brief TDD seam test for the distributed-lock DMesg handler factory path.
 */

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <string_view>

#include "dmn-dmesg.hpp"

namespace {

class TestCustomHandler : public dmn::Dmn_DMesg::Dmn_DMesgHandler {
public:
  TestCustomHandler(std::string_view name, std::string_view topic,
                    dmn::Dmn_DMesg::FilterTask filter_fn,
                    dmn::Dmn_DMesg::AsyncProcessTask async_process_fn,
                    dmn::Dmn_DMesg::HandlerConfig configs)
      : dmn::Dmn_DMesg::Dmn_DMesgHandler(name, topic, std::move(filter_fn),
                                         std::move(async_process_fn),
                                         std::move(configs)) {}

  void postToOwnContext() {
    this->scheduleInHandlerContext(
        [this]() -> void { this->m_ready.set_value(); });
  }

  void waitForReady() { this->m_ready.get_future().wait(); }

private:
  std::promise<void> m_ready{};
};

} // namespace

TEST(DlockDmesgSeamCustomHandlerPostsInOwnContext, PostToOwnContext) {
  dmn::Dmn_DMesg dmesg{"dmesg-custom-seam"};

  dmn::Dmn_DMesg::HandlerSpec spec{
      "custom-handler", "lock-topic", nullptr, nullptr, {}};

  auto handler = dmesg.openHandlerWithFactory(
      spec,
      [](const dmn::Dmn_DMesg::HandlerSpec &cfg)
          -> std::shared_ptr<dmn::Dmn_DMesg::Dmn_DMesgHandler> {
        return std::make_shared<TestCustomHandler>(
            cfg.m_name, cfg.m_topic, cfg.m_filter_fn, cfg.m_async_process_fn,
            cfg.m_configs);
      });

  EXPECT_TRUE(handler);

  auto custom =
      std::dynamic_pointer_cast<TestCustomHandler>(handler.operator->());
  EXPECT_TRUE(custom);

  ASSERT_TRUE(custom);
  custom->postToOwnContext();
  custom->waitForReady();

  dmesg.closeHandler(handler);
}

int main(int argc, char *argv[]) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  google::protobuf::ShutdownProtobufLibrary();
  return result;
}
