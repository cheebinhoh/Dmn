/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-dmesg.hpp
 * @brief DMESG publisher/subscriber wrapper using Protobuf messages.
 *
 * Overview
 * --------
 * This header declares Dmn_DMesg, a publisher built on top of Dmn_Pub that
 * exchanges messages using the generated Protobuf type `dmn::DMesgPb`
 * (proto/dmn-dmesg.proto). It also declares Dmn_DMesg::Dmn_DMesgHandler, a
 * light-weight, IO-style handler that client code holds (via std::shared_ptr)
 * to publish and consume DMesgPb messages.
 *
 * Design pattern
 * --------------
 * Proxy    - Dmn_DMesgHandlerProxy provides a lightweight proxy to
 *            Dmn_DMesgHandler instances, offering pointer-like access while
 *            allowing the publisher to control and own handler lifetime.
 * Composite - Dmn_DMesgNet builds network I/O on top of Dmn_DMesg, which uses
 *             the Dmn_Pub/Dmn_Sub publish-subscribe hierarchy.
 *
 * Key responsibilities
 * - Represent messages with the Protobuf type `dmn::DMesgPb`. Clients extend
 *   the proto when extra fields are needed (no C++ subclassing required).
 * - Provide an ergonomic handler API (Dmn_Io-like) for reading and writing
 *   DMesgPb messages without requiring clients to subclass publisher-specific
 *   subscriber interfaces.
 * - Maintain per-topic running counters and last-known messages so new
 *   handlers can receive the most recent state for every topic (playback).
 * - Detect simple publish conflicts based on per-topic running counters,
 *   marking the writer and eligible subscribed handlers as conflicted.
 *
 * Handler model and behaviour
 * - Dmn_DMesgHandler derives from Dmn_Pub::Dmn_Sub and registers with the
 *   Dmn_Pub notification system.
 * - Handlers can:
 *     * subscribe to a specific topic (empty topic is permitted),
 *     * provide an optional filter functor to drop unwanted messages,
 *     * provide an optional async-process functor to process messages as they
 *       arrive, and
 *     * be configured via HandlerConfig options to control behaviour.
 * - System messages (where DMesgPb.message_type == "sys") may be delivered
 *   selectively based on handler configuration.
 * - The handler API offers blocking reads, non-blocking/asynchronous delivery,
 *   write variants (move/copy), and helpers to check and resolve conflict
 *   state.
 *
 * Concurrency and async model
 * - Handler registration, publisher notifications, and conflict-state resets
 *   are serialized on the publisher's asynchronous context.
 * - Handler filters run synchronously on the publisher context; accepted
 *   message-processing, handler-event, and conflict callbacks run serially on
 *   the handler's async context in publisher event order.
 * - A filter must not synchronously wait on or re-enter the publisher context.
 *   Handler callbacks must not synchronously wait on either context, re-enter
 *   the same handler, or close the handler from within their callback.
 * - The publisher owns authoritative per-handler counter/conflict snapshots
 *   in its serialized context; handlers apply ordered snapshots to their
 *   handler-local state on their own async context.
 * - Publishing and notification follow Dmn_Pub semantics; the DMESG wrapper
 *   implements additional topic/counter logic and schedules async tasks where
 *   appropriate.
 *
 * Conflict detection summary
 * - Each topic has an associated running counter tracked by the publisher.
 * - If a handler publishes a message with a topic counter older than the
 *   publisher's current counter for that topic, the writer is placed into
 *   conflict. Subscribed handlers with an established counter for that topic
 *   can also enter conflict when processing the conflict notification.
 * - While in conflict, a handler's writes may be rejected until the client
 *   resolves the conflict (resolveConflict()). Conflict callbacks can be
 *   installed to notify clients when a conflict occurs.
 *
 * Configuration and constants
 * - HandlerConfig keys (Handler_IncludeSys, Handler_NoTopicFilter) are used
 *   to control handler behaviour; defaults are provided in
 *   kHandlerConfig_Default.
 *
 * Design notes
 * - Dmn_DMesgHandler keeps per-handler buffers, a mirror of per-topic counters
 *   and conflict state, and a small internal subscriber object. Dmn_DMesg
 *   stores publisher-owned per-handler snapshots alongside its global
 *   per-topic counters and last-published-message cache.
 * - Publisher state is serialized by its async context. Public handler state
 *   access and callbacks are serialized by that handler's async context; state
 *   crosses the boundary only as ordered snapshots.
 *
 * See also
 * - proto/dmn-dmesg.proto : Protobuf definition for dmn::DMesgPb.
 * - dmn-pub-sub.hpp       : Base publisher/subscriber primitives used here.
 */

#ifndef DMN_DMESG_HPP_
#define DMN_DMESG_HPP_

#include "dmn-pub-sub.hpp"

#include "proto/dmn-dmesg.pb.h"

#include <atomic>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace dmn {

class Dmn_DMesgHandler;

/**
 * @brief Identifier string for the sys topic (used by DMesgNet).
 */
extern const char *const kDMesgSysIdentifier;

class Dmn_DMesg : public Dmn_Pub<dmn::DMesgPb> {
public:
  /**
   * @brief Callback invoked in the handler's async context with a copy of each
   * delivered message. It runs serially with other handler-context work; do not
   * synchronously wait on either async context, re-enter this handler, or close
   * it from within the callback.
   */
  using AsyncProcessTask = std::function<void(dmn::DMesgPb)>;

  /**
   * @brief Predicate that returns false to drop an incoming message before
   * handler delivery.
   */
  using FilterTask = std::function<bool(const dmn::DMesgPb &)>;

  enum class HandlerEventType {
    kMessage,
    kConflictEntered,
    kConflictResolved,
  };

  /**
   * @brief A publisher-ordered handler delivery or conflict-state transition.
   */
  struct HandlerEvent {
    HandlerEventType m_type{};
    std::string m_topic{};
    uint64_t m_handler_running_counter{};
    uint64_t m_conflict_generation{};
    std::optional<dmn::DMesgPb> m_message{};
  };

  /**
   * @brief Observer invoked in handler context in publisher event order.
   *
   * Implementations should enqueue bounded work and must not block or call
   * back into the publisher. Do not synchronously wait on either async
   * context, re-enter this handler, or close it from within the callback.
   */
  using HandlerEventCallbackTask =
      std::function<void(const HandlerEvent &event)>;

  /**
   * @brief Key/value map used to pass per-handler configuration options.
   */
  using HandlerConfig = std::unordered_map<std::string, std::string>;

  class Dmn_DMesgHandler;

  /**
   * @brief Normalized constructor inputs for a standard or derived handler.
   */
  struct HandlerSpec {
    std::string m_name{};
    std::string m_topic{};
    FilterTask m_filter_fn{};
    AsyncProcessTask m_async_process_fn{};
    HandlerConfig m_configs{};
    HandlerEventCallbackTask m_handler_event_fn{};

    HandlerSpec() = default;
    HandlerSpec(std::string_view name, std::string_view topic,
                FilterTask filter_fn = {},
                AsyncProcessTask async_process_fn = {},
                HandlerConfig configs = {},
                HandlerEventCallbackTask handler_event_fn = {})
        : m_name{name}, m_topic{topic}, m_filter_fn{std::move(filter_fn)},
          m_async_process_fn{std::move(async_process_fn)},
          m_configs{std::move(configs)},
          m_handler_event_fn{std::move(handler_event_fn)} {}
  };

  using HandlerFactory =
      std::function<std::shared_ptr<Dmn_DMesgHandler>(const HandlerSpec &)>;

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
   * receive system messages (DMesgPb.message_type == "sys").
   */
  static constexpr std::string_view kHandlerConfig_IncludeSys =
      "Handler_IncludeSys";

  /**
   * @brief If set to "yes" or "1", handlers opened with this config will
   * ignore topic filtering when reading: read() returns messages regardless of
   * topic value and write() will not set the message topic automatically.
   */
  static constexpr std::string_view kHandlerConfig_NoTopicFilter =
      "Handler_NoTopicFilter";

  /**
   * @brief Type used to represent key/value configuration entries for the
   * Dmn_DMesg object itself.
   */
  using KeyValueConfiguration = std::unordered_map<std::string, std::string>;

  /**
   * @brief Dmn_DMesgHandler is an IO-style interface for clients to publish
   * and consume dmn::DMesgPb messages. It implements Dmn_Io<DMesgPb>.
   *
   * The handler composes a small nested subscriber object (Dmn_DMesgHandlerSub)
   * that integrates with the underlying Dmn_Pub infrastructure. The handler
   * maintains its own filters, async processing callback, per-topic running
   * counters, and a small buffer for arriving messages.
   *
   * Clients obtain handlers from Dmn_DMesg::openHandler(...) and release them
   * via Dmn_DMesg::closeHandler(...).
   */
  class Dmn_DMesgHandler : public Dmn_Io<dmn::DMesgPb>,
                           public dmn::Dmn_Pub<dmn::DMesgPb>::Dmn_Sub,
                           private Dmn_Async<Dmn_BlockingQueue_Mt> {
  private:
    using ConflictCallbackTask =
        std::function<void(Dmn_DMesgHandler &handler, const dmn::DMesgPb &)>;

  public:
    enum WriteOptions { kDefault = 0, kBlock, kForce, kMaxWriteOptions };
    using WriteFlags = std::bitset<kMaxWriteOptions>;

    using Dmn_Io<dmn::DMesgPb>::write;

    /**
     * @brief Construct a handler that subscribes to a specific topic and
     * optionally provides filter and async-process callbacks.
     *
     * @param name             Unique name/identifier for the handler.
     * @param topic            Topic string to subscribe/publish to.
     * @param filter_fn        Optional filter functor: return false exclude
     *                         message; runs synchronously on the publisher
     *                         context.
     * @param async_process_fn Optional functor to process accepted messages on
     *                         the handler's async context.
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
     * using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, std::string_view topic,
                     FilterTask filter_fn);

    /**
     * @brief Construct a handler with topic only (no filter, no async fn),
     * using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, std::string_view topic);

    /**
     * @brief Construct a handler that subscribes to the empty topic with
     * filter and async-process callbacks and a custom configuration.
     */
    Dmn_DMesgHandler(std::string_view name, FilterTask filter_fn,
                     AsyncProcessTask async_process_fn, HandlerConfig configs);

    /**
     * @brief Construct a handler that subscribes to the empty topic with
     * filter and async-process callbacks using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, FilterTask filter_fn,
                     AsyncProcessTask async_process_fn);

    /**
     * @brief Construct a handler that subscribes to the empty topic with
     * filter only, using default configuration.
     */
    Dmn_DMesgHandler(std::string_view name, FilterTask filter_fn);

    /**
     * @brief Construct a handler with only a name; subscribes to the empty
     * topic with default behaviour.
     */
    explicit Dmn_DMesgHandler(std::string_view name);

    virtual ~Dmn_DMesgHandler() noexcept;

    Dmn_DMesgHandler(const Dmn_DMesgHandler &obj) = delete;
    Dmn_DMesgHandler &operator=(const Dmn_DMesgHandler &obj) = delete;
    Dmn_DMesgHandler(Dmn_DMesgHandler &&obj) = delete;
    Dmn_DMesgHandler &operator=(Dmn_DMesgHandler &&obj) = delete;

    /**
     * @brief Check whether this handler is currently in a conflict state.
     *
     * @param topic The topic to check, or an empty string to check any topic.
     *
     * @return @c true if the handler is in conflict for the given topic (or for
     * any topic when @p topic is empty), @c false otherwise.
     */
    auto isInConflict(std::string_view topic = "") -> bool;

    /**
     * @brief Return the current running counter for the given topic.
     *
     * @param topic The topic whose running counter is queried.
     *
     * @return The running counter value for @p topic.
     */
    auto getTopicRunningCounter(std::string_view topic) -> uint64_t;

    /**
     * @brief Set the running counter for the given topic.
     *
     * @param topic          The topic whose running counter is to be updated.
     * @param runningCounter The new counter value to assign.
     */
    void setTopicRunningCounter(std::string_view topic,
                                uint64_t runningCounter);

    /**
     * @brief Blocking read: return the next available DMesgPb or nullopt if
     * the read fails / on shutdown.
     */
    auto read() -> std::optional<dmn::DMesgPb> override;

    /**
     * @brief Mark the handler's conflict as resolved. This posts the state
     * change to the publisher context and waits until its snapshot has been
     * applied on the handler context.
     */
    void resolveConflict(std::string_view topic = "");

    /**
     * @brief Set a callback to be invoked when the handler enters a conflict
     * state, in the handler's async context.
     *
     * @param conflict_fn Callback receiving the handler and the message that
     *                    caused the conflict. Do not synchronously call an
     *                    API that waits on either async context, re-enter
     *                    the handler, or close it from within the callback.
     */
    void setConflictCallbackTask(ConflictCallbackTask conflict_fn);

    /**
     * @brief Publish the provided DMesgPb by moving it into the publisher
     * queue (efficient path).
     *
     * @param dmesgpb Message to publish (moved).
     */
    void write(dmn::DMesgPb &&dmesgpb) override;

    /**
     * @brief Publish the provided DMesgPb by copying it into the publisher
     * queue.
     *
     * @param dmesgpb Message to publish (copied).
     */
    void write(const dmn::DMesgPb &dmesgpb) override;

    /**
     * @brief Publish the provided DMesgPb by moving it into the publisher
     * queue (efficient path).
     *
     * @param dmesgpb Message to publish (moved).
     * @param flags Write options: Block waiting for publisher to process the
     * message, Force to force message without conflict check.
     */
    void write(dmn::DMesgPb &&dmesgpb, WriteFlags flags);

    /**
     * @brief Publish the provided DMesgPb by copying it into the publisher
     * queue, it directs the call to const argument write version.
     *
     * @param dmesgpb Message to publish (copied).
     * @param flags Write options: Block waiting for publisher to process the
     * message, Force to force message without conflict check.
     */
    void write(dmn::DMesgPb &dmesgpb, WriteFlags flags);

    /**
     * @brief Publish the provided DMesgPb by copying it into the publisher
     * queue.
     *
     * @param dmesgpb Message to publish (copied).
     * @param flags Write options: Block waiting for publisher to process the
     * message, Force to force message without conflict check.
     */
    void write(const dmn::DMesgPb &dmesgpb, WriteFlags flags);

    /**
     * @brief Publish the message and return true if no conflict, or false
     * otherwise.
     *
     * @param dmesgpb message to be publish (moved).
     * @param flags Write options: Block waiting for publisher to process the
     * message, Force to force message without conflict check.
     *
     * @return True if write success without conflict or false otherwise.
     */
    auto writeAndCheckConflict(dmn::DMesgPb &&dmesgpb,
                               WriteFlags flags = kDefault) -> bool;

    /**
     * @brief Publish the message and return true if no conflict, or false
     * otherwise.
     *
     * @param dmesgpb message to be publish (copied).
     * @param flags Write options: Block waiting for publisher to process the
     * message, Force to force message without conflict check.
     *
     * @return True if write success without conflict or false otherwise.
     */
    auto writeAndCheckConflict(dmn::DMesgPb &dmesgpb,
                               WriteFlags flags = kDefault) -> bool;

    /**
     * @brief Called by the publisher to notify this subscriber of a new
     * DMesgPb message. Runs synchronously on the publisher's async context.
     *
     * Behavior summary:
     *  - Messages published by the same handler are skipped (handler does
     *    not re-receive its own writes), except system messages which can be
     *    delivered based on handler configuration.
     *  - Messages with a running counter older than the handler's last seen
     *    counter for the topic are skipped (out-of-order / stale).
     *  - If configured, system messages can be queued for read() or passed
     *    to m_async_process_fn for handling in this handler's async context.
     *  - The filter runs synchronously here on the publisher context.
     *    Accepted-message, handler-event, and conflict callbacks are queued to
     *    this handler's async context in publisher order.
     *
     * @param dmesgPb The message delivered by the publisher.
     */
    void notify(const dmn::DMesgPb &dmesgpb,
                Dmn_Pub<dmn::DMesgPb> *pub) override;

    friend class Dmn_DMesg;

  protected:
    /**
     * @brief Return the running counter for the given topic (internal helper).
     *
     * @param topic The topic whose running counter is queried.
     *
     * @return The running counter value for @p topic.
     */
    auto getTopicRunningCounterInternal(std::string_view topic) -> uint64_t;

    /**
     * @brief Set the running counter for the given topic (internal helper).
     *
     * @param topic          The topic whose running counter is to be updated.
     * @param runningCounter The new counter value to assign.
     */
    void setTopicRunningCounterInternal(std::string_view topic,
                                        uint64_t runningCounter);

    /**
     * @brief Internal write implementation used by public write(...) overloads.
     *
     * @param dmesgPb The message to publish.
     * @param move    If true, move the message; otherwise copy.
     * @param block   Block waiting for the publisher to process the message.
     */
    void writeDMesgInternal(dmn::DMesgPb &dmesgpb, bool move,
                            bool block = false);

    /**
     * @brief Schedule a callable task to be executed within the handler's
     * asynchronous execution context.
     *
     * @tparam Callable The type of the callable object (e.g., lambda,
     * function).
     * @param  fnc      The callable task to be scheduled and executed.
     */
    template <typename Callable> void scheduleInHandlerContext(Callable &&fnc) {
      this->addExecTask(std::forward<Callable>(fnc));
    }

  private:
    /**
     * @brief Return true if the handler is currently marked in a conflict
     * state on its async context.
     *
     * @param topic the topic to check if it is in conflict, or "" any topic.
     */
    auto isInConflictInternal(std::string_view topic) const -> bool;

    /**
     * @brief Pause and wait until initial playback has completed.
     */
    void isAfterInitialPlayback();

    void enqueuePublisherEvent(HandlerEvent event, bool notifyObserver,
                               bool invokeConflictCallback);

    /**
     * @brief Queue the playback-complete marker in handler context and wait
     * until preceding handler-context work has completed.
     */
    void setAfterInitialPlayback();

    /**
     * @brief Set the playback-complete flag and notify all blocking threads.
     *
     * This method must run in the handler's async context.
     */
    void setAfterInitialPlaybackInternal();

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

    std::unique_ptr<
        Dmn_BlockingQueue<Dmn_BlockingQueue_Mt<dmn::DMesgPb>, dmn::DMesgPb>>
        m_buffers{};
    // This is a handler-context mirror of publisher-owned state.
    std::unordered_map<std::string, uint64_t> m_topic_running_counter{};

    ConflictCallbackTask m_conflict_callback_fn{};
    std::set<std::string> m_topic_in_conflict{};
    HandlerEventCallbackTask m_handler_event_fn{};
    uint64_t m_conflict_generation{};

    // Set true after the handler has received the initial playback of
    // last-known messages for each topic.
    std::atomic_flag m_after_initial_playback{};
  }; // class Dmn_DMesgHandler

  /**
   * @brief Lightweight proxy to a Dmn_DMesgHandler instance.
   *
   * Wraps a std::weak_ptr<Dmn_DMesgHandler> to provide pointer-like
   * access to the underlying handler while allowing the publisher to owns
   * and control handler lifetime (via closeHandler()). Clients should test
   * the proxy's bool conversion before dereferencing to avoid exceptions,
   * otherwise an exception is thrown if the underlying handler has released.
   *
   * Thread-safety: The proxy itself is not thread-safe; do not share a
   * single Dmn_DMesgHandlerProxy instance across threads.
   */
  class Dmn_DMesgHandlerProxy {
    friend class Dmn_DMesg;

  public:
    /**
     * @brief Dereference the proxy to access the underlying handler shared
     * pointer.
     *
     * @return shared_ptr<Dmn_DMesgHandler> pointing to the live handler.
     *
     * @throws std::runtime_error if the handler has already been closed.
     */
    std::shared_ptr<Dmn_DMesgHandler> operator->() const {
      auto handler = m_handler.lock();
      if (!handler) {
        throw std::runtime_error("handler has been closed");
      }

      return handler;
    }

    /**
     * @brief Check whether the proxied handler is still alive.
     *
     * @return true if the underlying handler exists and has not been closed,
     * false if it has been released.
     */
    explicit operator bool() const noexcept { return !m_handler.expired(); }

  private:
    void reset() { m_handler.reset(); }

    std::weak_ptr<Dmn_DMesgHandler> m_handler{};
  };

  using HandlerType = Dmn_DMesgHandlerProxy;

  /**
   * @brief Construct a Dmn_DMesg publisher instance.
   *
   * @param name Identification name for this DMesg instance.
   */
  explicit Dmn_DMesg(std::string_view name);

  /**
   * @brief Unregister handlers and drain queued publisher work before teardown.
   *
   * Callers must stop and join threads that may use this publisher before
   * destruction begins. The derived destructor performs handler unregistration
   * and drains the async context while Dmn_DMesg state is still alive.
   */
  virtual ~Dmn_DMesg() noexcept;

  Dmn_DMesg(const Dmn_DMesg &obj) = delete;
  Dmn_DMesg &operator=(const Dmn_DMesg &obj) = delete;
  Dmn_DMesg(Dmn_DMesg &&obj) = delete;
  Dmn_DMesg &operator=(Dmn_DMesg &&obj) = delete;

  /**
   * @brief Create, register and return a new Dmn_DMesgHandler.
   *
   * This template forwards its arguments to the Dmn_DMesgHandler constructor.
   * Registration and the initial playback of last-known messages are performed
   * asynchronously in the publisher's serialized async context so that handler
   * construction remains lock-free for fast paths.
   *
   * @return the handler proxy to internal shared_ptr handler registered
   * with DMesg.
   */
  template <class... U>
    requires(sizeof...(U) != 1 ||
             (!std::is_same_v<std::remove_cvref_t<U>, HandlerSpec> && ...))
  auto openHandler(U &&...arg) -> HandlerType;

  /**
   * @brief Open a standard handler from normalized inputs, including its
   * optional publisher-ordered event observer.
   */
  auto openHandler(const HandlerSpec &spec) -> HandlerType;

  /**
   * @brief Open a handler using a normalized spec and factory for derived
   * types.
   */
  auto openHandlerWithFactory(const HandlerSpec &spec,
                              const HandlerFactory &factory) -> HandlerType;

  /**
   * @brief Unregister and free the provided handler.
   *
   * @param handlerToClose the internal handler will be reset upon return from
   * this method.
   */
  void closeHandler(HandlerType &handlerToClose);

  /**
   * @brief Get the topic last message or nullptr if no message for the topic.
   *
   * @param topic the topic that last message to be returned.
   *
   * @return last message of topic or nullptr if no message for such topic.
   */
  auto
  getTopicLastMessage(std::string_view topic) -> std::optional<dmn::DMesgPb>;

  /**
   * @brief Reset conflict state by posting last message of the topic.
   *
   * @param topic Topic to reset
   */
  void resetConflictStateWithLastTopicMessage(std::string_view topic);

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
   * @brief Return the cache of last topic message.
   *
   * @return The cache of last topic messages.
   */
  virtual auto getLastTopicCacheInternal()
      -> std::unordered_map<std::string, dmn::DMesgPb> &;

  /**
   * @brief Post an async action that resets a handler's conflict state in the
   * publisher's singleton async thread context.
   *
   * @param handler_ptr Pointer to the handler whose conflict state should be
   * reset.
   */
  void resetHandlerConflictState(const Dmn_DMesgHandler *handler_ptr,
                                 std::string_view = "");

private:
  /**
   * @brief Internal helper to finalize handler wiring and registration.
   */
  auto finalizeHandlerRegistration(std::shared_ptr<Dmn_DMesgHandler> handler)
      -> HandlerType;

  /**
   * @brief Run in the publisher's async thread context to playback the last
   * message for each topic to newly registered handlers.
   */
  void playbackLastTopicDMesgPbInternal();

  /**
   * @brief Reset conflict state by posting last message of the topic.
   *
   * @param topic Topic to reset
   */
  void resetConflictStateWithLastTopicMessageInternal(std::string_view topic);

  /**
   * @brief Internal helper that resets handler conflict state. Must be
   * executed in the publisher's async thread context.
   *
   * @param handler_ptr Pointer to the handler.
   */
  void resetHandlerConflictStateInternal(const Dmn_DMesgHandler *handler_ptr,
                                         std::string_view = "");

  struct HandlerState {
    std::unordered_map<std::string, uint64_t> m_topic_running_counter{};
    std::set<std::string> m_topic_in_conflict{};
    uint64_t m_conflict_generation{};
  };

  void setHandlerTopicRunningCounter(const Dmn_DMesgHandler *handler_ptr,
                                     std::string_view topic,
                                     uint64_t runningCounter);
  void throwHandlerConflict(const Dmn_DMesgHandler *handler_ptr,
                            const dmn::DMesgPb &message);
  void resolveHandlerConflict(const Dmn_DMesgHandler *handler_ptr,
                              std::string_view topic,
                              const dmn::DMesgPb *message = nullptr);
  void enqueueHandlerEvent(const Dmn_DMesgHandler *handler_ptr,
                           HandlerEventType type, std::string_view topic,
                           const dmn::DMesgPb *message, bool notifyObserver,
                           bool invokeConflictCallback);

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
  std::unordered_map<const Dmn_DMesgHandler *, HandlerState> m_handler_states{};
  std::unordered_map<std::string, uint64_t> m_topic_running_counter{};
  std::unordered_map<std::string, dmn::DMesgPb> m_topic_last_dmesgpb{};
}; // class Dmn_DMesg

inline auto Dmn_DMesg::finalizeHandlerRegistration(
    std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> handler) -> HandlerType {
  auto handlerProxy = Dmn_DMesg::Dmn_DMesgHandlerProxy();

  handler->m_owner = this;
  this->registerSubscriber(handler);
  handlerProxy.m_handler = handler;

  // Registration, playback, and the handler-context barrier run in order.
  auto waitHandler = this->addExecTaskWithWait([this, handler]() {
    this->m_handlers.push_back(handler);
    this->m_handler_states.try_emplace(handler.get());
    this->playbackLastTopicDMesgPbInternal();
    handler->setAfterInitialPlayback();
  });

  waitHandler->wait();

  return handlerProxy;
}

template <class... U>
  requires(sizeof...(U) != 1 ||
           (!std::is_same_v<std::remove_cvref_t<U>, Dmn_DMesg::HandlerSpec> &&
            ...))
auto Dmn_DMesg::openHandler(U &&...arg) -> HandlerType {
  // This function:
  //  - constructs a handler
  //  - registers the handler as a subscriber
  //  - wires handler<->subscriber<->publisher links
  //  - schedules an async task on the publisher's singleton async thread to:
  //      * add the handler and initialize its publisher-owned state snapshot
  //      * playback last-known messages per topic
  //      * wait for the handler context to finish initial-playback work and
  //        mark the handler as initialized
  //
  // Publisher-owned handler snapshots and registration/playback are
  // serialized on the publisher context. Each handler receives snapshots on
  // its own async context for public state access and callbacks.

  std::shared_ptr<Dmn_DMesg::Dmn_DMesgHandler> handler =
      std::make_shared<Dmn_DMesg::Dmn_DMesgHandler>(std::forward<U>(arg)...);

  return finalizeHandlerRegistration(handler);
}

inline auto Dmn_DMesg::openHandler(const HandlerSpec &spec) -> HandlerType {
  return openHandlerWithFactory(spec, [](const HandlerSpec &handlerSpec) {
    return std::make_shared<Dmn_DMesgHandler>(
        handlerSpec.m_name, handlerSpec.m_topic, handlerSpec.m_filter_fn,
        handlerSpec.m_async_process_fn, handlerSpec.m_configs);
  });
}

inline auto Dmn_DMesg::openHandlerWithFactory(
    const HandlerSpec &spec, const HandlerFactory &factory) -> HandlerType {
  auto handler = factory(spec);
  if (!handler) {
    throw std::runtime_error("handler factory produced a null handler");
  }

  // Install the observer before registration can trigger initial playback.
  handler->m_handler_event_fn = spec.m_handler_event_fn;

  return finalizeHandlerRegistration(handler);
}

} // namespace dmn

#endif // DMN_DMESG_HPP_
