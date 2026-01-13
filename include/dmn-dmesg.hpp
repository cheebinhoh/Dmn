/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesg.hpp
 * @brief DMESG publisher/subscriber wrapper using Protobuf messages.
 *
 * This header defines Dmn_DMesg, a publisher that uses a Protobuf message
 * (dmn::DMesgPb) as the message payload, and Dmn_DMesg::Dmn_DMesgHandler,
 * a light-weight IO-style handler for clients to read and write DMesgPb
 * messages.
 *
 * Key ideas and behaviour:
 *  - Messages are represented by the generated Protobuf type dmn::DMesgPb
 *    (proto/dmn-dmesg.proto). Clients extend the Protobuf message when they
 *    need extra fields, rather than subclassing a C++ base class.
 *
 *  - Instead of requiring clients to subclass Dmn_Pub::Dmn_Sub for subscribing,
 *    the Dmn_DMesg provides handlers (Dmn_DMesgHandler) which compose a small
 *    internal subscriber object. Clients hold a shared_ptr to a handler that
 *    implements a Dmn_Io-like read/write API for DMesgPb messages.
 *
 *  - Handlers may subscribe to a specific topic (dmn::DMesgPb::topic). They
 *    can optionally provide a filter functor to drop unwanted messages and an
 *    async-processing functor to process messages as they arrive.
 *
 *  - DMesg supports concurrent publishers (handlers) publishing to the same
 *    Dmn_DMesg instance. A simple running-counter based conflict detection is
 *    implemented: if an incoming message's topic running counter is older than
 *    the publisher's current running counter for that topic, only the writer's
 *    handler is marked in a conflict state; further writes from that handler
 *    will be rejected until the client resolves the conflict.
 *
 *  - To avoid locking in the hot path, some operations (for example handler
 *    registration, playback of last-known messages per topic, and conflict
 *    state resets) are performed as asynchronous tasks in the publisher's
 *    singleton async thread context.
 *
 * Design notes:
 *  - A Dmn_DMesgHandler composes a nested Dmn_DMesgHandlerSub which is a
 *    concrete implementation of Dmn_Pub<dmn::DMesgPb>::Dmn_Sub. This avoids
 *    complex multiple inheritance and keeps handler lifetime management simple.
 *
 *  - Handler configuration options (HandlerConfig) control behaviour such as
 *    whether system messages are delivered to handlers and whether topic-based
 *    filtering is enforced on read.
 *
 *  - The Dmn_DMesg class itself stores per-topic running counters and the
 *    last published message for each topic so that newly opened handlers can
 *    receive the most recent message for each topic (playback).
 */

#ifndef DMN_DMESG_HPP_
#define DMN_DMESG_HPP_

#include <atomic>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sys/time.h>

#include "dmn-pub-sub.hpp"

#include "proto/dmn-dmesg.pb.h"

namespace dmn {

extern const char *const kDMesgSysIdentifier;

class Dmn_DMesg : public Dmn_Pub<dmn::DMesgPb> {
public:
  using AsyncProcessTask = std::function<void(dmn::DMesgPb)>;
  using FilterTask = std::function<bool(const dmn::DMesgPb &)>;
  using HandlerConfig = std::unordered_map<std::string, std::string>;

  /**
   * @brief Default handler configuration values.
   *
   * Typical defaults:
   *  - kHandlerConfig_IncludeSys => "no"
   *  - kHandlerConfig_NoTopicFilter => "no"
   */
  static const HandlerConfig kHandlerConfig_Default;

  /**
   * @brief If set to "yes" or "1", handlers opened with this config will
   *        receive system messages (DMesgPb.message_type == "sys").
   */
  static constexpr std::string_view kHandlerConfig_IncludeSys =
      "Handler_IncludeSys";

  /**
   * @brief If set to "yes" or "1", handlers opened with this config will
   *        ignore topic filtering when reading: read() returns messages
   *        regardless of topic value and write() will not set the message
   *        topic automatically.
   */
  static constexpr std::string_view kHandlerConfig_NoTopicFilter =
      "Handler_NoTopicFilter";

  /**
   * @brief Type used to represent key/value configuration entries for the
   *        Dmn_DMesg object itself.
   */
  using KeyValueConfiguration = std::unordered_map<std::string, std::string>;

  /**
   * @brief Dmn_DMesgHandler is an IO-style interface for clients to publish
   *        and consume dmn::DMesgPb messages. It implements Dmn_Io<DMesgPb>.
   *
   * The handler composes a small nested subscriber object (Dmn_DMesgHandlerSub)
   * that integrates with the underlying Dmn_Pub infrastructure. The handler
   * maintains its own filters, async processing callback, per-topic running
   * counters, and a small buffer for arriving messages.
   *
   * Clients obtain handlers from Dmn_DMesg::openHandler(...) and release them
   * via Dmn_DMesg::closeHandler(...).
   */
  class Dmn_DMesgHandler : public Dmn_Io<dmn::DMesgPb> {
  private:
    using ConflictCallbackTask =
        std::function<void(Dmn_DMesgHandler &handler, const dmn::DMesgPb &)>;

    /**
     * @brief Internal subscriber that forwards notifications into its owning
     *        Dmn_DMesgHandler.
     *
     * This class is intentionally small and is used only by Dmn_DMesgHandler.
     * m_owner points to the owning handler so that notify(...) can update the
     * handler state (buffer, conflict detection, async processing, etc.).
     */
    class Dmn_DMesgHandlerSub : public dmn::Dmn_Pub<dmn::DMesgPb>::Dmn_Sub {
    public:
      Dmn_DMesgHandlerSub() = default;
      virtual ~Dmn_DMesgHandlerSub() noexcept;

      Dmn_DMesgHandlerSub(const Dmn_DMesgHandlerSub &obj) = delete;
      const Dmn_DMesgHandlerSub &
      operator=(const Dmn_DMesgHandlerSub &obj) = delete;
      Dmn_DMesgHandlerSub(Dmn_DMesgHandlerSub &&obj) = delete;
      Dmn_DMesgHandlerSub &operator=(Dmn_DMesgHandlerSub &&obj) = delete;

      /**
       * @brief Called by the publisher to notify this subscriber of a new
       *        DMesgPb message.
       *
       * Behavior summary:
       *  - Messages published by the same handler are skipped (handler does
       *    not re-receive its own writes), except system messages which can be
       *    delivered based on handler configuration.
       *  - Messages with a running counter older than the handler's last seen
       *    counter for the topic are skipped (out-of-order / stale).
       *  - System messages may be saved as the handler's last-known system
       *    message.
       *  - If configured, system messages can be queued for read() or passed
       *    to m_async_process_fn for asynchronous handling.
       *
       * @param dmesgPb The message delivered by the publisher.
       */
      void notify(const dmn::DMesgPb &dmesgpb) override;

      // m_owner is intentionally public to allow the containing Dmn_DMesg to
      // access and wire-up the subscriber. This nested class is private to the
      // handler and not exposed to external clients.
      Dmn_DMesgHandler *m_owner{};
    }; // class Dmn_DMesgHandlerSub

  public:
    /**
     * @brief Construct a handler that subscribes to a specific topic and
     *        optionally provides filter and async-process callbacks.
     *
     * @param name             Unique name/identifier for the handler.
     * @param topic            Topic string to subscribe/publish to (empty
     *                         string is a valid topic).
     * @param filter_fn        Optional filter functor: return false to drop
     *                         a message.
     * @param async_process_fn Optional functor to process messages
     *                         asynchronously as they arrive.
     * @param configs          Optional handler-specific configuration.
     */
    Dmn_DMesgHandler(std::string_view name, std::string_view topic,
                     FilterTask filter_fn, AsyncProcessTask async_process_fn,
                     HandlerConfig configs);

    /**
     * @brief Same as above but with default HandlerConfig.
     */
    Dmn_DMesgHandler(std::string_view name, std::string_view topic,
                     FilterTask filter_fn, AsyncProcessTask async_process_fn);

    /**
     * @brief Construct a handler with topic and a filter (no async fn),
     *        using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, std::string_view topic,
                     FilterTask filter_fn);

    /**
     * @brief Construct a handler with topic only (no filter, no async fn),
     *        using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, std::string_view topic);

    /**
     * @brief Construct a handler that subscribes to the empty topic with
     *        filter and async-process callbacks and a custom configuration.
     */
    Dmn_DMesgHandler(std::string_view name, FilterTask filter_fn,
                     AsyncProcessTask async_process_fn, HandlerConfig configs);

    /**
     * @brief Construct a handler that subscribes to the empty topic with
     *        filter and async-process callbacks using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, FilterTask filter_fn,
                     AsyncProcessTask async_process_fn);

    /**
     * @brief Construct a handler that subscribes to the empty topic with
     *        filter only, using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, FilterTask filter_fn);

    /**
     * @brief Construct a handler with only a name; subscribes to the empty
     *        topic with default behaviour.
     */
    Dmn_DMesgHandler(std::string_view name);

    virtual ~Dmn_DMesgHandler() noexcept;

    Dmn_DMesgHandler(const Dmn_DMesgHandler &obj) = delete;
    const Dmn_DMesgHandler &operator=(const Dmn_DMesgHandler &obj) = delete;
    Dmn_DMesgHandler(Dmn_DMesgHandler &&obj) = delete;
    Dmn_DMesgHandler &operator=(Dmn_DMesgHandler &&obj) = delete;

    /**
     * @brief The method returns yes or not if the handler is in conflict
     *
     * @return True or False if the handler is in conflict state from last
     *         written message
     */
    auto isInConflict() -> bool;

    /**
     * @brief The method returns running counter of the topic.
     *
     * @param topic The topic
     *
     * @return The running counter of the topic
     */
    auto getTopicRunningCounter(std::string_view topic) -> unsigned long;

    /**
     * @brief The method sets running counter of the topic.
     *
     * @param topic The topic
     * @param runningCounter The running counter to be set for the topic
     */
    void setTopicRunningCounter(std::string_view topic,
                                unsigned long runningCounter);

    /**
     * @brief Blocking read: return the next available DMesgPb or nullopt if
     *        an exception occurs.
     */
    auto read() -> std::optional<dmn::DMesgPb> override;

    /**
     * @brief Mark the handler's conflict as resolved. This posts an async task
     *        to the publisher's singleton async thread to clear the handler's
     *        conflict state (so the handler need not manage mutexes itself).
     */
    void resolveConflict();

    /**
     * @brief Set a callback to be invoked when the handler enters a conflict
     *        state.
     *
     * @param conflict_fn Callback receiving the handler and the message that
     *                    caused the conflict.
     */
    void setConflictCallbackTask(ConflictCallbackTask conflict_fn);

    /**
     * @brief Publish the provided DMesgPb by moving it into the publisher
     *        queue (efficient path).
     *
     * @param dmesgpb Message to publish (moved).
     */
    void write(dmn::DMesgPb &&dmesgpb) override;

    /**
     * @brief Publish the provided DMesgPb by copying it into the publisher
     *        queue.
     *
     * @param dmesgpb Message to publish (copied).
     */
    void write(dmn::DMesgPb &dmesgpb) override;

    /**
     * @brief Publish the message and return true if no conflict, or false
     *        otherwise.
     *
     * @param dmesgpb message to be publish (moved).
     */
    auto writeAndCheckConflict(dmn::DMesgPb &&dmesgpb) -> bool;

    /**
     * @brief Publish the message and return true if no conflict, or false
     *        otherwise.
     *
     * @param dmesgpb message to be publish (copied).
     */
    auto writeAndCheckConflict(dmn::DMesgPb &dmesgpb) -> bool;

    /**
     * @brief Block until there are no pending asynchronous tasks for this
     *        handler (for example, pending notify/async-process tasks).
     */
    void waitForEmpty();

    friend class Dmn_DMesg;
    friend class Dmn_DMesgHandlerSub;

  protected:
    /**
     * @brief The method returns running counter of the topic.
     *
     * @param topic The topic
     *
     * @return The running counter of the topic
     */
    auto getTopicRunningCounterInternal(std::string_view topic)
        -> unsigned long;

    /**
     * @brief The method sets running counter of the topic.
     *
     * @param topic The topic
     * @param runningCounter The running counter to be set for the topic
     */
    void setTopicRunningCounterInternal(std::string_view topic,
                                        unsigned long runningCounter);

    /**
     * @brief Internal write implementation used by public write(...) overloads.
     *
     * @param dmesgPb The message to publish.
     * @param move    If true, move the message; otherwise copy.
     */
    void writeDMesgInternal(dmn::DMesgPb &dmesgpb, bool move);

  private:
    /**
     * @brief Return true if the handler is currently marked in a conflict
     *        state (atomic check).
     */
    auto isInConflictInternal() const -> bool;

    /**
     * @brief Internal helper to mark the handler as resolved. Must be called
     *        in the publisher's async thread context.
     */
    void resolveConflictInternal();

    /**
     * @brief Put the handler into conflict state and schedule the conflict
     *        callback on the publisher's async thread.
     *
     * @param dmesgpb The message that caused the conflict.
     */
    void throwConflict(const dmn::DMesgPb &dmesgpb);

    /**
     * Data members set during construction.
     */
    std::string m_name{};
    std::string m_topic{};
    FilterTask m_filter_fn{};
    AsyncProcessTask m_async_process_fn{};
    HandlerConfig m_configs{};

    /**
     * Internal state flags derived from configuration.
     */
    bool m_include_dmesgpb_sys{};
    bool m_no_topic_filter{};

    Dmn_DMesg *m_owner{};
    Dmn_DMesgHandlerSub m_sub{};

    Dmn_Buffer<dmn::DMesgPb> m_buffers{};
    dmn::DMesgPb m_last_dmesgpb_sys{};
    std::unordered_map<std::string, uint64_t> m_topic_running_counter{};

    ConflictCallbackTask m_conflict_callback_fn{};
    std::atomic<bool> m_in_conflict{};

    // Set true after the handler has received the initial playback of
    // last-known messages for each topic.
    bool m_after_initial_playback{};
  }; // class Dmn_DMesgHandler

  /**
   * @brief Construct a Dmn_DMesg publisher instance.
   *
   * @param name Identification name for this DMesg instance.
   */
  Dmn_DMesg(std::string_view name);
  virtual ~Dmn_DMesg() noexcept;

  Dmn_DMesg(const Dmn_DMesg &obj) = delete;
  const Dmn_DMesg &operator=(const Dmn_DMesg &obj) = delete;
  Dmn_DMesg(Dmn_DMesg &&obj) = delete;
  Dmn_DMesg &operator=(Dmn_DMesg &&obj) = delete;

  /**
   * @brief Create, register and return a new Dmn_DMesgHandler.
   *
   * This template forwards its arguments to the Dmn_DMesgHandler constructor.
   * Registration and the initial playback of last-known messages are performed
   * asynchronously in the publisher's singleton async context so that handler
   * construction remains lock-free for fast paths.
   *
   * @return shared_ptr to the newly created handler.
   */
  template <class... U>
  std::shared_ptr<Dmn_DMesgHandler> openHandler(U &&...arg);

  /**
   * @brief Unregister and free the provided handler.
   *
   * @param handlerToClose Shared pointer reference to the handler to close.
   *                       The handler will be removed from internal lists and
   *                       the shared_ptr will be reset by the caller.
   */
  void closeHandler(std::shared_ptr<Dmn_DMesgHandler> &handlerToClose);

protected:
  using Dmn_Pub::publish;

  /**
   * @brief Publish a system message via the async context.
   *
   * @param dmesgpb_sys The system message to publish.
   */
  void publishSysInternal(const dmn::DMesgPb &dmesgpb_sys);

  /**
   * @brief Publish a normal DMesgPb to subscribers.
   *
   * Conflict detection:
   *  - If the incoming message's topic running counter is older than the
   *    publisher's recorded counter for that topic, it indicates the writer
   *    is out-of-sync. In that case, only the writer's handler is placed into
   *    a conflict state; its future writes will be rejected until the client
   *    resolves the conflict.
   *
   * @param dmesgPb The message to be published.
   */
  void publishInternal(const dmn::DMesgPb &dmesgPb) override;

  /**
   * @brief Post an async action that resets a handler's conflict state in the
   *        publisher's singleton async thread context.
   *
   * @param handler_ptr Pointer to the handler whose conflict state should be
   *                    reset.
   */
  void resetHandlerConflictState(const Dmn_DMesgHandler *handler_ptr);

private:
  /**
   * @brief Run in the publisher's async thread context to playback the last
   *        message for each topic to newly registered handlers.
   */
  void playbackLastTopicDMesgPbInternal();

  /**
   * @brief Internal helper that resets handler conflict state. Must be
   *        executed in the publisher's async thread context.
   *
   * @param handler_ptr Pointer to the handler.
   */
  void resetHandlerConflictStateInternal(const Dmn_DMesgHandler *handler_ptr);

  /**
   * Data members provided at construction.
   */
  std::string m_name{};

  /**
   * Internal state:
   *  - list of active handlers
   *  - per-topic running counters
   *  - last published message per topic
   */
  std::vector<std::shared_ptr<Dmn_DMesgHandler>> m_handlers{};
  std::unordered_map<std::string, uint64_t> m_topic_running_counter{};
  std::unordered_map<std::string, dmn::DMesgPb> m_topic_last_dmesgpb{};
}; // class Dmn_DMesg

template <class... U>
std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler>
Dmn_DMesg::openHandler(U &&...arg) {
  // This function:
  //  - constructs a handler
  //  - registers the handler as a subscriber
  //  - wires handler<->subscriber<->publisher links
  //  - schedules an async task on the publisher's singleton async thread to:
  //      * add the handler to the internal list
  //      * playback last-known messages per topic
  //      * mark handler as initialized after playback
  //
  // The use of the publisher's singleton async context keeps most operations
  // mutex-free for the hot paths (publish/notify).

  std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> handler =
      std::make_shared<Dmn_DMesg::Dmn_DMesgHandler>(std::forward<U>(arg)...);

  /* The topic filter is executed within the DMesg singleton asynchronous
   * thread context, but the filter value is maintained per Dmn_DMesgHandler.
   * This design keeps DMesg itself mutex-free while remaining thread safe.
   */
  handler->m_owner = this;

  this->registerSubscriber(&(handler->m_sub));

  auto handler_ret = handler;

  DMN_ASYNC_CALL_WITH_CAPTURE(
      {
        this->m_handlers.push_back(handler);
        this->playbackLastTopicDMesgPbInternal();
        handler->m_after_initial_playback = true;
      },
      this, handler);

  return handler_ret;
}

} // namespace dmn

#endif // DMN_DMESG_HPP_
