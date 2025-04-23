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

const char *DMesgSysIdentifier = "sys.dmn-dmesg";

// class Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub::
    ~Dmn_DMesgHandlerSub() noexcept try {

} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandlerSub::notify(
    dmn::DMesgPb dmesgPb) {
  if (dmesgPb.sourcewritehandleridentifier() != m_owner->m_name ||
      dmesgPb.type() == dmn::DMesgTypePb::sys) {
    std::string id = dmesgPb.topic();
    long long runningCounter = m_owner->m_topicRunningCounter[id];

    if (dmesgPb.runningcounter() > runningCounter) {
      m_owner->m_topicRunningCounter[id] = dmesgPb.runningcounter();

      if (dmesgPb.type() == dmn::DMesgTypePb::sys) {
        m_owner->m_lastDMesgSysPb = dmesgPb;
      }

      if ((dmn::DMesgTypePb::sys != dmesgPb.type() ||
           m_owner->m_includeDMesgSys) &&
          (!m_owner->m_filterFn || m_owner->m_filterFn(dmesgPb))) {
        if (m_owner->m_asyncProcessFn) {
          m_owner->m_asyncProcessFn(std::move_if_noexcept(dmesgPb));
        } else {
          m_owner->m_buffers.push(dmesgPb);
        }
      }
    }
  }
}

// class Dmn_DMesg::Dmn_DMesgHandler
Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              FilterTask filterFn,
                                              AsyncProcessTask asyncProcessFn)
    : Dmn_DMesgHandler{name, false, filterFn, asyncProcessFn} {}

Dmn_DMesg::Dmn_DMesgHandler::Dmn_DMesgHandler(std::string_view name,
                                              bool includeDMesgSys,
                                              FilterTask filterFn,
                                              AsyncProcessTask asyncProcessFn)
    : m_name{name}, m_includeDMesgSys{includeDMesgSys}, m_filterFn{filterFn},
      m_asyncProcessFn{asyncProcessFn} {
  // set the chained of owner for composite Dmn_DMesgHandlerSub object
  m_sub.m_owner = this;
}

Dmn_DMesg::Dmn_DMesgHandler::~Dmn_DMesgHandler() noexcept try {
  m_sub.waitForEmpty();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

std::optional<dmn::DMesgPb> Dmn_DMesg::Dmn_DMesgHandler::read() {
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

void Dmn_DMesg::Dmn_DMesgHandler::resolveConflict() {
  m_owner->resetHandlerConflictState(this);
}

void Dmn_DMesg::Dmn_DMesgHandler::setConflictCallbackTask(
    ConflictCallbackTask conflictFn) {
  m_conflictCallbackFn = conflictFn;
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &&dmesgPb) {
  if (nullptr == m_owner) {
    return;
  }

  dmn::DMesgPb movedDMesgPb = std::move_if_noexcept(dmesgPb);

  writeDMesgInternal(movedDMesgPb, true);
}

void Dmn_DMesg::Dmn_DMesgHandler::write(dmn::DMesgPb &dmesgPb) {
  if (nullptr == m_owner) {
    return;
  }

  writeDMesgInternal(dmesgPb, false);
}

void Dmn_DMesg::Dmn_DMesgHandler::waitForEmpty() { m_sub.waitForEmpty(); }

void Dmn_DMesg::Dmn_DMesgHandler::writeDMesgInternal(dmn::DMesgPb &dmesgPb,
                                                     bool move) {
  assert(nullptr != m_owner);

  if (m_inConflict) {
    throw std::runtime_error("last write results in conflicted, "
                             "handler needs to be reset");
  }

  struct timeval tv;
  gettimeofday(&tv, NULL);

  std::string topic = dmesgPb.topic();
  long long nextRunningCounter = incrementByOne(m_topicRunningCounter[topic]);

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

  m_topicRunningCounter[topic] = nextRunningCounter;
}

bool Dmn_DMesg::Dmn_DMesgHandler::isInConflict() const { return m_inConflict; }

void Dmn_DMesg::Dmn_DMesgHandler::resolveConflictInternal() {
  m_inConflict = false;
}

void Dmn_DMesg::Dmn_DMesgHandler::throwConflict(const dmn::DMesgPb dmesgPb) {
  m_inConflict = true;

  if (m_conflictCallbackFn) {
    m_sub.write(
        [this, dmesgPb]() { this->m_conflictCallbackFn(*this, dmesgPb); });
  }
}

// class Dmn_DMesg
Dmn_DMesg::Dmn_DMesg(std::string_view name, KeyValueConfiguration config)
    : Dmn_Pub{name, 0, // Dmn_DMesg manages re-send per topic
              [this](const Dmn_Sub *const sub, const dmn::DMesgPb &msg) {
                const Dmn_DMesgHandler::Dmn_DMesgHandlerSub *const handlerSub =
                    dynamic_cast<
                        const Dmn_DMesgHandler::Dmn_DMesgHandlerSub *const>(
                        sub);
                assert(handlerSub != nullptr);

                Dmn_DMesgHandler *handler = handlerSub->m_owner;

                return nullptr != handler && nullptr != handler->m_owner &&
                       (true == msg.playback() ||
                        handler->m_afterInitialPlayback) &&
                       (handler->m_subscribedTopics.size() == 0 ||
                        std::find(handler->m_subscribedTopics.begin(),
                                  handler->m_subscribedTopics.end(),
                                  msg.topic()) !=
                            handler->m_subscribedTopics.end());
              }},
      m_name{name}, m_config{config} {}

Dmn_DMesg::~Dmn_DMesg() noexcept try { this->waitForEmpty(); } catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

void Dmn_DMesg::closeHandler(
    std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> &handlerToClose) {
  this->unregisterSubscriber(&(handlerToClose->m_sub));
  handlerToClose->waitForEmpty();
  handlerToClose->m_owner = nullptr;

  Dmn_DMesgHandler *handlerPtr = handlerToClose.get();
  handlerToClose = {};

  DMN_ASYNC_CALL_WITH_CAPTURE(
      {
        std::vector<std::shared_ptr<Dmn_DMesgHandler>>::iterator it =
            std::find_if(m_handlers.begin(), m_handlers.end(),
                         [handlerPtr](auto handler) {
                           return handler.get() == handlerPtr;
                         });

        if (it != m_handlers.end()) {
          m_handlers.erase(it);
        }
      },
      this, handlerPtr);
}

void Dmn_DMesg::playbackLastTopicDMesgPbInternal() {
  for (auto &topicDmesgPb : m_topicLastDMesgPb) {
    dmn::DMesgPb pb = topicDmesgPb.second;

    DMESG_PB_SET_MSG_PLAYBACK(pb, true);

    this->publishInternal(pb);
  }
}

void Dmn_DMesg::publishInternal(dmn::DMesgPb dmesgPb) {
  // for message that is playback, we skip the check if it is conflict as
  // only openHandler with lower running counter will read those message.
  if (dmesgPb.playback()) {
    Dmn_Pub::publishInternal(dmesgPb);
  } else {
    std::string id = dmesgPb.topic();

    long long nextRunningCounter = incrementByOne(m_topicRunningCounter[id]);

    std::vector<std::shared_ptr<Dmn_DMesgHandler>>::iterator it = std::find_if(
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
    Dmn_Pub::publishInternal(dmesgPb);
    m_topicRunningCounter[id] = nextRunningCounter;
    m_topicLastDMesgPb[id] = dmesgPb;
  }
}

void Dmn_DMesg::publishSysInternal(dmn::DMesgPb dmesgSysPb) {
  assert(dmesgSysPb.topic() == DMesgSysIdentifier);
  assert(dmesgSysPb.type() == dmn::DMesgTypePb::sys);

  std::string id = dmesgSysPb.topic();
  long long nextRunningCounter = incrementByOne(m_topicRunningCounter[id]);

  DMESG_PB_SET_MSG_RUNNINGCOUNTER(dmesgSysPb, nextRunningCounter);
  Dmn_Pub::publishInternal(dmesgSysPb);
  m_topicRunningCounter[id] = nextRunningCounter;
}

void Dmn_DMesg::resetHandlerConflictState(const Dmn_DMesgHandler *handlerPtr) {
  DMN_ASYNC_CALL_WITH_CAPTURE(
      { this->resetHandlerConflictStateInternal(handlerPtr); }, this,
      handlerPtr);
}

void Dmn_DMesg::resetHandlerConflictStateInternal(
    const Dmn_DMesgHandler *handlerPtr) {
  std::vector<std::shared_ptr<Dmn_DMesgHandler>>::iterator it = std::find_if(
      m_handlers.begin(), m_handlers.end(),
      [handlerPtr](auto handler) { return handler.get() == handlerPtr; });

  if (it != m_handlers.end()) {
    (*it)->resolveConflictInternal();
  }
}

} // namespace dmn
