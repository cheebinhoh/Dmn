/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * The DMesg implements a specific publisher subscriber model (inherit
 * from dmn-pub-sub module) where the difference is that:
 * - the data item is a Protobuf message (dmn::DMesgPb) defined in
 *   proto/dmn-dmesg.proto, so instead of subclassing the DMesg, clients
 *   can extend the dmn::DMesgPb protobuf message to varying the
 *   message without reinvent the wheel through subclass of the dmn-pub-sub
 *   module.
 * - instead of subclass Pub::Sub class to implement specific
 *   subscriber, the client of the API asks DMesg to return a handler that
 *   subscribes to a specific set of topic (optional, or all topics), and can
 *   use to handler to publish and subscribe dmn::DMesgPb message through the
 *   Io like interface' read and write API methods, so instead of
 *   inherittence from DMesg, clients can use object composition with it.
 * - it supports the concept that subscriber can subscribe to certain topic
 *   as defined in the dmn::DMesgPb message.
 * - it allows various clients of the Dmesg to publish data of certain
 *   topic at the same time, and implements a simple conflict detection and
 *   resolution for participated clients of the same Dmesg object, DMesg
 *   object takes care of synchronizing message between publisher and all
 *   subscribers of the same DMesg object.
 */

#ifndef DMN_DMESG_HPP_

#define DMN_DMESG_HPP_

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

#include "dmn-pub-sub.hpp"

#include "proto/dmn-dmesg.pb.h"

namespace dmn {

extern const char *kDMesgSysIdentifier;

class DMesg : public Pub<dmn::DMesgPb> {
public:
  using FilterTask = std::function<bool(const dmn::DMesgPb &)>;
  using AsyncProcessTask = std::function<void(dmn::DMesgPb)>;

  /**
   * @brief The key (std::string) and value (std::string) for DMesg
   *        configuration.
   */
  using KeyValueConfiguration = std::map<std::string, std::string>;

  /**
   * @brief The DMesgHandler is intentionalled modelled to inherit from
   *        Io that provides read/write IO interface across a range of
   *        IO, like socket, file, kafka, etc, and the DMesgHandler is
   *        just another IO, because of that we do NOT able to inherit from
   *        Pub as Pub inherits Async which inherits from Pipe
   *        which inherit Io with template type specialized to functor,
   *        this is a diamond shape multiple inheritance where common parent
   *        has to have same instantiated template type. Instead the class
   *        DMesgHandler uses a wrapper class DMesgHandlerSub which
   *        inherits from Pub<dmn::DMesgPb>::Sub.
   */
  class DMesgHandler : public Io<dmn::DMesgPb> {
  private:
    using ConflictCallbackTask =
        std::function<void(DMesgHandler &handler, const dmn::DMesgPb &)>;

    class DMesgHandlerSub : public dmn::Pub<dmn::DMesgPb>::Sub {
    public:
      DMesgHandlerSub() = default;
      virtual ~DMesgHandlerSub() noexcept;

      DMesgHandlerSub(const DMesgHandlerSub &obj) = delete;
      const DMesgHandlerSub &operator=(const DMesgHandlerSub &obj) = delete;
      DMesgHandlerSub(DMesgHandlerSub &&obj) = delete;
      DMesgHandlerSub &operator=(DMesgHandlerSub &&obj) = delete;

      /**
       * @brief The method is called by the dmn::Pub (publisher) object to
       *        notify the DMesgHandlerSub about the new DMesgPB message.
       *
       *        The DMesgHandlerSub will follow the following to process
       *        the message:
       *        - skip messages sent through the same DMesgHandler handler,
       *          unless it is a system message (of DMesgPb).
       *        - skip message which running counter is lower than running
       *          counter of the topic the DMesgHandlerSub has received.
       *        - store a copy of the message as last system message if it is.
       *        - if it is system message and the DMesgHandler (owner of
       *          the DMesgHandlerSub) is opened with m_include_dmesgsys
       *          as true, the DMesgPB message will be either pushed into
       *          the buffer waiting to be read through DMesgHandler' read
       *          or handling through m_async_process_fn callback.
       *
       * @param dmesgPb The DMesgPb message notified by publisher object
       */
      void notify(dmn::DMesgPb dmesgPb) override;

      // WARNING: it is marked as public so that a closure function
      // to DMesg can access and manipulate it, as there is no
      // direct way to declare an inline closure as friend (we can
      // define a function that accept object of type DMesgHandlerSub
      // class and then pass the object as capture to a closure
      // created within the function and returns the closure to
      // DMesg, but all the trouble to access this.
      //
      // But no external client will access the nested DMesgHandlerSub
      // class and it is just a composite object within the DMesgHandler
      // class to integrate with dmn-pub-sub through Pub::sub, so
      // marking m_owner as public does not violate data encapsulation.

      DMesgHandler *m_owner{};
    }; // class DMesgHandlerSub

  public:
    /**
     * @brief The delegating constructor for DMesgHandler.
     *
     * @param name           The name or unique identification to the handler
     * @param filterFn       The functor callback that returns false to filter
     * out DMesgPB message, if no functor is provided, no filter is performed
     * @param asyncProcessFn The functor callback to process each notified
     * DMesgPb message
     */
    DMesgHandler(std::string_view name, FilterTask filterFn = nullptr,
                 AsyncProcessTask asyncProcessFn = nullptr);

    /**
     * @brief The primitive constructor for DMesgHandler.
     *
     * @param name            The name or unique identification to the handler
     * @param includeDMesgSys True if the handler will be notified of DMesgPb
     *                        sys message, default is false
     * @param filterFn        The functor callback that returns false to filter
     *                        out DMesgPB message, if no functor is provided,
     *                        no filter is performed
     * @param asyncProcessFn  The functor callback to process each notified
     *                        DMesgPb message
     */
    DMesgHandler(std::string_view name, bool includeDMesgSys,
                 FilterTask filterFn, AsyncProcessTask asyncProcessFn);

    virtual ~DMesgHandler() noexcept;

    DMesgHandler(const DMesgHandler &obj) = delete;
    const DMesgHandler &operator=(const DMesgHandler &obj) = delete;
    DMesgHandler(DMesgHandler &&obj) = delete;
    DMesgHandler &operator=(DMesgHandler &&obj) = delete;

    /**
     * @brief The method reads a DMesgPb message out of the handler
     *        opened with DMesg. This is a blocking call until a DMesgPb
     *        message is available or exception is thrown, then nullopt
     *        is returned.
     *
     * @return DMesgPb The next DMesgPb message or nullopt if exception
     *                 is thrown.
     */
    std::optional<dmn::DMesgPb> read() override;

    /**
     * @brief The method marks the handler as conflict resolved by posting an
     *        asynchronous action on publisher singleton asynchronous thread
     *        context to reset the handler' context state.
     */
    void resolveConflict();

    /**
     * @brief The method set the callback function for conflict.
     *
     * @param cb The conflict callback function
     */
    void setConflictCallbackTask(ConflictCallbackTask conflictFn);

    /**
     * @brief The method writes and publishes the DMesgPb message through DMesg
     *        publisher queue to all subscribers. This method will move the
     *        DMesg data.
     *
     * @param dMesgPb The DMesgPb message to be published
     */
    void write(dmn::DMesgPb &&dmesgPb) override;

    /**
     * @brief The method writes and publishes the DMesgPb message through DMesg
     *        publisher queue to all subscribers. This method will copy the
     *        DMesg data.
     *
     * @param dMesgPb The DMesgPb message to be published
     */
    void write(dmn::DMesgPb &dmesgPb) override;

    /**
     * @brief The method returns true if the handler has NO pending to be
     * executed asynchronous jobs (example is notify methods).
     */
    void waitForEmpty();

    friend class DMesg;
    friend class DMesgHandlerSub;

  protected:
    /**
     * @brief The method writes and publishes the DMesgPb message through
     *        DMesg publisher queue to all subscribers. This method will
     *        move the DMesgPb message if move argument is true, otherwise
     *        copy the message.
     *
     * @param dmesgPb The DMesgPb messsgeto be published
     * @param move    True to move than copy the data
     */
    void writeDMesgInternal(dmn::DMesgPb &dmesgPb, bool move);

  private:
    /**
     * @brief The method returns true if the handler is in conflict state.
     *
     * @return True if the handler is in conflict state, false otherwise
     */
    bool isInConflict() const;

    /**
     * @brief The method marks the handler as conflict resolved, and to be
     *        executed in the publisher's singleton asynchronous thread
     *        context (to avoid the need of additional mutex).
     */
    void resolveConflictInternal();

    /**
     * @brief The method marks the handler as in conflict state and executes the
     *        conflict callback function in the handler singleton asynchronous
     *        thread context.
     *
     * @param mesgPb The dmesgPb data that results in conflict state
     */
    void throwConflict(const dmn::DMesgPb dmesgPb);

    /**
     * data member for constructor to instantiate the object.
     */
    std::string m_name{};
    bool m_include_dmesgsys{};
    FilterTask m_filter_fn{};
    AsyncProcessTask m_async_process_fn{};

    /**
     * data members for internal logic
     */
    DMesg *m_owner{};
    DMesgHandlerSub m_sub{};

    std::vector<std::string> m_subscribed_topics{};

    Buffer<dmn::DMesgPb> m_buffers{};
    dmn::DMesgPb m_last_dmesgsyspb{};
    std::map<std::string, long long> m_topic_running_counter{};

    ConflictCallbackTask m_conflict_callback_fn{};
    std::atomic<bool> m_in_conflict{};

    bool m_after_initial_playback{};
  }; // class DMesgHandler

  /**
   * @brief The constructor for DMesg.
   *
   * @param name   The identification name for the instantiated object
   * @param config The configuration key value (reserved for future use)
   */
  DMesg(std::string_view name, KeyValueConfiguration config = {});
  virtual ~DMesg() noexcept;

  DMesg(const DMesg &obj) = delete;
  const DMesg &operator=(const DMesg &obj) = delete;
  DMesg(DMesg &&obj) = delete;
  DMesg &operator=(DMesg &&obj) = delete;

  /**
   * @brief The method creates a new DMesgHandler, registers the handler to
   *        receive published message and returns the handler to the caller. It
   *        takes forward arguments as in DMesgHandler::openHandler(...).
   *
   * @param name            The name or unique identification to the handler
   * @param includeDMesgSys True if the handler will be notified of DMesgPb
   *                        sys message, default is false
   * @param filterFn        The functor callback that returns false to filter
   *                        out DMesgPB message, if no functor is provided,
   *                        no filter is performed
   * @param asyncProcessFn  The functor callback to process each notified
   *                        DMesgPb message
   *
   * @return newly created DMesgHandler
   */
  template <class... U> std::shared_ptr<DMesgHandler> openHandler(U &&...arg);

  /**
   * @brief The method creates a new handler, registers the handler to receive
   *        certain published message (by topic) and returns the handler to the
   *        caller.
   *
   * @param topics          The list of topics to be subscribed for the opened
   *                        handler
   * @param name            The name or unique identification to the handler
   * @param includeDMesgSys True if the handler will be notified of DMesgPb
   *                        sys message, default is false
   * @param filterFn        The functor callback that returns false to filter
   *                        out DMesgPB message, if no functor is provided,
   *                        no filter is performed
   * @param asyncProcessFn  The functor callback to process each notified
   *                        DMesgPb message
   *
   * @return newly created handler
   */
  template <class... U>
  std::shared_ptr<DMesgHandler> openHandler(std::vector<std::string> topics,
                                            U &&...arg);

  /**
   * @brief The method unregisters and removes the handler from the DMesg and
   *        then free the handler (if handlerToClose argument is the only
   *        shared pointer than one owned by DMesg).
   *
   * @param handlerToClose The handler to be closed
   */
  void closeHandler(std::shared_ptr<DMesgHandler> &handlerToClose);

protected:
  using Pub::publish;

  /**
   * @brief The method publishes system message through async context queue.
   *
   * @param dmesgSysPb The system DMesgPb message
   */
  void publishSysInternal(dmn::DMesgPb dmesgSysPb);

  /**
   * @brief The method publishes dmesgPb to registered subscribers. If the to be
   *        published dmesgPb' topic has smaller runningcounter than what is in
   *        the m_topic_running_counter, it means that the writer is out of sync
   *        and in race condition that its published dmn::DMesgPb' topic has a
   *        runningcounter that is early in value than the same topic's running
   *        counter published by the DMesg. In this case, we put the writer
   *        handler to be in conflict state, and throws exception for future
   *        write (of the writer) until the client of the handler manually marks
   *        it as conflict resolved. We do not put all handlers in conflict, but
   *        the particular writer' handler.
   *
   * @param dmesgPb The dmesgPb to be published
   */
  void publishInternal(dmn::DMesgPb dmesgPb) override;

  /**
   * @brief The method posts an asynchronous action in the publisher's singleton
   *        asynchronous thread context to reset the handler's conflict state.
   *
   * @param handlerName the identification string for the open handler
   */
  void resetHandlerConflictState(const DMesgHandler *handlerPtr);

private:
  /**
   * @brief The internal method is to be called in the publisher's singleton
   *        asynchronous thread context to playback last message of each topic.
   */
  void playbackLastTopicDMesgPbInternal();

  /**
   * @brief The method resets the handler conflict state, it must be called
   *        within the publisher' singleton asynchronous thread context to be
   *        thread safe and that is the same thread that puts the handler in
   *        conflict state.
   *
   * @param handlerName the identification string for the open handler
   */
  void resetHandlerConflictStateInternal(const DMesgHandler *handlerPtr);

  /**
   * data members for constructor to instantiate the object.
   */
  std::string m_name{};
  KeyValueConfiguration m_config{};

  /**
   * data members for internal logic.
   */
  std::vector<std::shared_ptr<DMesgHandler>> m_handlers{};
  std::map<std::string, long long> m_topic_running_counter{};
  std::map<std::string, dmn::DMesgPb> m_topic_last_dmesgpb{};
}; // class DMesg

template <class... U>
std::shared_ptr<DMesg::DMesgHandler> DMesg::openHandler(U &&...arg) {
  static const std::vector<std::string> emptyTopics{};

  auto handlerRet = this->openHandler(emptyTopics, std::forward<U>(arg)...);

  return handlerRet;
}

template <class... U>
std::shared_ptr<DMesg::DMesgHandler>
DMesg::openHandler(std::vector<std::string> topics, U &&...arg) {
  // this is primitive openHandler() method that will
  // - create the handler
  // - register the handler as subscriber
  // - chain up the different level of objects to its owner, handler's
  //   subscriber to handler, handler to the DMesg publisher
  // - then add an asynchronous task to run in the publisher singleton
  //   asynchronous thread context and the task will add the handler to the
  //   list of handlers known to the DMesg subscriber, playback prior data per
  //   topic, and set flag that the newly created handler has been fully
  //   initialized after playback of prior data.
  //
  // This allows us to maintain a singleton asynchronous thread context is
  // responsible for publishing and notifying data between subscriber and
  // publisher.

  std::shared_ptr<DMesg::DMesgHandler> handler =
      std::make_shared<DMesg::DMesgHandler>(std::forward<U>(arg)...);
  auto handlerRet = handler;

  this->registerSubscriber(&(handler->m_sub));
  handler->m_owner = this;

  /* The topic filter is executed within the DMesg singleton asynchronous
   * thread context, but the filter value is maintained per DMesgHandler,
   * and this allow the DMesg to be mutex free while thread safe.
   */
  handlerRet->m_subscribed_topics = topics;

  DMN_ASYNC_CALL_WITH_CAPTURE(
      {
        this->m_handlers.push_back(handler);
        this->playbackLastTopicDMesgPbInternal();
        handler->m_after_initial_playback = true;
      },
      this, handler);

  return handlerRet;
}

} // namespace dmn

#endif // DMN_DMESG_HPP_
