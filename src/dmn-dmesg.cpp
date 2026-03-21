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
/** @brief Full constructor: initialises all handler fields from the given
 * arguments. */
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

/** @brief Delegates to the full constructor using kHandlerConfig_Default. */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn)
    : Dmn_DMesgHandler{name, topic, std::move(filter_fn),
                       std::move(async_process_fn), kHandlerConfig_Default} {}

/** @brief Delegates with a null async-process callback. */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic,
                                              FilterTask filter_fn)
    : Dmn_DMesgHandler{name, topic, std::move(filter_fn),
                       static_cast<AsyncProcessTask>(nullptr)} {}

/** @brief Delegates with null filter and async-process callbacks. */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              std::string_view topic)
    : Dmn_DMesgHandler{name, topic, static_cast<FilterTask>(nullptr)} {}

/** @brief Delegates to the full constructor with an empty topic string. */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn,
                                              HandlerConfig configs)
    : Dmn_DMesgHandler{name, "", std::move(filter_fn),
                       std::move(async_process_fn), std::move(configs)} {}

/** @brief Delegates with kHandlerConfig_Default for the empty-topic overload.
 */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filter_fn,
                                              AsyncProcessTask async_process_fn)
    : Dmn_DMesgHandler{name, std::move(filter_fn), std::move(async_process_fn),
                       kHandlerConfig_Default} {}

/** @brief Delegates with a null async-process callback for the empty-topic
 * overload. */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filter_fn)
    : Dmn_DMesgHandler{name, std::move(filter_fn),
                       static_cast<AsyncProcessTask>(nullptr)} {}

/** @brief Minimal constructor: name only, all callbacks null, empty topic. */
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name)
    : Dmn_DMesgHandler{name, static_cast<FilterTask>(nullptr)} {}

Dmn_DMesg::Dmn_DMesgHandler::~Dmn_DMesgHandler() noexcept try {
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

/**
 * @brief Posts an async task to the publisher's context to check the
 *        conflict state, blocking until the result is available.
 *
 * @param topic Topic to check, or "" for any topic.
 * @return true if the handler is in conflict for the given topic.
 */
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

/** @brief Spin-waits until the initial playback of last-known messages
 * completes. */
void Dmn_DMesg::Dmn_DMesgHandler::isAfterInitialPlayback() {
  while (!m_after_initial_playback.test()) {
    m_after_initial_playback.wait(false, std::memory_order_relaxed);
  }
}

/**
 * @brief Block until the initial playback is done, then return the
 *        per-topic running counter from the publisher's async context.
 *
 * @param topic Topic whose running counter is requested.
 * @return Current running counter value for the topic.
 */
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

/** @brief Look up the running counter for @p topic directly (no locking). */
auto Dmn_DMesg::Dmn_DMesgHandler::getTopicRunningCounterInternal(
    std::string_view topic) -> uint64_t {
  auto iter = m_topic_running_counter.find(std::string{topic});
  if (m_topic_running_counter.end() == iter) {
    return 0;
  }

  return iter->second;
}

/** @brief Schedule setAfterInitialPlaybackInternal() in the async context. */
void Dmn_DMesg::Dmn_DMesgHandler::setAfterInitialPlayback() {
  [[maybe_unused]] auto waitHandler = m_sub->addExecTaskWithWait(
      [this]() -> void { this->setAfterInitialPlaybackInternal(); });
}

/** @brief Set the playback-complete flag and wake all waiters (runs in async
 * context). */
void Dmn_DMesg::Dmn_DMesgHandler::setAfterInitialPlaybackInternal() {
  m_after_initial_playback.test_and_set(std::memory_order_relaxed);
  m_after_initial_playback.notify_all();
}

/**
 * @brief Set the running counter for @p topic from the publisher's async
 * context.
 *
 * @param topic          Topic to update.
 * @param runningCounter New counter value.
 */
void Dmn_DMesg::Dmn_DMesgHandler::setTopicRunningCounter(
    std::string_view topic, uint64_t runningCounter) {
  auto waitHandler =
      m_sub->addExecTaskWithWait([this, &runningCounter, topic]() -> void {
        this->setTopicRunningCounterInternal(topic, runningCounter);
      });

  waitHandler->wait();
}

/** @brief Directly update the counter map entry for @p topic (no locking). */
void Dmn_DMesg::Dmn_DMesgHandler::setTopicRunningCounterInternal(
    std::string_view topic, uint64_t runningCounter) {
  m_topic_running_counter[std::string{topic}] = runningCounter;
}

/**
 * @brief Block waiting for the next DMesgPb from the internal buffer.
 *
 * Waits for the initial playback to complete before blocking on the
 * buffer queue. Returns std::nullopt if the queue is shut down.
 *
 * @return The next message, or std::nullopt on shutdown/error.
 */
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

/**
 * @brief Delegate conflict resolution to the publisher's async context via
 *        Dmn_DMesg::resetHandlerConflictState().
 *
 * @param topic Topic to resolve, or "" for all topics.
 */
void Dmn_DMesg::Dmn_DMesgHandler::resolveConflict(std::string_view topic) {
  this->isAfterInitialPlayback();

  m_owner->resetHandlerConflictState(this, topic);
}

/** @brief Post the conflict callback update to the async context and wait. */
void Dmn_DMesg::Dmn_DMesgHandler::setConflictCallbackTask(
    ConflictCallbackTask conflict_fn) {

  auto waitHandler = m_sub->addExecTaskWithWait([this, &conflict_fn]() -> void {
    m_conflict_callback_fn = std::move(conflict_fn);
  });

  waitHandler->wait();

  return;
}

/** @brief Move-write overload: delegates to write(dmesgpb, flags=kDefault). */
void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &&dmesgpb) {
  this->write(dmesgpb, false);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(const dmn::DMesgPb &dmesgpb) {
  this->write(dmesgpb, false);
}

/**
 * @brief Move-write with flags: moves the message and dispatches
 *        writeDMesgInternal() in the async context.
 *
 * @param dmesgpb Message to publish (moved).
 * @param flags   Bitmask of WriteOptions (kBlock, kForce).
 */
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

/**
 * @brief Copy-write with flags: copies the message and dispatches
 *        writeDMesgInternal() in the async context.
 *
 * @param dmesgpb Message to publish (copied).
 * @param flags   Bitmask of WriteOptions (kBlock, kForce).
 */
void Dmn_DMesg::Dmn_DMesgHandler::write(const dmn::DMesgPb &dmesgpb,
                                        WriteFlags flags) {
  assert(nullptr != m_owner);

  this->isAfterInitialPlayback();

  auto copiedDmesgpb = dmesgpb;

  bool block = flags.test(kBlock);
  if (flags.test(kForce)) {
    copiedDmesgpb.set_force(true);
  }

  auto waitHandler =
      m_sub->addExecTaskWithWait([this, &copiedDmesgpb, block]() -> void {
        writeDMesgInternal(copiedDmesgpb, false, block);
      });

  waitHandler->wait();
}

/**
 * @brief Move-write then check for conflict; sets kBlock automatically.
 *
 * @param dmesgpb Message to publish (moved).
 * @param flags   Additional WriteOptions (kForce, etc.).
 * @return true if write succeeded without conflict, false otherwise.
 */
auto Dmn_DMesg::Dmn_DMesgHandler::writeAndCheckConflict(dmn::DMesgPb &&dmesgpb,
                                                        WriteFlags flags)
    -> bool {
  std::string topic = dmesgpb.topic();

  flags.set(kBlock);

  this->write(dmesgpb, flags);

  return !this->isInConflict(topic);
}

/**
 * @brief Copy-write then check for conflict; sets kBlock automatically.
 *
 * @param dmesgpb Message to publish (copied).
 * @param flags   Additional WriteOptions (kForce, etc.).
 * @return true if write succeeded without conflict, false otherwise.
 */
auto Dmn_DMesg::Dmn_DMesgHandler::writeAndCheckConflict(dmn::DMesgPb &dmesgpb,
                                                        WriteFlags flags)
    -> bool {
  std::string topic = dmesgpb.topic();

  flags.set(kBlock);

  this->write(dmesgpb, flags);

  return !this->isInConflict(topic);
}

/**
 * @brief Stamp and publish the message; must be called from the async context.
 *
 * Sets timestamp, source identifiers, and topic (if unset), increments the
 * per-topic running counter, then calls Dmn_Pub::publish().
 *
 * @param dmesgpb Message to publish (modified in-place).
 * @param move    If true, publish via std::move_if_noexcept; otherwise copy.
 * @param block   If true, block until the publisher processes the message.
 * @throws std::runtime_error if the handler is already in conflict for the
 * topic.
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
 * @brief Check conflict state directly (no async dispatch).
 *
 * @param topic Topic to check, or "" to check any topic.
 * @return true if the handler has at least one conflicted topic (or the
 *         specific topic is in conflict).
 */
auto Dmn_DMesg::Dmn_DMesgHandler::isInConflictInternal(
    std::string_view topic) const -> bool {
  return "" == topic ? (!m_topic_in_conflict.empty())
                     : m_topic_in_conflict.contains(std::string{topic});
}

/**
 * @brief Remove @p topic (or all topics if empty) from the conflict set.
 *
 * Must be called from within the publisher's async context.
 *
 * @param topic Topic to clear, or "" to clear all conflicted topics.
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
 * @brief Mark the topic as conflicted and schedule the conflict callback.
 *
 * Inserts the message's topic into the conflict set and, if a conflict
 * callback is registered, posts it for execution on the async thread.
 *
 * @param dmesgpb The message that triggered the conflict.
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
  auto waitHandler = this->addExecTaskWithWait([this]() -> void {
    for (auto &h : m_handlers) {
      this->unregisterSubscriber(h->m_sub.get());
    }

    m_handlers.clear();
  });

  waitHandler->wait();

  this->waitForEmpty();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

/**
 * @brief Unregister the handler's subscriber, clear its owner pointer, remove
 *        it from the publisher's handler list, and reset the proxy's weak_ptr.
 *
 * @param handler Proxy referencing the handler to close.
 */
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

/**
 * @brief Return the last published message for @p topic, or std::nullopt if
 *        none has been published yet. The lookup is performed in the async
 * context.
 *
 * @param topic Topic to look up.
 * @return The last DMesgPb for the topic, or std::nullopt.
 */
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

/** @brief Return a mutable reference to the per-topic last-message cache (no
 * locking). */
auto Dmn_DMesg::getLastTopicCacheInternal()
    -> std::unordered_map<std::string, dmn::DMesgPb> & {
  return m_topic_last_dmesgpb;
}

/** @brief Re-publish each topic's last-known message with the playback flag
 * set. */
void Dmn_DMesg::playbackLastTopicDMesgPbInternal() {
  for (auto &topic_dmesgpb : m_topic_last_dmesgpb) {
    dmn::DMesgPb msgpb = topic_dmesgpb.second;

    DMESG_PB_SET_MSG_PLAYBACK(msgpb, true);

    this->publishInternal(msgpb);
  }
}

/**
 * @brief Override of Dmn_Pub::publishInternal() that applies global conflict
 *        detection and advances per-topic running counters before forwarding
 *        to the base class for subscriber notification.
 *
 * Playback messages bypass conflict detection. For normal messages:
 *  - If the source handler is already in conflict the message is silently
 * dropped.
 *  - If the incoming running counter is stale, the message is marked conflicted
 *    and the source handler is placed in conflict state.
 *  - Otherwise the global counter and last-message cache are updated.
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
 * @brief Publish a sys-type message, advancing only the sys-topic counter.
 *
 * Unlike publishInternal(), this path does not perform conflict detection
 * and does not update the last-message cache (sys messages are special).
 *
 * @param dmesgpb_sys System message to publish; must use the sys topic and
 * type.
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

/**
 * @brief Force-publish the last known message for @p topic so all handlers
 *        can synchronise to it, clearing their conflict state.
 *
 * @param topic Topic whose last message should be force-republished.
 */
void Dmn_DMesg::resetConflictStateWithLastTopicMessage(std::string_view topic) {

  auto waitHandler = this->addExecTaskWithWait([this, topic]() -> void {
    this->resetConflictStateWithLastTopicMessageInternal(topic);
  });

  waitHandler->wait();
}

/** @brief Internal: force-republish the last cached message for @p topic (no
 * async dispatch). */
void Dmn_DMesg::resetConflictStateWithLastTopicMessageInternal(
    std::string_view topic) {
  auto iter = m_topic_last_dmesgpb.find(std::string{topic});
  if (m_topic_last_dmesgpb.end() != iter) {
    dmn::DMesgPb dmesgpb = iter->second;

    DMESG_PB_SET_MSG_FORCE(dmesgpb, true);
    this->publishInternal(dmesgpb);
  }
}

/**
 * @brief Post an async task to reset the conflict state of a specific handler
 *        for the given topic.
 *
 * @param handler_ptr Pointer to the handler whose conflict state should be
 * reset.
 * @param topic       Topic to resolve, or "" for all topics.
 */
void Dmn_DMesg::resetHandlerConflictState(const Dmn_DMesgHandler *handler_ptr,
                                          std::string_view topic) {
  std::string topicToBeReset{topic};

  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->resetHandlerConflictStateInternal(handler_ptr, topicToBeReset); },
      this, handler_ptr, topicToBeReset);
}

/** @brief Locate @p handler_ptr in the handler list and call
 * resolveConflictInternal(). */
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
