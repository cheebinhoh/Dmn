/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesg.cpp
 * @brief Implementation of Dmn_DMesg and its nested Dmn_DMesgHandler.
 *
 * This file provides the concrete implementation of the DMesg
 * publisher/subscriber system, which exchanges messages using the
 * generated Protobuf type dmn::DMesgPb.
 *
 * Key implementation areas:
 * - Dmn_DMesgHandlerSub::notify(): the hot-path subscriber callback
 *   that filters, routes, and buffers or async-delivers arriving
 *   DMesgPb messages.
 * - Dmn_DMesgHandler: manages per-topic running counters, conflict
 *   detection, conflict callbacks, and the writeDMesgInternal() path
 *   that stamps and publishes outbound messages.
 * - Dmn_DMesg: the publisher side, including publishInternal() which
 *   applies global conflict detection and advances per-topic running
 *   counters, and openHandler()/closeHandler() lifecycle management.
 * - Playback: newly registered handlers receive the last published
 *   message for each topic via playbackLastTopicDMesgPbInternal().
 */

#include "dmn-dmesg.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
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

/**
 * @brief Route an incoming DMesgPb message to its owning Dmn_DMesgHandler.
 *
 * This is the hot-path subscriber callback invoked by Dmn_Pub::publishInternal()
 * for every message published to the Dmn_DMesg instance.  The method applies
 * the following routing logic:
 *
 *  1. Conflict messages: if the message carries conflict=true, the source
 *     is not this handler, the topic matches the handler's filter, and the
 *     topic already exists in m_topic_running_counter (i.e. this handler has
 *     seen the topic before), the handler enters conflict state via
 *     throwConflictInternal().
 *
 *  2. Normal messages: the running counter is compared against the handler's
 *     last seen counter for the topic. A message is considered accepted when:
 *       - Its running counter is higher (i.e. it is newer), OR
 *       - force=true is set on the message.
 *
 *     For force=true messages, acceptance only updates
 *     m_topic_running_counter and resolves any prior conflict on that topic
 *     via resolveConflictInternal(). These messages are not dispatched to
 *     m_async_process_fn and are not pushed to m_buffers.
 *
 *     For non-force accepted messages, after applying the source/topic/filter
 *     checks, the handler:
 *       - Updates m_topic_running_counter,
 *       - Resolves any prior conflict on that topic via resolveConflictInternal(),
 *       - Dispatches the message either via m_async_process_fn (if set) or by
 *         pushing onto m_buffers (for blocking read() callers).
 *
 *     Accepted sys-type messages always update m_last_dmesgpb_sys, regardless
 *     of whether they are dispatched or only used to update state.
 *
 * @param dmesgpb The incoming protobuf message published by Dmn_DMesg.
 */
void Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub::notify(
    const dmn::DMesgPb &dmesgpb) {
  const std::string &topic = dmesgpb.topic();
  auto iter = m_owner->m_topic_running_counter.find(topic);

  if (dmesgpb.conflict()) {
    if (dmesgpb.sourcewritehandleridentifier() != m_owner->m_name &&
        (m_owner->m_no_topic_filter || m_owner->m_topic.empty() ||
         dmesgpb.topic() == m_owner->m_topic)) {
      if (m_owner->m_topic_running_counter.end() != iter) {
        m_owner->throwConflictInternal(dmesgpb);
      }
    }
  } else {
    const auto runningCounter =
        (m_owner->m_topic_running_counter.end() != iter ? iter->second : 0);

    if (dmesgpb.runningcounter() > runningCounter || dmesgpb.force()) {
      if (dmesgpb.type() == dmn::DMesgTypePb::sys) {
        m_owner->m_last_dmesgpb_sys = dmesgpb;
      }

      if (dmesgpb.force()) {
        m_owner->m_topic_running_counter[topic] = dmesgpb.runningcounter();
        m_owner->resolveConflictInternal(dmesgpb.topic());
      } else if (dmesgpb.sourcewritehandleridentifier() != m_owner->m_name ||
                 dmesgpb.type() == dmn::DMesgTypePb::sys) {
        if ((dmn::DMesgTypePb::sys != dmesgpb.type() ||
             m_owner->m_include_dmesgpb_sys) &&
            (m_owner->m_no_topic_filter || m_owner->m_topic.empty() ||
             dmesgpb.topic() == m_owner->m_topic) &&
            (!m_owner->m_filter_fn || m_owner->m_filter_fn(dmesgpb))) {
          m_owner->m_topic_running_counter[topic] = dmesgpb.runningcounter();
          m_owner->resolveConflictInternal(dmesgpb.topic());

          if (m_owner->m_async_process_fn) {
            m_owner->m_async_process_fn(dmesgpb);
          } else {
            m_owner->m_buffers->push(dmesgpb);
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
  m_buffers = std::make_unique<Dmn_BlockingQueue<dmn::DMesgPb>>();

  // set the chained of owner for composite Dmn_DMesgHandlerSub object
  auto iter = m_configs.find(std::string{kHandlerConfig_IncludeSys});
  if (m_configs.end() != iter) {
    m_include_dmesgpb_sys =
        stringCompare(iter->second, "1") || stringCompare(iter->second, "yes");
  }

  iter = m_configs.find(std::string{kHandlerConfig_NoTopicFilter});
  if (m_configs.end() != iter) {
    m_no_topic_filter =
        stringCompare(iter->second, "1") || stringCompare(iter->second, "yes");
  }
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
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

auto Dmn_DMesg::Dmn_DMesgHandler::isInConflict(std::string_view topic) -> bool {
  bool inConflict{};

  this->isAfterInitialPlayback();

  auto waitHandler =
      m_sub->addExecTaskWithWait([this, topic, &inConflict]() -> void {
        inConflict = this->isInConflictInternal(topic);
      });

  waitHandler->wait();

  return inConflict;
}

/**
 * @brief Block the calling thread until the initial playback has been delivered.
 *
 * New handlers receive the last published message for each known topic as a
 * "playback" burst when they are first opened.  This method spin-waits (using
 * atomic::wait) until that initial burst has been marked complete by
 * setAfterInitialPlaybackInternal(), ensuring all public API calls that depend
 * on a consistent initial state observe messages in the correct order.
 */
void Dmn_DMesg::Dmn_DMesgHandler::isAfterInitialPlayback() {
  while (!m_after_initial_playback.test()) {
    m_after_initial_playback.wait(false, std::memory_order_relaxed);
  }
}

auto Dmn_DMesg::Dmn_DMesgHandler::getTopicRunningCounter(std::string_view topic)
    -> uint64_t {
  uint64_t runningCounter{};

  this->isAfterInitialPlayback();

  auto waitHandler =
      m_sub->addExecTaskWithWait([this, &runningCounter, topic]() -> void {
        runningCounter = this->getTopicRunningCounterInternal(topic);
      });

  waitHandler->wait();

  return runningCounter;
}

/**
 * @brief Return the handler's last-seen running counter for @p topic.
 *
 * Must be called from within the handler's Dmn_Async serialisation context
 * (i.e. from a task posted to m_sub). Callers outside that context should use
 * getTopicRunningCounter() which wraps this with an addExecTaskWithWait().
 *
 * @param topic The topic whose running counter is requested.
 * @return The stored running counter, or 0 if the topic has never been seen.
 */
auto Dmn_DMesg::Dmn_DMesgHandler::getTopicRunningCounterInternal(
    std::string_view topic) -> uint64_t {
  auto iter = m_topic_running_counter.find(std::string{topic});
  if (m_topic_running_counter.end() == iter) {
    return 0;
  }

  return iter->second;
}

/**
 * @brief Signal that initial playback has completed for this handler.
 *
 * Posts setAfterInitialPlaybackInternal() into the handler's serialised
 * Dmn_Async context so that the flag is set after all in-flight playback
 * messages have been processed.  Callers blocked in isAfterInitialPlayback()
 * are then woken via atomic::notify_all().
 */
void Dmn_DMesg::Dmn_DMesgHandler::setAfterInitialPlayback() {
  [[maybe_unused]] auto waitHandler = m_sub->addExecTaskWithWait(
      [this]() -> void { this->setAfterInitialPlaybackInternal(); });
}

/**
 * @brief Internal: atomically set the initial-playback flag and wake waiters.
 *
 * Must be called from within the handler's Dmn_Async serialisation context.
 */
void Dmn_DMesg::Dmn_DMesgHandler::setAfterInitialPlaybackInternal() {
  m_after_initial_playback.test_and_set(std::memory_order_relaxed);
  m_after_initial_playback.notify_all();
}

void Dmn_DMesg::Dmn_DMesgHandler::setTopicRunningCounter(
    std::string_view topic, uint64_t runningCounter) {
  auto waitHandler =
      m_sub->addExecTaskWithWait([this, &runningCounter, topic]() -> void {
        this->setTopicRunningCounterInternal(topic, runningCounter);
      });

  waitHandler->wait();
}

/**
 * @brief Internal: overwrite the stored running counter for @p topic.
 *
 * Must be called from within the handler's Dmn_Async serialisation context.
 *
 * @param topic          The topic whose counter is to be updated.
 * @param runningCounter The new counter value to store.
 */
void Dmn_DMesg::Dmn_DMesgHandler::setTopicRunningCounterInternal(
    std::string_view topic, uint64_t runningCounter) {
  m_topic_running_counter[std::string{topic}] = runningCounter;
}

auto Dmn_DMesg::Dmn_DMesgHandler::read() -> std::optional<dmn::DMesgPb> {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  try {
    return m_buffers->pop();
  } catch (...) {
    // Translate queue shutdown/interruption into an empty optional to
    // match the Dmn_Io::read() contract.
    return std::nullopt;
  }
}

void Dmn_DMesg::Dmn_DMesgHandler::resolveConflict(std::string_view topic) {
  this->isAfterInitialPlayback();

  m_owner->resetHandlerConflictState(this, topic);
}

void Dmn_DMesg::Dmn_DMesgHandler::setConflictCallbackTask(
    ConflictCallbackTask conflict_fn) {

  auto waitHandler = m_sub->addExecTaskWithWait([this, &conflict_fn]() -> void {
    m_conflict_callback_fn = std::move(conflict_fn);
  });

  waitHandler->wait();

  return;
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &&dmesgpb) {
  this->write(dmesgpb, false);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &dmesgpb) {
  this->write(dmesgpb, false);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &&dmesgpb,
                                        WriteFlags flags) {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  dmn::DMesgPb moved_dmesgpb = std::move(dmesgpb);

  bool block = flags.test(kBlock);
  if (flags.test(kForce)) {
    DMESG_PB_SET_MSG_FORCE(moved_dmesgpb, true);
  }

  auto waithandler =
      m_sub->addExecTaskWithWait([this, &moved_dmesgpb, block]() -> void {
        writeDMesgInternal(moved_dmesgpb, true, block);
      });
  waithandler->wait();
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &dmesgpb,
                                        WriteFlags flags) {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  bool block = flags.test(kBlock);
  if (flags.test(kForce)) {
    dmesgpb.set_force(true);
  }

  auto waitHandler =
      m_sub->addExecTaskWithWait([this, &dmesgpb, block]() -> void {
        writeDMesgInternal(dmesgpb, false, block);
      });
  waitHandler->wait();
}

auto Dmn_DMesg::Dmn_DMesgHandler::writeAndCheckConflict(dmn::DMesgPb &&dmesgpb,
                                                        WriteFlags flags)
    -> bool {
  std::string topic = dmesgpb.topic();

  flags.set(kBlock);

  this->write(dmesgpb, flags);

  return !this->isInConflict(topic);
}

auto Dmn_DMesg::Dmn_DMesgHandler::writeAndCheckConflict(dmn::DMesgPb &dmesgpb,
                                                        WriteFlags flags)
    -> bool {
  std::string topic = dmesgpb.topic();

  flags.set(kBlock);

  this->write(dmesgpb, flags);

  return !this->isInConflict(topic);
}

/**
 * @brief Stamp, validate, and publish a DMesgPb message on behalf of the handler.
 *
 * This is the serialised write path: it runs inside the handler's Dmn_Async
 * context and performs the following steps:
 *  1. Reject if the topic is currently in conflict (unless force=true).
 *  2. Fill in metadata: timestamp, source-write-handler identifier, topic
 *     (if empty and the handler has a default topic), and source identifier.
 *  3. Advance and stamp the per-topic running counter.
 *  4. Clear the conflict flag for the topic and delegate to
 *     Dmn_DMesg::publish() (move or copy according to @p move).
 *
 * @param dmesgpb The message to publish; metadata fields are filled in-place.
 * @param move    If true, publish using move semantics; otherwise copy.
 * @param block   If true, the publish call blocks until all subscribers have
 *                been notified.
 *
 * @throws std::runtime_error if the topic is in conflict and force is not set.
 */
void Dmn_DMesg::Dmn_DMesgHandler::writeDMesgInternal(dmn::DMesgPb &dmesgpb,
                                                     bool move, bool block) {
  assert(nullptr != m_owner);

  if (m_topic_in_conflict.contains(dmesgpb.topic()) && !dmesgpb.force()) {
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

  const std::string &topic = dmesgpb.topic();

  const auto next_running_counter =
      incrementByOne(m_topic_running_counter[topic]);

  DMESG_PB_SET_MSG_RUNNINGCOUNTER(dmesgpb, next_running_counter);

  m_topic_running_counter[topic] = next_running_counter;
  m_topic_in_conflict.erase(topic);

  if (move) {
    m_owner->publish(std::move_if_noexcept(dmesgpb), block);
  } else {
    m_owner->publish(dmesgpb, block);
  }
}

/**
 * @brief Internal: return true if the handler is in conflict for @p topic.
 *
 * When @p topic is empty, returns true if any topic is in conflict.
 *
 * @param topic Topic to check, or "" to check for any conflict.
 * @return true if the handler has an active conflict for the given topic.
 */
auto Dmn_DMesg::Dmn_DMesgHandler::isInConflictInternal(
    std::string_view topic) const -> bool {
  return "" == topic ? (!m_topic_in_conflict.empty())
                     : m_topic_in_conflict.contains(std::string{topic});
}

/**
 * @brief Internal: remove the conflict flag for @p topic (or all topics).
 *
 * When @p topic is empty, all conflict flags are cleared.
 *
 * @param topic Topic to clear, or "" to clear all conflicts.
 */
void Dmn_DMesg::Dmn_DMesgHandler::resolveConflictInternal(
    std::string_view topic) {
  if ("" == topic) {
    m_topic_in_conflict.clear();
  } else {
    m_topic_in_conflict.erase(std::string{topic});
  }
}

/**
 * @brief Internal: mark the handler as being in conflict for the message's topic.
 *
 * Records the topic in m_topic_in_conflict and, if a conflict callback has
 * been registered, schedules it asynchronously via m_sub.
 *
 * @param dmesgpb The conflicting message that triggered the conflict.
 */
void Dmn_DMesg::Dmn_DMesgHandler::throwConflictInternal(
    const dmn::DMesgPb &dmesgpb) {
  m_topic_in_conflict.insert(dmesgpb.topic());

  if (m_conflict_callback_fn) {
    m_sub->addExecTask([this, dmesgpb]() -> void {
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
                       ((msg.playback() &&
                         !handler->m_after_initial_playback.test()) ||
                        handler->m_after_initial_playback.test()) &&
                       (handler->m_no_topic_filter ||
                        handler->m_topic.empty() ||
                        msg.topic() == handler->m_topic);
              }},
      m_name{name} {}

Dmn_DMesg::~Dmn_DMesg() noexcept try {
  for (auto &h : m_handlers) {
    this->unregisterSubscriber(h->m_sub.get());
  }

  this->waitForEmpty();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_DMesg::closeHandler(HandlerType &handler) {
  auto inhandler = handler.m_handler.lock();
  if (!inhandler) {
    handler.m_handler.reset();

    return;
  }

  this->unregisterSubscriber(inhandler->m_sub.get());

  inhandler->m_owner = nullptr;

  const Dmn_DMesgHandler *const handler_ptr = inhandler.get();

  auto waitHandler = this->addExecTaskWithWait([this, handler_ptr]() -> void {
    auto iter =
        std::ranges::find_if(m_handlers.begin(), m_handlers.end(),
                             [handler_ptr](const auto &handler) -> bool {
                               return handler.get() == handler_ptr;
                             });

    if (iter != m_handlers.end()) {
      m_handlers.erase(iter);
    }
  });

  waitHandler->wait();

  handler.m_handler.reset();
}

auto Dmn_DMesg::getTopicLastMessage(std::string_view topic)
    -> std::optional<dmn::DMesgPb> {
  std::optional<dmn::DMesgPb> ret{};

  auto waitHandler = this->addExecTaskWithWait([this, topic, &ret]() -> void {
    auto cache = this->getLastTopicCacheInternal();

    auto iter = cache.find(std::string(topic));
    if (cache.end() != iter) {
      ret = iter->second;
    }
  });

  waitHandler->wait();

  return ret;
}

/**
 * @brief Return a reference to the per-topic last-message cache.
 *
 * This virtual method allows Dmn_DMesgNet to substitute its own per-topic
 * cache (m_topic_last_dmesgpb in the subclass) so that heartbeat messages
 * sent to the network use the most-recent state even after reconciliation.
 *
 * Must be called from within the Dmn_DMesg Dmn_Async serialisation context.
 *
 * @return Reference to the map from topic string to the last DMesgPb for that
 *         topic.
 */
auto Dmn_DMesg::getLastTopicCacheInternal()
    -> std::unordered_map<std::string, dmn::DMesgPb> & {
  return m_topic_last_dmesgpb;
}

/**
 * @brief Replay the last cached message for every known topic to newly opened
 *        handlers.
 *
 * Called during handler registration (after the subscriber is attached) to
 * ensure each handler receives the most-recent message for each topic it
 * subscribes to.  Each replayed message has the playback flag set so that
 * subscribers can distinguish initial-state messages from live updates.
 *
 * Must be called from within the Dmn_DMesg Dmn_Async serialisation context.
 */
void Dmn_DMesg::playbackLastTopicDMesgPbInternal() {
  for (auto &topic_dmesgpb : m_topic_last_dmesgpb) {
    dmn::DMesgPb msgpb = topic_dmesgpb.second;

    DMESG_PB_SET_MSG_PLAYBACK(msgpb, true);

    this->publishInternal(msgpb);
  }
}

/**
 * @brief Core publish path with conflict detection and running-counter
 *        management.
 *
 * For each non-playback message this method:
 *  1. Checks whether the originating handler is already in conflict; if so
 *     and force is not set, the message is silently dropped.
 *  2. Computes the expected next running counter for the topic.
 *     - If the incoming counter is stale (< expected) or the message already
 *       carries conflict=true, the message is re-published with conflict=true
 *       and the source handler is placed in conflict state.
 *     - Otherwise the running counter is advanced, the message is cached as
 *       the last value for the topic, and it is forwarded to all subscribers
 *       via Dmn_Pub::publishInternal().
 *
 * Playback messages bypass conflict checks and are forwarded unchanged.
 *
 * Must be called from within the Dmn_DMesg Dmn_Async serialisation context.
 *
 * @param dmesgpb The message to publish.
 */
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
  if (iter != m_handlers.end() &&
      (*iter)->isInConflictInternal(dmesgpb.topic()) && !dmesgpb.force()) {
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
  if (copied_dmesgpb.runningcounter() < next_running_counter ||
      copied_dmesgpb.conflict()) {
    copied_dmesgpb.set_conflict(true);
    if (iter != m_handlers.end()) {
      (*iter)->throwConflictInternal(copied_dmesgpb);
    }
  } else {
    DMESG_PB_SET_MSG_RUNNINGCOUNTER(copied_dmesgpb, next_running_counter);
    m_topic_running_counter[topic] = next_running_counter;
    m_topic_last_dmesgpb[topic] = copied_dmesgpb;
  }

  Dmn_Pub::publishInternal(copied_dmesgpb);
}

/**
 * @brief Publish a system-type DMesgPb message, advancing the sys running counter.
 *
 * System messages (type == DMesgTypePb::sys) use a dedicated publish path that
 * unconditionally advances the topic running counter without conflict detection.
 * This is used for cluster heartbeat / election messages that must always be
 * delivered to all subscribers regardless of ordering conflicts.
 *
 * Must be called from within the Dmn_DMesg Dmn_Async serialisation context.
 *
 * @param dmesgpb_sys The system message to publish (topic must equal
 *                    kDMesgSysIdentifier and type must be sys).
 */
void Dmn_DMesg::publishSysInternal(const dmn::DMesgPb &dmesgpb_sys) {
  assert(dmesgpb_sys.topic() == kDMesgSysIdentifier);
  assert(dmesgpb_sys.type() == dmn::DMesgTypePb::sys);

  const std::string &topic = dmesgpb_sys.topic();
  const uint64_t next_running_counter =
      incrementByOne(m_topic_running_counter[topic]);

  dmn::DMesgPb copied = dmesgpb_sys;

  DMESG_PB_SET_MSG_RUNNINGCOUNTER(copied, next_running_counter);
  Dmn_Pub::publishInternal(copied);
  m_topic_running_counter[topic] = next_running_counter;
}

void Dmn_DMesg::resetConflictStateWithLastTopicMessage(std::string_view topic) {

  auto waitHandler = this->addExecTaskWithWait([this, topic]() -> void {
    this->resetConflictStateWithLastTopicMessageInternal(topic);
  });

  waitHandler->wait();
}

/**
 * @brief Internal: forcibly re-publish the last known message for a topic to
 *        reset any conflict state on that topic across all handlers.
 *
 * If a cached message exists for @p topic, it is re-published with force=true
 * so that all handlers update their running counters and exit conflict state.
 *
 * Must be called from within the Dmn_DMesg Dmn_Async serialisation context.
 *
 * @param topic The topic whose conflict state should be reset.
 */
void Dmn_DMesg::resetConflictStateWithLastTopicMessageInternal(
    std::string_view topic) {
  auto iter = m_topic_last_dmesgpb.find(std::string{topic});
  if (m_topic_last_dmesgpb.end() != iter) {
    dmn::DMesgPb dmesgpb = iter->second;

    DMESG_PB_SET_MSG_FORCE(dmesgpb, true);
    this->publishInternal(dmesgpb);
  }
}

void Dmn_DMesg::resetHandlerConflictState(const Dmn_DMesgHandler *handler_ptr,
                                          std::string_view topic) {
  std::string topicToBeReset{topic};

  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->resetHandlerConflictStateInternal(handler_ptr, topicToBeReset); },
      this, handler_ptr, topicToBeReset);
}

/**
 * @brief Internal: clear the conflict flag on a specific handler for @p topic.
 *
 * Looks up @p handler_ptr in m_handlers and calls resolveConflictInternal() on
 * it if found.  Must be called from within the Dmn_DMesg Dmn_Async
 * serialisation context.
 *
 * @param handler_ptr Pointer to the handler whose conflict should be cleared.
 * @param topic       Topic to clear, or "" to clear all conflicts on the handler.
 */
void Dmn_DMesg::resetHandlerConflictStateInternal(
    const Dmn_DMesgHandler *handler_ptr, std::string_view topic) {
  auto iter = std::ranges::find_if(m_handlers.begin(), m_handlers.end(),
                                   [handler_ptr](const auto &handler) -> bool {
                                     return handler.get() == handler_ptr;
                                   });

  if (iter != m_handlers.end()) {
    (*iter)->resolveConflictInternal(topic);
  }
}

} // namespace dmn
