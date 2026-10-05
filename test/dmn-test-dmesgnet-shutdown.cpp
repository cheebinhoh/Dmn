/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesgnet-shutdown.cpp
 * @brief Verify input shutdown precedes Dmn_DMesgNet's final heartbeat write.
 */

#include "dmn-dmesgnet.hpp"
#include "dmn-io.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

struct ShutdownEvents {
  std::mutex mutex;
  std::condition_variable inputShutdown;
  std::vector<std::string> sequence;
  bool stopped{};
};

class BlockingInput final : public dmn::Dmn_Io<std::string> {
public:
  explicit BlockingInput(std::shared_ptr<ShutdownEvents> events)
      : m_events{std::move(events)} {}

  auto read() -> std::optional<std::string> override {
    std::unique_lock lock{m_events->mutex};
    m_events->inputShutdown.wait(lock, [this] { return m_events->stopped; });

    return std::nullopt;
  }

  void write(const std::string &) override {}
  void write(std::string &&) override {}

  void shutdown() override {
    {
      std::lock_guard lock{m_events->mutex};
      if (!m_events->stopped) {
        m_events->sequence.emplace_back("input-shutdown");
        m_events->stopped = true;
      }
    }

    m_events->inputShutdown.notify_all();
  }

private:
  std::shared_ptr<ShutdownEvents> m_events;
};

class RecordingOutput final : public dmn::Dmn_Io<std::string> {
public:
  explicit RecordingOutput(std::shared_ptr<ShutdownEvents> events)
      : m_events{std::move(events)} {}

  auto read() -> std::optional<std::string> override { return std::nullopt; }

  void write(const std::string &payload) override { record(payload); }
  void write(std::string &&payload) override { record(payload); }

private:
  void record(const std::string &payload) {
    dmn::DMesgPb message;
    if (message.ParseFromString(payload) &&
        message.type() == dmn::DMesgTypePb::sys &&
        message.body().sys().self().state() == dmn::DMesgStatePb::Destroyed) {
      std::lock_guard lock{m_events->mutex};
      m_events->sequence.emplace_back("destroyed-heartbeat");
    }
  }

  std::shared_ptr<ShutdownEvents> m_events;
};

} // namespace

TEST(DmnDmesgNetShutdownTest, StopsInputBeforeWritingDestroyedHeartbeat) {
  auto events{std::make_shared<ShutdownEvents>()};
  auto input{std::make_shared<BlockingInput>(events)};
  auto output{std::make_shared<RecordingOutput>(events)};

  { dmn::Dmn_DMesgNet node{"shutdown-order", input, output}; }

  std::lock_guard lock{events->mutex};
  ASSERT_EQ(events->sequence.size(), 2U);
  EXPECT_EQ(events->sequence[0], "input-shutdown");
  EXPECT_EQ(events->sequence[1], "destroyed-heartbeat");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
