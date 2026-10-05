/**
 * Copyright © 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-test-dmesgnet-shutdown.cpp
 * @brief Verify input shutdown precedes Dmn_DMesgNet's final heartbeat write.
 */

#include "dmn-dmesgnet.hpp"
#include "dmn-io.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ShutdownEvents {
  std::mutex m_mutex;
  std::condition_variable m_input_shutdown;
  std::condition_variable m_read_started;
  std::condition_variable m_read_exited;
  std::vector<std::string> m_sequence;
  bool m_stopped{};
  bool m_read_entered{};
  bool m_reader_exited{};
  bool m_shutdown_woke_reader{};
};

class BlockingInput final : public dmn::Dmn_Io<std::string> {
public:
  explicit BlockingInput(std::shared_ptr<ShutdownEvents> events)
      : m_events{std::move(events)} {}

  auto read() -> std::optional<std::string> override {
    std::unique_lock lock{m_events->m_mutex};
    m_events->m_read_entered = true;
    m_events->m_read_started.notify_all();
    m_events->m_input_shutdown.wait(lock,
                                    [this] { return m_events->m_stopped; });
    m_events->m_reader_exited = true;
    m_events->m_sequence.emplace_back("input-read-exit");
    m_events->m_read_exited.notify_all();

    return std::nullopt;
  }

  void write(const std::string &) override {}
  void write(std::string &&) override {}

  void shutdown() override {
    {
      std::lock_guard lock{m_events->m_mutex};
      if (!m_events->m_stopped) {
        m_events->m_sequence.emplace_back("input-shutdown");
        m_events->m_stopped = true;
      }
    }

    m_events->m_input_shutdown.notify_all();
    std::unique_lock lock{m_events->m_mutex};
    m_events->m_shutdown_woke_reader =
        m_events->m_read_exited.wait_for(lock, std::chrono::seconds(2), [this] {
          return m_events->m_reader_exited;
        });
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
      std::lock_guard lock{m_events->m_mutex};
      m_events->m_sequence.emplace_back("destroyed-heartbeat");
    }
  }

  std::shared_ptr<ShutdownEvents> m_events;
};

} // namespace

TEST(DmnDmesgNetShutdownTest, StopsInputBeforeWritingDestroyedHeartbeat) {
  auto events{std::make_shared<ShutdownEvents>()};
  auto input{std::make_shared<BlockingInput>(events)};
  auto output{std::make_shared<RecordingOutput>(events)};
  auto node{
      std::make_unique<dmn::Dmn_DMesgNet>("shutdown-order", input, output)};

  std::unique_lock lock{events->m_mutex};
  const bool readBlocked{
      events->m_read_started.wait_for(lock, std::chrono::seconds(2), [&events] {
        return events->m_read_entered;
      })};
  lock.unlock();
  ASSERT_TRUE(readBlocked) << "Dmn_DMesgNet input task did not enter read()";

  node.reset();

  lock.lock();
  EXPECT_TRUE(events->m_reader_exited);
  EXPECT_TRUE(events->m_shutdown_woke_reader);
  ASSERT_EQ(events->m_sequence.size(), 3U);
  EXPECT_EQ(events->m_sequence[0], "input-shutdown");
  EXPECT_EQ(events->m_sequence[1], "input-read-exit");
  EXPECT_EQ(events->m_sequence[2], "destroyed-heartbeat");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
