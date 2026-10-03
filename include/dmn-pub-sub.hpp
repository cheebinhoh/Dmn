/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-pub-sub.hpp
 * @brief Lightweight publish/subscribe adaptor classes as wrapper.
 *
 * Overview
 * --------
 * - This header provides a small, efficient publish/subscribe adaptor classes:
 *   * Dmn_Pub<T> publishes items of type T.
 *   * Dmn_Pub<T>::Dmn_Sub is the subscriber interface that receives items.
 *
 * Design pattern
 * --------------
 * - Adapter - it allows other subclasses to be adapted as publishers or
 *             subscribers.
 * - Observer - it defines many-to-many dependencies between objects so that
 *              when one object publishes a message, all dependent subscribers
 *              are notified, and a subscriber can subscribe more than one
 *              publishers.
 *
 * Key design goals
 * ----------------
 * - Simplicity: minimal API to publish, register and unregister subscribers.
 * - Correctness: clear ownership and lifetime semantics, safe cleanup on
 *   destruction.
 * - Concurrency: publish calls enqueue work asynchronously, then the
 *   publisher processes subscriber notifications serially.
 *
 * Threading and execution model
 * -----------------------------
 * - The Dmn_Pub is derived from Dmn_Async. Each Dmn_Pub object has its
 *   own singleton asynchronous execution context as provided by Dmn_Async.
 * - publish(const T&) schedules a delivery task in the publisher's async
 *   context. That task (publishInternal) performs buffering and invokes each
 *   accepted subscriber's notify() directly in the publisher's async context.
 * - Notifications are serialized with other publisher work. A subscriber
 *   that needs to perform expensive work should copy or enqueue the item into
 *   its own execution context before returning from notify().
 *
 * Synchronization
 * ---------------
 * - It is piggy back on Dmn_Async's synchronization job queue.
 *
 * Buffering and replay
 * --------------------
 * - The publisher keeps a bounded history (m_buffer) of up to m_capacity
 *   recently-published items.
 * - A capacity of zero is supported and disables replay: items are removed
 *   from the history immediately after publication, while live subscribers
 *   still receive notifications.
 * - When a subscriber registers, missed items from the buffer are replayed to
 *   the subscriber according to the subscriber's replay setting:
 *   - m_replayQuantity == -1 : replay all buffered items
 *   - m_replayQuantity == 0  : replay none
 *   - m_replayQuantity  > 0  : replay the last N items
 *
 * Filtering
 * ---------
 * - An optional filter function (Dmn_Pub_Filter_Task) can be supplied at
 *   construction. If provided, the filter is invoked for each (subscriber,
 *   item) pair to decide whether that subscriber should receive the item.
 *
 * Lifetime and cleanup
 * --------------------
 * - Dmn_Sub stores a list of back-pointer (m_pubs) to its publishers while
 *   registered.
 * - Dmn_Sub destruction synchronously unregisters from each publisher.
 *   Publisher ownership keeps a registered subscriber alive until the
 *   unregister task executes, after earlier queued publisher work.
 * - Dmn_Pub destruction unlinks subscribers on its async context and drains
 *   queued work. It does not make calls that begin concurrently with object
 *   destruction safe; callers must stop and join API-calling threads first.
 * - A derived Dmn_Pub must stop/join callers and drain its async context in its
 *   derived destructor before destroying state used by overridden
 *   publishInternal(). A derived Dmn_Sub's registered lifetime is retained by
 *   the publisher until serialized unregistration completes.
 *
 * Error handling and exception safety
 * -----------------------------------
 * - Exceptions from each filter or notify callback are recorded and delivery
 *   continues for other subscribers. Blocking publish rethrows the first
 *   callback exception after completing delivery; callers can retrieve all
 *   recorded failures with takeCallbackFailures().
 * - Callback failures during registration replay are recorded and do not
 *   prevent registration from completing.
 * - Destructors are noexcept; exceptions thrown during cleanup are swallowed to
 *   guarantee noexcept finalization.
 *
 * Usage summary
 * -------------
 * - Create a Dmn_Pub<T> with a name and optional capacity and filter.
 * - Derive from Dmn_Pub<T>::Dmn_Sub and implement notify(const T&, Dmn_Pub *).
 * - Call registerSubscriber() to register a subscriber (buffer replay occurs
 *   synchronously as part of registration).
 * - Call publish(item) to enqueue an asynchronous publish. Optionally pass
 *   block=true to wait until the publish task completes.
 *
 * See also
 * - dmn-async.hpp : asynchronous task execution and synchronization helpers.
 * - dmn-proc.hpp  : process-level helper macros used for mutex cleanup.
 */

#ifndef DMN_PUB_SUB_HPP_
#define DMN_PUB_SUB_HPP_

#include "dmn-async.hpp"
#include "dmn-blockingqueue-lf.hpp"
#include "dmn-blockingqueue-mt.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dmn {

template <typename T = std::string,
          template <class> class QueueType = Dmn_BlockingQueue_Mt>
class Dmn_Pub : public Dmn_Async<QueueType> {
public:
  /**
   * @brief Identifies which subscriber callback failed during publication.
   */
  enum class CallbackFailureKind { kFilter, kNotify };

  /**
   * @brief A recorded exception from a filter or subscriber notification.
   *
   * The exception can be inspected by rethrowing @c m_exception. Failures are
   * recorded in callback invocation order and remain available until drained
   * with @ref takeCallbackFailures().
   */
  struct CallbackFailure {
    CallbackFailureKind m_kind{};
    std::string m_subscriberName{"unknown"};
    std::exception_ptr m_exception{};
  };

  /**
   * Subscriber interface for receiving items published by Dmn_Pub<T>.
   *
   * Implementors should derive from Dmn_Pub<T>::Dmn_Sub and override
   * notify(const T&, Dmn_Pub *) to handle delivered items. The notify callback
   * is executed in the publisher's singleton asynchronous context.
   *
   * Lifetime notes:
   * - A Dmn_Sub holds back-pointers to publishers while registered. Its
   *   destructor synchronously queues unregistration; the publisher's shared
   *   ownership keeps it alive until that task runs after earlier queued
   *   publisher operations.
   * - Callers must prevent publisher API calls from racing with publisher
   *   destruction. A derived publisher must drain queued work in its own
   *   destructor before destroying state used by overridden publication logic.
   */
  class Dmn_Sub {
  public:
    explicit Dmn_Sub(ssize_t replayQuantity = -1)
        : m_replayQuantity{replayQuantity} {}
    virtual ~Dmn_Sub() noexcept;

    /**
     * @brief Return a diagnostic name for this subscriber.
     *
     * Override this method to identify the subscriber in recorded callback
     * failures. The default name is @c "unknown".
     */
    virtual auto GetName() const noexcept -> std::string_view {
      return "unknown";
    }

    Dmn_Sub(const Dmn_Sub &obj) = delete;
    Dmn_Sub &operator=(const Dmn_Sub &obj) = delete;
    Dmn_Sub(Dmn_Sub &&obj) = delete;
    Dmn_Sub &operator=(Dmn_Sub &&obj) = delete;

    /**
     * @brief Called to deliver a published item to this subscriber. This method
     * is invoked inside the publisher's asynchronous thread context.
     * Subclasses must implement this method to process received items.
     *
     * @param item The data item delivered by the publisher.
     * @param pub The pointer to the publisher (subject) that notifies the
     * observer.
     */
    virtual void notify(const T &item, Dmn_Pub *pub = nullptr) = 0;

    friend class Dmn_Pub;

  private:
    ssize_t m_replayQuantity{
        -1}; ///< Number of buffered items to replay on registration: -1 = all,
             ///< 0 = none, N = last N.
    std::vector<Dmn_Pub *> m_pubs{}; ///< Back-pointers to publishers this
                                     ///< subscriber is registered with.
  }; // class Dmn_Sub

  using Dmn_Pub_Filter_Task =
      std::function<bool(const Dmn_Sub *const, const T &t)>;

  /**
   * @brief Constructor
   *
   * @param name A human-readable name used by Dmn_Async for the publisher
   *             thread context.
   * @param capacity Maximum number of historical items kept for replay to new
   *                 subscribers. A value of zero disables replay while
   *                 preserving notifications to currently registered
   *                 subscribers.
   * @param filter_fn Optional filter function; if provided, it is invoked for
   *                  each (subscriber, item) pair to decide whether that
   *                  subscriber should receive the item.
   * @param callbackFailureCapacity Maximum number of callback failures to
   *                                 retain: -1 means unlimited, 0 disables
   *                                 storage, and a positive value keeps only
   *                                 the most recent failures.
   */
  explicit Dmn_Pub(std::string_view name, size_t capacity = 10,
                   Dmn_Pub_Filter_Task filter_fn = {},
                   ssize_t callbackFailureCapacity = -1);
  virtual ~Dmn_Pub() noexcept;

  Dmn_Pub(const Dmn_Pub &obj) = delete;
  const Dmn_Pub &operator=(const Dmn_Pub &obj) = delete;
  Dmn_Pub(Dmn_Pub &&obj) = delete;
  Dmn_Pub &operator=(Dmn_Pub &&obj) = delete;

  /**
   * @brief Publish an item to all registered subscribers. By default,
   * schedules the delivery in the publisher's asynchronous thread context
   * and returns without waiting. Subscriber notify() callbacks run serially
   * in that publisher context; pass block=true to wait for the publish task
   * to complete. If a callback throws, delivery continues to other
   * subscribers, then the first callback exception is rethrown when
   * @p block is true and is available through @ref takeCallbackFailures().
   *
   * @param item The data item to publish.
   * @param block The caller will be blocked waiting for item to be published,
   * default is false.
   *
   * Calls from multiple threads are ordered by the publisher queue's
   * successful enqueue order. The caller must prevent new API calls from
   * racing with publisher destruction.
   */
  void publish(const T &item, bool block = false);

  /**
   * @brief Retrieve and clear recorded filter/notification callback failures.
   *
   * The drain runs in the publisher's asynchronous context and this call waits
   * for it to finish. Do not call from that context (for example, from a
   * subscriber callback), because waiting for a task queued to the same context
   * would deadlock. Failures from non-blocking publication become available
   * after the corresponding work has completed; call @ref waitForEmpty() before
   * draining when the caller needs to include all currently queued work.
   * Blocking publication also records failures before rethrowing the first
   * callback exception to its caller. Failures during registration replay are
   * recorded as well; registration continues through all retained items and
   * does not rethrow those notification exceptions.
   *
   * @return Recorded filter/notification failures in invocation order.
   */
  auto takeCallbackFailures() -> std::vector<CallbackFailure>;

  /**
   * @brief Register a subscriber.
   *
   * Template parameters:
   *  - U : Dmn_Sub or class type that inherits from Dmn_Sub.
   *  - X... : parameter pack of argument types to be forwarded to constructor
   *           of class U.
   *
   * Register a subscriber of class interface/subclass from Dmn_Sub with
   * this publisher. After registration, items in the publisher's buffer are
   * replayed to the subscriber.
   *
   * The immediate semantics (synchronous registration) allow callers to rely
   * on the subscriber being registered when the call returns.
   *
   * @param arg Arguments forwarded to U::U() constructor.
   * @return std::shared_ptr<U> pointing to the instance of class U.
   */
  template <typename U, class... X>
    requires(sizeof...(X) != 1 ||
             !std::same_as<std::tuple_element_t<0, std::tuple<X...>>,
                           std::shared_ptr<U>>)
  auto registerSubscriber(X &&...arg) -> std::shared_ptr<U>;

  /**
   * @brief Register a subscriber of class interface/subclass from Dmn_Sub with
   * this publisher. After registration, items in the publisher's buffer are
   * replayed to the subscriber.
   *
   * The immediate semantics (synchronous registration) allow callers to rely
   * on the subscriber being registered when the call returns.
   *
   * @param sub shared pointer of object to be claimed by Dmn_Pub.
   */
  void registerSubscriber(std::shared_ptr<Dmn_Sub> sub);

  /**
   * @brief Deregister a previously registered subscriber.
   *
   * @param sub A pointer to a Dmn_Sub instance to deregister.
   */
  void unregisterSubscriber(Dmn_Sub *sub);

protected:
  /**
   * @brief Core implementation that performs buffering and iterates subscribers
   * to dispatch notifications. This runs inside the publisher's asynchronous
   * context (scheduled by publish())
   *
   * Subclasses may override to customize behavior. They must preserve
   * serialized publisher-context execution and drain queued work in their
   * derived destructor before destroying state accessed by this method.
   *
   * @param item The data item to deliver to subscribers.
   */
  virtual void publishInternal(const T &item);

private:
  void recordCallbackFailure(CallbackFailureKind kind, const Dmn_Sub &sub,
                             std::exception_ptr exception);

  std::string m_name{}; ///< Human-readable name forwarded to the async context.
  size_t m_capacity{};  ///< Maximum number of items retained in @c m_buffer.
  Dmn_Pub_Filter_Task
      m_filter_fn{}; ///< Optional per-(subscriber,item) delivery filter.
  ssize_t m_callbackFailureCapacity{};

  std::deque<T> m_buffer{}; ///< Bounded circular history buffer for replay.
  std::vector<std::shared_ptr<Dmn_Sub>>
      m_subscribers{}; ///< Currently registered subscribers.
  std::vector<CallbackFailure> m_callbackFailures{};
}; // class Dmn_Pub

// class Dmn_Pub::Dmn_Sub
template <typename T, template <class> class QueueType>
Dmn_Pub<T, QueueType>::Dmn_Sub::~Dmn_Sub() noexcept try {
  for (auto &pub : m_pubs) {
    pub->unregisterSubscriber(this);
  }
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

// class Dmn_Pub
template <typename T, template <class> class QueueType>
Dmn_Pub<T, QueueType>::Dmn_Pub(std::string_view name, size_t capacity,
                               Dmn_Pub_Filter_Task filter_fn,
                               ssize_t callbackFailureCapacity)
    : Dmn_Async<QueueType>(name), m_name{name}, m_capacity{capacity},
      m_filter_fn{filter_fn},
      m_callbackFailureCapacity{callbackFailureCapacity} {}

template <typename T, template <class> class QueueType>
Dmn_Pub<T, QueueType>::~Dmn_Pub() noexcept try {
  auto waitHandler = this->addExecTaskWithWait([this]() -> void {
    for (auto &sub : m_subscribers) {
      auto it = std::find(sub->m_pubs.begin(), sub->m_pubs.end(), this);
      assert(sub->m_pubs.end() != it);
      sub->m_pubs.erase(it);
    }
  });

  waitHandler->wait();

  this->waitForEmpty();
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T, template <class> class QueueType>
void Dmn_Pub<T, QueueType>::publish(const T &item, bool block) {
  if (block) {
    auto waitHandler = this->addExecTaskWithWait(
        [this, &item]() -> void { this->publishInternal(item); });

    waitHandler->wait();
  } else {
    this->addExecTask([this, item]() { this->publishInternal(item); });
  }
}

template <typename T, template <class> class QueueType>
void Dmn_Pub<T, QueueType>::recordCallbackFailure(
    CallbackFailureKind kind, const Dmn_Sub &sub,
    std::exception_ptr exception) {
  if (m_callbackFailureCapacity == 0) {
    return;
  }

  if (m_callbackFailureCapacity > 0 &&
      m_callbackFailures.size() >=
          static_cast<size_t>(m_callbackFailureCapacity)) {
    m_callbackFailures.erase(m_callbackFailures.begin());
  }

  m_callbackFailures.push_back(
      {kind, std::string{sub.GetName()}, std::move(exception)});
}

template <typename T, template <class> class QueueType>
auto Dmn_Pub<T, QueueType>::takeCallbackFailures()
    -> std::vector<CallbackFailure> {
  std::vector<CallbackFailure> failures{};

  auto waitHandler = this->addExecTaskWithWait(
      [this, &failures]() -> void { failures.swap(m_callbackFailures); });

  waitHandler->wait();

  return failures;
}

template <typename T, template <class> class QueueType>
void Dmn_Pub<T, QueueType>::publishInternal(const T &item) {
  /* Dmn_Async guarantees that publisher state is accessed by only one thread
   * in the asynchronous context, so m_subscribers does not need a mutex.
   * registerSubscriber() and unregisterSubscriber() run their mutations in
   * that context and wait for the corresponding task before returning.
   */

  /* Keep the published item in circular ring buffer for
   * efficient access to playback to new subscribers whose misses the
   * data.
   */

  m_buffer.push_back(item);

  if (m_buffer.size() > m_capacity) {
    // Erase a range of elements from the start to the 'excess' point
    // This performs all shifts in a single pass.
    m_buffer.erase(m_buffer.begin(),
                   m_buffer.begin() + (m_buffer.size() - m_capacity));
  }

  std::exception_ptr firstFailure{};
  for (auto &sub : m_subscribers) {
    bool accepted = true;

    if (m_filter_fn) {
      try {
        accepted = m_filter_fn(sub.get(), item);
      } catch (...) {
        auto failure = std::current_exception();
        recordCallbackFailure(CallbackFailureKind::kFilter, *sub, failure);

        if (!firstFailure) {
          firstFailure = std::move(failure);
        }

        continue;
      }
    }

    if (accepted) {
      try {
        sub->notify(item, this);
      } catch (...) {
        auto failure = std::current_exception();
        recordCallbackFailure(CallbackFailureKind::kNotify, *sub, failure);

        if (!firstFailure) {
          firstFailure = std::move(failure);
        }
      }
    }
  }

  if (firstFailure) {
    std::rethrow_exception(firstFailure);
  }
} // method publishInternal()

template <typename T, template <class> class QueueType>
void Dmn_Pub<T, QueueType>::registerSubscriber(std::shared_ptr<Dmn_Sub> sub) {
  auto waitHandler = this->addExecTaskWithWait([this, sub]() -> void {
    auto it = std::find(sub->m_pubs.begin(), sub->m_pubs.end(), this);
    if (it != sub->m_pubs.end()) {
      return;
    }

    sub->m_pubs.push_back(this);
    m_subscribers.push_back(sub);

    // resend the data items that the registered subscriber
    // miss.
    size_t numberOfItemsToBeSkipped = 0;
    if (sub->m_replayQuantity == 0) {
      numberOfItemsToBeSkipped = m_buffer.size();
    } else if (sub->m_replayQuantity > 0 &&
               m_buffer.size() > static_cast<size_t>(sub->m_replayQuantity)) {
      numberOfItemsToBeSkipped =
          m_buffer.size() - static_cast<size_t>(sub->m_replayQuantity);
    }

    auto startIt = std::next(m_buffer.begin(), numberOfItemsToBeSkipped);
    for (auto it = startIt; it != m_buffer.end(); it++) {
      try {
        sub->notify(*it, this);
      } catch (...) {
        recordCallbackFailure(CallbackFailureKind::kNotify, *sub,
                              std::current_exception());
      }
    }
  });

  waitHandler->wait();
}

template <typename T, template <class> class QueueType>
template <typename U, class... X>
  requires(sizeof...(X) != 1 ||
           !std::same_as<std::tuple_element_t<0, std::tuple<X...>>,
                         std::shared_ptr<U>>)
auto Dmn_Pub<T, QueueType>::registerSubscriber(X &&...arg)
    -> std::shared_ptr<U> {
  auto subSp = std::make_shared<U>(std::forward<X>(arg)...);

  registerSubscriber(subSp);

  return subSp;
}

template <typename T, template <class> class QueueType>
void Dmn_Pub<T, QueueType>::unregisterSubscriber(Dmn_Sub *sub) {
  auto waitHandler = this->addExecTaskWithWait([this, sub]() -> void {
    auto it = std::find(sub->m_pubs.begin(), sub->m_pubs.end(), this);
    assert(it != sub->m_pubs.end());

    sub->m_pubs.erase(it);

    m_subscribers.erase(
        std::remove_if(m_subscribers.begin(), m_subscribers.end(),
                       [sub](auto &sp) { return sp.get() == sub; }),
        m_subscribers.end());
  });

  waitHandler->wait();
}

} // namespace dmn

#endif // DMN_PUB_SUB_HPP_
