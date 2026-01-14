/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesg.cpp
 * @brief DMESG publisher/subscriber wrapper using Protobuf messages.
 */

#include "dmn-dmesg.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <utility>
#include <vector>

#include "dmn-async.hpp"
#include "dmn-dmesg-pb-util.hpp"
#include "dmn-pub-sub.hpp"
#include "dmn-util.hpp"

#include "proto/dmn-dmesg-type.pb.h"
#include "proto/dmn-dmesg.pb.h"

namespace dmn {

const char *const kDMesgSysIdentifier = "sys.dmn-dmesg";

const Dmn_DMesg::HandlerConfig Dmn_DMesg::kHandlerConfig_Default = {};

// class Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub::
    ~Dmn_DMesgHandlerSub() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub::notify(
    const dmn::DMesgPb &dmesgpb) {
  if (dmesgpb.conflict()) {
    if (dmesgpb.sourcewritehandleridentifier() != m_owner->m_name &&
        (m_owner->m_no_topic_filter || m_owner->m_topic.empty() ||
         dmesgpb.topic() == m_owner->m_topic)) {
      m_owner->throwConflict(dmesgpb);
    }
  } else {
    if (dmesgpb.sourcewritehandleridentifier() != m_owner->m_name ||
        dmesgpb.type() == dmn::DMesgTypePb::sys) {
      const std::string &topic = dmesgpb.topic();
      const auto runningCounter = m_owner->m_topic_running_counter[topic];

      if (dmesgpb.runningcounter() > runningCounter || dmesgpb.force()) {
        m_owner->m_topic_running_counter[topic] = dmesgpb.runningcounter();

        if (dmesgpb.type() == dmn::DMesgTypePb::sys) {
          m_owner->m_last_dmesgpb_sys = dmesgpb;
        }

        if ((dmn::DMesgTypePb::sys != dmesgpb.type() ||
             m_owner->m_include_dmesgpb_sys) &&
            (m_owner->m_no_topic_filter || m_owner->m_topic.empty() ||
             dmesgpb.topic() == m_owner->m_topic) &&
            (!m_owner->m_filter_fn || m_owner->m_filter_fn(dmesgpb))) {
          if (m_owner->m_async_process_fn) {
            m_owner->m_async_process_fn(std::move_if_noexcept(dmesgpb));
          } else {
            m_owner->resolveConflictInternal();

            dmn::DMesgPb copied = dmesgpb;
            m_owner->m_buffers.push(copied);
          }
        }
      }
    }
  } /* if (dmesgpb.conflict()) */
}

// class Dmn_DMesg::Dmn_DMesgHandler
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn,
                                              HandlerConfig configs)
    : m_name{name}, m_topic{topic}, m_filter_fn{std::move(filter_fn)},
      m_async_process_fn{std::move(async_process_fn)},
      m_configs{std::move(configs)} {
  // set the chained of owner for composite Dmn_DMesgHandlerSub object
  auto iter = m_configs.find(std::string(kHandlerConfig_IncludeSys));
  if (m_configs.end() != iter) {
    m_include_dmesgpb_sys =
        stringCompare(iter->second, "1") || stringCompare(iter->second, "yes");
  }

  iter = m_configs.find(std::string(kHandlerConfig_NoTopicFilter));
  if (m_configs.end() != iter) {
    m_no_topic_filter =
        stringCompare(iter->second, "1") || stringCompare(iter->second, "yes");
  }

  m_sub.m_owner = this;
}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn)
    : Dmn_DMesgHandler{name, topic, std::move(filter_fn),
                       std::move(async_process_fn), kHandlerConfig_Default} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic,
                                              FilterTask filter_fn)
    : Dmn_DMesgHandler{name, topic, std::move(filter_fn),
                       static_cast<AsyncProcessTask>(nullptr)} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic)
    : Dmn_DMesgHandler{name, topic, static_cast<FilterTask>(nullptr)} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn,
                                              HandlerConfig configs)
    : Dmn_DMesgHandler{name, "", std::move(filter_fn),
                       std::move(async_process_fn), std::move(configs)} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn)
    : Dmn_DMesgHandler{name, std::move(filter_fn), std::move(async_process_fn),
                       kHandlerConfig_Default} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filter_fn)
    : Dmn_DMesgHandler{name, std::move(filter_fn),
                       static_cast<AsyncProcessTask>(nullptr)} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name)
    : Dmn_DMesgHandler{name, static_cast<FilterTask>(nullptr)} {}

Dmn_DMesg::Dmn_DMesgHandler::~Dmn_DMesgHandler() noexcept try {
  m_sub.waitForEmpty();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

auto Dmn_DMesg::Dmn_DMesgHandler::isInConflict() -> bool {
  bool inConflict{};

  this->isAfterInitialPlayback();

  auto waitHandler = m_sub.addExecTaskWithWait([this, &inConflict]() -> void {
    inConflict = this->isInConflictInternal();
  });

  waitHandler->wait();

  return inConflict;
}

void Dmn_DMesg::Dmn_DMesgHandler::isAfterInitialPlayback() {
  while (!m_after_initial_playback.test()) {
    m_after_initial_playback.wait(false, std::memory_order_relaxed);
  }
}

auto Dmn_DMesg::Dmn_DMesgHandler::getTopicRunningCounter(std::string_view topic)
    -> unsigned long {
  unsigned long runningCounter{};

  this->isAfterInitialPlayback();

  auto waitHandler =
      m_sub.addExecTaskWithWait([this, &runningCounter, topic]() -> void {
        runningCounter = this->getTopicRunningCounterInternal(topic);
      });

  waitHandler->wait();

  return runningCounter;
}

auto Dmn_DMesg::Dmn_DMesgHandler::getTopicRunningCounterInternal(
    std::string_view topic) -> unsigned long {
  auto iter = m_topic_running_counter.find(std::string(topic));
  if (m_topic_running_counter.end() == iter) {
    return 0;
  }

  return iter->second;
}

void Dmn_DMesg::Dmn_DMesgHandler::setAfterInitialPlayback() {
  [[maybe_unused]] auto waitHandler = m_sub.addExecTaskWithWait(
      [this]() -> void { this->setAfterInitialPlaybackInternal(); });
}

void Dmn_DMesg::Dmn_DMesgHandler::setAfterInitialPlaybackInternal() {
  m_after_initial_playback.test_and_set(std::memory_order_relaxed);
  m_after_initial_playback.notify_all();
}

void Dmn_DMesg::Dmn_DMesgHandler::setTopicRunningCounter(
    std::string_view topic, unsigned long runningCounter) {
  auto waitHandler =
      m_sub.addExecTaskWithWait([this, &runningCounter, topic]() -> void {
        this->setTopicRunningCounterInternal(topic, runningCounter);
      });

  waitHandler->wait();
}

void Dmn_DMesg::Dmn_DMesgHandler::setTopicRunningCounterInternal(
    std::string_view topic, unsigned long runningCounter) {
  m_topic_running_counter[std::string(topic)] = runningCounter;
}

auto Dmn_DMesg::Dmn_DMesgHandler::read() -> std::optional<dmn::DMesgPb> {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  dmn::DMesgPb mesgPb = m_buffers.pop();

  return mesgPb;
}

void Dmn_DMesg::Dmn_DMesgHandler::resolveConflict() {
  this->isAfterInitialPlayback();

  m_owner->resetHandlerConflictState(this);
}

void Dmn_DMesg::Dmn_DMesgHandler::setConflictCallbackTask(
    ConflictCallbackTask conflict_fn) {
  m_conflict_callback_fn = std::move(conflict_fn);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &&dmesgpb) {
  this->write(dmesgpb, false);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &dmesgpb) {
  this->write(dmesgpb, false);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &&dmesgpb, bool block) {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  dmn::DMesgPb moved_dmesgpb = std::move(dmesgpb);

  auto waithandler =
      m_sub.addExecTaskWithWait([this, &moved_dmesgpb, block]() -> void {
        writeDMesgInternal(moved_dmesgpb, true, block);
      });
  waithandler->wait();
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &dmesgpb, bool block) {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  auto waitHandler =
      m_sub.addExecTaskWithWait([this, &dmesgpb, block]() -> void {
        writeDMesgInternal(dmesgpb, false, block);
      });
  waitHandler->wait();
}

auto Dmn_DMesg::Dmn_DMesgHandler::writeAndCheckConflict(dmn::DMesgPb &&dmesgpb)
    -> bool {

  this->write(dmesgpb, true);

  return !this->isInConflict();
}

auto Dmn_DMesg::Dmn_DMesgHandler::writeAndCheckConflict(dmn::DMesgPb &dmesgpb)
    -> bool {

  this->write(dmesgpb, true);

  return !this->isInConflict();
}

void Dmn_DMesg::Dmn_DMesgHandler::waitForEmpty() {

  this->isAfterInitialPlayback();

  m_sub.waitForEmpty();
}

void Dmn_DMesg::Dmn_DMesgHandler::writeDMesgInternal(dmn::DMesgPb &dmesgpb,
                                                     bool move, bool block) {
  assert(nullptr != m_owner);

  if (m_in_conflict && !dmesgpb.force()) {
    throw std::runtime_error("last write results in conflicted, "
                             "handler needs to be reset");
  }

  struct timeval tval{};
  gettimeofday(&tval, nullptr);

  DMESG_PB_SET_MSG_TIMESTAMP_FROM_TV(dmesgpb, tval);
  DMESG_PB_SET_MSG_SOURCEWRITEHANDLERIDENTIFIER(dmesgpb, m_name);

  if (dmesgpb.topic().empty() && (!m_topic.empty())) {
    DMESG_PB_SET_MSG_TOPIC(dmesgpb, m_topic);
  }

  if (dmesgpb.sourceidentifier().empty()) {
    DMESG_PB_SET_MSG_SOURCEIDENTIFIER(dmesgpb, m_name);
  }

  const std::string topic = dmesgpb.topic();

  const auto next_running_counter =
      incrementByOne(m_topic_running_counter[topic]);

  DMESG_PB_SET_MSG_RUNNINGCOUNTER(dmesgpb, next_running_counter);

  m_topic_running_counter[topic] = next_running_counter;
  m_in_conflict = false;

  if (move) {
    m_owner->publish(std::move_if_noexcept(dmesgpb), block);
  } else {
    m_owner->publish(dmesgpb, block);
  }
}

auto Dmn_DMesg::Dmn_DMesgHandler::isInConflictInternal() const -> bool {
  return m_in_conflict;
}

void Dmn_DMesg::Dmn_DMesgHandler::resolveConflictInternal() {
  m_in_conflict = false;
}

void Dmn_DMesg::Dmn_DMesgHandler::throwConflict(const dmn::DMesgPb &dmesgpb) {
  m_in_conflict = true;

  if (m_conflict_callback_fn) {
    m_sub.write([this, dmesgpb]() -> void {
      this->m_conflict_callback_fn(*this, dmesgpb);
    });
  }
}

// class Dmn_DMesg
Dmn_DMesg::Dmn_DMesg(std::string_view name)
    : Dmn_Pub{name, 0, // Dmn_DMesg manages re-send per topic
              [](const Dmn_Sub *const sub, const dmn::DMesgPb &msg) -> bool {
                const auto *const handler_sub = dynamic_cast<
                    const Dmn_DMesgHandler::Dmn_DMesgHandlerSub *const>(sub);
                assert(handler_sub != nullptr);

                const Dmn_DMesgHandler *const handler = handler_sub->m_owner;

                return nullptr != handler && nullptr != handler->m_owner &&
                       (msg.playback() ||
                        handler->m_after_initial_playback.test()) &&
                       (handler->m_no_topic_filter ||
                        handler->m_topic.empty() ||
                        msg.topic() == handler->m_topic);
              }},
      m_name{name} {}

Dmn_DMesg::~Dmn_DMesg() noexcept try { this->waitForEmpty(); } catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_DMesg::closeHandler(
    std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> &handler) {
  this->unregisterSubscriber(&(handler->m_sub));
  handler->waitForEmpty();
  handler->m_owner = nullptr;

  const Dmn_DMesgHandler *const handler_ptr = handler.get();
  handler = {};

  DMN_ASYNC_CALL_WITH_CAPTURE(
      {
        auto iter =
            std::ranges::find_if(m_handlers.begin(), m_handlers.end(),
                                 [handler_ptr](const auto &handler) -> bool {
                                   return handler.get() == handler_ptr;
                                 });

        if (iter != m_handlers.end()) {
          m_handlers.erase(iter);
        }
      },
      this, handler_ptr);
}

void Dmn_DMesg::playbackLastTopicDMesgPbInternal() {
  for (auto &topic_dmesgpb : m_topic_last_dmesgpb) {
    dmn::DMesgPb msgpb = topic_dmesgpb.second;

    DMESG_PB_SET_MSG_PLAYBACK(msgpb, true);

    this->publishInternal(msgpb);
  }
}

void Dmn_DMesg::publishInternal(const dmn::DMesgPb &dmesgpb) {
  // if it is a playback, we do not check if it is in conflict.
  if (dmesgpb.playback()) {
    Dmn_Pub::publishInternal(dmesgpb);

    return;
  }

  auto iter = std::ranges::find_if(
      m_handlers.begin(), m_handlers.end(),
      [&dmesgpb](const auto &handler) -> bool {
        return handler->m_name == dmesgpb.sourcewritehandleridentifier();
      });

  // if source is still in conflict, we do not allow it to send any message
  // until it resolves conflict state.
  if (iter != m_handlers.end() && (*iter)->isInConflictInternal()) {
    // avoid throw conflict multiple times
    return;
  }

  dmn::DMesgPb copied_dmesgpb = dmesgpb;
  const std::string &topic = copied_dmesgpb.topic();

  auto next_running_counter = incrementByOne(m_topic_running_counter[topic]);
  if (copied_dmesgpb.force()) {
    next_running_counter = copied_dmesgpb.runningcounter();
  }

  // if this is a message is out of date and put the sender in conflict
  if (copied_dmesgpb.runningcounter() < next_running_counter) {
    copied_dmesgpb.set_conflict(true);

    if (iter != m_handlers.end()) {
      (*iter)->throwConflict(copied_dmesgpb);
    }
  } else {
    DMESG_PB_SET_MSG_RUNNINGCOUNTER(copied_dmesgpb, next_running_counter);
    m_topic_running_counter[topic] = next_running_counter;
    m_topic_last_dmesgpb[topic] = copied_dmesgpb;
  }

  Dmn_Pub::publishInternal(copied_dmesgpb);
}

void Dmn_DMesg::publishSysInternal(const dmn::DMesgPb &dmesgpb_sys) {
  assert(dmesgpb_sys.topic() == kDMesgSysIdentifier);
  assert(dmesgpb_sys.type() == dmn::DMesgTypePb::sys);

  const std::string &topic = dmesgpb_sys.topic();
  const unsigned long next_running_counter =
      incrementByOne(m_topic_running_counter[topic]);

  dmn::DMesgPb copied = dmesgpb_sys;

  DMESG_PB_SET_MSG_RUNNINGCOUNTER(copied, next_running_counter);
  Dmn_Pub::publishInternal(copied);
  m_topic_running_counter[topic] = next_running_counter;
}

void Dmn_DMesg::resetHandlerConflictState(const Dmn_DMesgHandler *handler_ptr) {
  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->resetHandlerConflictStateInternal(handler_ptr); }, this,
      handler_ptr);
}

void Dmn_DMesg::resetHandlerConflictStateInternal(
    const Dmn_DMesgHandler *handler_ptr) {
  auto iter = std::ranges::find_if(m_handlers.begin(), m_handlers.end(),
                                   [handler_ptr](const auto &handler) -> bool {
                                     return handler.get() == handler_ptr;
                                   });

  if (iter != m_handlers.end()) {
    (*iter)->resolveConflictInternal();
  }
}

} // namespace dmn
