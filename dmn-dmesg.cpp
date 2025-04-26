/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 */

#include "dmn-dmesg.hpp"

#include <sys/time.h>

#include <atomic>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "dmn-debug.hpp"
#include "dmn-dmesg-pb-util.hpp"
#include "dmn-pub-sub.hpp"
#include "dmn-util.hpp"

#include "proto/dmn-dmesg.pb.h"

namespace dmn {

const char *kDMesgSysIdentifier = "sys.dmn-dmesg";

// class DMesg::DMesgHandler::DMesgHandlerSub
DMesg::DMesgHandler::DMesgHandlerSub::~DMesgHandlerSub() noexcept try {

} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void DMesg::DMesgHandler::DMesgHandlerSub::notify(dmn::DMesgPb dmesgPb) {
  if (dmesgPb.sourcewritehandleridentifier() != m_owner->m_name ||
      dmesgPb.type() == dmn::DMesgTypePb::sys) {
    std::string id = dmesgPb.topic();
    long long runningCounter = m_owner->m_topic_running_counter[id];

    if (dmesgPb.runningcounter() > runningCounter) {
      m_owner->m_topic_running_counter[id] = dmesgPb.runningcounter();

      if (dmesgPb.type() == dmn::DMesgTypePb::sys) {
        m_owner->m_last_dmesgsyspb = dmesgPb;
      }

      if ((dmn::DMesgTypePb::sys != dmesgPb.type() ||
           m_owner->m_include_dmesgsys) &&
          (!m_owner->m_filter_fn || m_owner->m_filter_fn(dmesgPb))) {
        if (m_owner->m_async_process_fn) {
          m_owner->m_async_process_fn(std::move_if_noexcept(dmesgPb));
        } else {
          m_owner->m_buffers.push(dmesgPb);
        }
      }
    }
  }
}

// class DMesg::DMesgHandler
DMesg::DMesgHandler::DMesgHandler(std::string_view name, FilterTask filterFn,
                                  AsyncProcessTask asyncProcessFn)
    : DMesgHandler{name, false, filterFn, asyncProcessFn} {}

DMesg::DMesgHandler::DMesgHandler(std::string_view name, bool includeDMesgSys,
                                  FilterTask filterFn,
                                  AsyncProcessTask asyncProcessFn)
    : m_name{name}, m_include_dmesgsys{includeDMesgSys}, m_filter_fn{filterFn},
      m_async_process_fn{asyncProcessFn} {
  // set the chained of owner for composite DMesgHandlerSub object
  m_sub.m_owner = this;
}

DMesg::DMesgHandler::~DMesgHandler() noexcept try {
  m_sub.waitForEmpty();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

std::optional<dmn::DMesgPb> DMesg::DMesgHandler::read() {
  if (nullptr != m_owner) {
    try {
      dmn::DMesgPb mesgPb = m_buffers.pop();

      return mesgPb;
    } catch (...) {
      // do nothing
    }
  }

  return {};
}

void DMesg::DMesgHandler::resolveConflict() {
  m_owner->resetHandlerConflictState(this);
}

void DMesg::DMesgHandler::setConflictCallbackTask(
    ConflictCallbackTask conflictFn) {
  m_conflict_callback_fn = conflictFn;
}

void DMesg::DMesgHandler::write(dmn::DMesgPb &&dmesgPb) {
  if (nullptr == m_owner) {
    return;
  }

  dmn::DMesgPb movedDMesgPb = std::move_if_noexcept(dmesgPb);

  writeDMesgInternal(movedDMesgPb, true);
}

void DMesg::DMesgHandler::write(dmn::DMesgPb &dmesgPb) {
  if (nullptr == m_owner) {
    return;
  }

  writeDMesgInternal(dmesgPb, false);
}

void DMesg::DMesgHandler::waitForEmpty() { m_sub.waitForEmpty(); }

void DMesg::DMesgHandler::writeDMesgInternal(dmn::DMesgPb &dmesgPb, bool move) {
  assert(nullptr != m_owner);

  if (m_in_conflict) {
    throw std::runtime_error("last write results in conflicted, "
                             "handler needs to be reset");
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);

  std::string topic = dmesgPb.topic();
  long long nextRunningCounter = incrementByOne(m_topic_running_counter[topic]);

  DMESG_PB_SET_MSG_TIMESTAMP_FROM_TV(dmesgPb, tv);
  DMESG_PB_SET_MSG_SOURCEWRITEHANDLERIDENTIFIER(dmesgPb, m_name);
  DMESG_PB_SET_MSG_RUNNINGCOUNTER(dmesgPb, nextRunningCounter);

  if ("" == dmesgPb.sourceidentifier()) {
    DMESG_PB_SET_MSG_SOURCEIDENTIFIER(dmesgPb, m_name);
  }

  if (move) {
    m_owner->publish(std::move_if_noexcept(dmesgPb));
  } else {
    m_owner->publish(dmesgPb);
  }

  m_topic_running_counter[topic] = nextRunningCounter;
}

bool DMesg::DMesgHandler::isInConflict() const { return m_in_conflict; }

void DMesg::DMesgHandler::resolveConflictInternal() { m_in_conflict = false; }

void DMesg::DMesgHandler::throwConflict(const dmn::DMesgPb dmesgPb) {
  m_in_conflict = true;

  if (m_conflict_callback_fn) {
    m_sub.write(
        [this, dmesgPb]() { this->m_conflict_callback_fn(*this, dmesgPb); });
  }
}

// class DMesg
DMesg::DMesg(std::string_view name, KeyValueConfiguration config)
    : Pub{name, 0, // DMesg manages re-send per topic
          [this](const Sub *const sub, const dmn::DMesgPb &msg) {
            const DMesgHandler::DMesgHandlerSub *const handlerSub =
                dynamic_cast<const DMesgHandler::DMesgHandlerSub *const>(sub);
            assert(handlerSub != nullptr);

            DMesgHandler *handler = handlerSub->m_owner;

            return nullptr != handler && nullptr != handler->m_owner &&
                   (true == msg.playback() ||
                    handler->m_after_initial_playback) &&
                   (handler->m_subscribed_topics.size() == 0 ||
                    std::find(handler->m_subscribed_topics.begin(),
                              handler->m_subscribed_topics.end(),
                              msg.topic()) !=
                        handler->m_subscribed_topics.end());
          }},
      m_name{name}, m_config{config} {}

DMesg::~DMesg() noexcept try { this->waitForEmpty(); } catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void DMesg::closeHandler(std::shared_ptr<DMesg::DMesgHandler> &handlerToClose) {
  this->unregisterSubscriber(&(handlerToClose->m_sub));
  handlerToClose->waitForEmpty();
  handlerToClose->m_owner = nullptr;

  DMesgHandler *handlerPtr = handlerToClose.get();
  handlerToClose = {};

  DMN_ASYNC_CALL_WITH_CAPTURE(
      {
        std::vector<std::shared_ptr<DMesgHandler>>::iterator it = std::find_if(
            m_handlers.begin(), m_handlers.end(),
            [handlerPtr](auto handler) { return handler.get() == handlerPtr; });

        if (it != m_handlers.end()) {
          m_handlers.erase(it);
        }
      },
      this, handlerPtr);
}

void DMesg::playbackLastTopicDMesgPbInternal() {
  for (auto &topicDmesgPb : m_topic_last_dmesgpb) {
    dmn::DMesgPb pb = topicDmesgPb.second;

    DMESG_PB_SET_MSG_PLAYBACK(pb, true);

    this->publishInternal(pb);
  }
}

void DMesg::publishInternal(dmn::DMesgPb dmesgPb) {
  // for message that is playback, we skip the check if it is conflict as
  // only openHandler with lower running counter will read those message.
  if (dmesgPb.playback()) {
    Pub::publishInternal(dmesgPb);
  } else {
    std::string id = dmesgPb.topic();

    long long nextRunningCounter = incrementByOne(m_topic_running_counter[id]);

    std::vector<std::shared_ptr<DMesgHandler>>::iterator it = std::find_if(
        m_handlers.begin(), m_handlers.end(), [&dmesgPb](auto handler) {
          return handler->m_name == dmesgPb.sourcewritehandleridentifier();
        });

    if (it != m_handlers.end() && (*it)->isInConflict()) {
      // avoid throw conflict multiple times
      return;
    } else if (dmesgPb.runningcounter() < nextRunningCounter) {
      if (it != m_handlers.end()) {
        (*it)->throwConflict(dmesgPb);

        return;
      }
    }

    DMESG_PB_SET_MSG_RUNNINGCOUNTER(dmesgPb, nextRunningCounter);
    Pub::publishInternal(dmesgPb);
    m_topic_running_counter[id] = nextRunningCounter;
    m_topic_last_dmesgpb[id] = dmesgPb;
  }
}

void DMesg::publishSysInternal(dmn::DMesgPb dmesgSysPb) {
  assert(dmesgSysPb.topic() == kDMesgSysIdentifier);
  assert(dmesgSysPb.type() == dmn::DMesgTypePb::sys);

  std::string id = dmesgSysPb.topic();
  long long nextRunningCounter = incrementByOne(m_topic_running_counter[id]);

  DMESG_PB_SET_MSG_RUNNINGCOUNTER(dmesgSysPb, nextRunningCounter);
  Pub::publishInternal(dmesgSysPb);
  m_topic_running_counter[id] = nextRunningCounter;
}

void DMesg::resetHandlerConflictState(const DMesgHandler *handlerPtr) {
  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->resetHandlerConflictStateInternal(handlerPtr); }, this,
      handlerPtr);
}

void DMesg::resetHandlerConflictStateInternal(const DMesgHandler *handlerPtr) {
  std::vector<std::shared_ptr<DMesgHandler>>::iterator it = std::find_if(
      m_handlers.begin(), m_handlers.end(),
      [handlerPtr](auto handler) { return handler.get() == handlerPtr; });

  if (it != m_handlers.end()) {
    (*it)->resolveConflictInternal();
  }
}

} // namespace dmn
