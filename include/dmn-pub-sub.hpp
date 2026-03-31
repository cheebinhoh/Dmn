/**
 * Copyright © 2025 - 2026 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-pub-sub.hpp
 * @brief Lightweight publish/subscribe adaptor classes as wrapper.
 *
 * Overview
 * --------
 * - This header provides a small, efficient publish/subscribe adaptar classes:
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
 * - Concurrency: deliveries occur asynchronously and subscribers process
 *   notifications.
 *
 * Threading and execution model
 * -----------------------------
 * - The Dmn_Pub is derivade from Dmn_Async. Each Dmn_Pub object has its
 *   own singleton asynchronous execution context as provided by Dmn_Async.
 * - publish(const T&) schedules a delivery task in the publisher's async
 *   context. That task (publishInternal) performs buffering and schedules per-
 *   subscriber deliveries into each subscriber's publish() method.
 * - notify(const T&, Dmn_Pub *pub) is a pure virtual callback implemented by
 *   subscriber subclasses and is always invoked directly in async publisher
 *   context, and it is up to subscriber to decide how does the notify() method
 *   being called, note that subscriber is adviced to copy the notification to
 *   its context before perform expensive computation.
 *
 * Synchronization
 * ---------------
 * - It is piggy back on Dmn_Async's synchronization job queue.
 *
 * Buffering and replay
 * --------------------
 * - The publisher keeps a bounded history (m_buffer) of up to m_capacity
 *   recently-published items.
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
 * - Dmn_Sub stores a back-pointer (m_pubs) to its publishers while registered.
 * - Dmn_Sub destructor automatically unregisters from the publisher (if still
 *   registered) and waits for any pending asynchronous tasks to finish so a
 *   destroyed subscriber will not receive further notifications.
 * - Dmn_Pub destructor: unregister all the subscribers still dangling.
 * - The Dmn_Pub and Dmn_Sub does not maintain object lifetime and ownership,
 *   these are left it to the client of the classes.
 *
 * Error handling and exception safety
 * -----------------------------------
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
#include "dmn-proc.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dmn {

template <typename T = std::string,
          template <class> class QueueType = Dmn_BlockingQueue_Mt>
class Dmn_Pub : public Dmn_Async<QueueType> {
public:
  /**
   * Subscriber interface for receiving items published by Dmn_Pub<T>.
   *
   * Implementors should derive from Dmn_Pub<T>::Dmn_Sub and override
   * notify(const T&, Dmn_Pub *) to handle delivered items. The notify callback
   * will be executed inside the subscriber's own singleton asynchronous thread
   * context (i.e., the Dmn_Sub's Dmn_Async context).
   *
   * Lifetime notes:
   * - A Dmn_Sub holds a back-pointer m_pubs to the publisher while it is
   *   registered. The Dmn_Sub destructor will automatically unregister from
   *   its publisher (if still registered) and wait for pending asynchronous
   *   tasks to complete before returning.
   */
  class Dmn_Sub {
  public:
    explicit Dmn_Sub(ssize_t replayQuantity = -1)
        : m_replayQuantity{replayQuantity} {}
    virtual ~Dmn_Sub() noexcept;

    Dmn_Sub(const Dmn_Sub &obj) = delete;
    Dmn_Sub &operator=(const Dmn_Sub &obj) = delete;
    Dmn_Sub(Dmn_Sub &&obj) = delete;
    Dmn_Sub &operator=(Dmn_Sub &&obj) = delete;

    /**
     * @brief Called to deliver a published item to this subscriber. This method
     * is invoked inside the subscriber's asynchronous thread context.
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
        -1}; // -1 resend all, 0 no resend, resend up to number
    std::vector<Dmn_Pub *> m_pubs{};
  }; // class Dmn_Sub

  using Dmn_Pub_Filter_Task =
      std::function<bool(const Dmn_Sub *const, const T &t)>;

  /**
   * @brief Constructor
   *
   * @param name A human-readable name used by Dmn_Async for the publisher
   *             thread context.
   * @param capacity Maximum number of historical items kept for replay to new
   *                 subscribers. If capacity <= 0, behavior is implementation
   *                 defined (caller should pass a positive size).
   * @param filter_fn Optional filter function; if provided, it is invoked for
   *                  each (subscriber, item) pair to decide whether that
   *                  subscriber should receive the item.
   */
  explicit Dmn_Pub(std::string_view name, size_t capacity = 10,
                   Dmn_Pub_Filter_Task filter_fn = {});
  virtual ~Dmn_Pub() noexcept;

  Dmn_Pub(const Dmn_Pub &obj) = delete;
  const Dmn_Pub &operator=(const Dmn_Pub &obj) = delete;
  Dmn_Pub(Dmn_Pub &&obj) = delete;
  Dmn_Pub &operator=(Dmn_Pub &&obj) = delete;

  /**
   * @brief Publish an item to all registered subscribers. The publish() call is
   * non-blocking (with blocking optional) with respect to subscriber handling:
   * it schedules the delivery in the publisher's asynchronous thread context
   * (so the caller returns quickly) while delivery to each subscriber is
   * dispatched into the corresponding subscriber's asynchronous context.
   *
   * @param item The data item to publish.
   * @param block The caller will be blocked waiting for item to be published,
   * default is false.
   */
  void publish(const T &item, bool block = false);

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
   * Subclasses may override to customize behavior, but must honor the locking
   * and delivery expectations used by register/unregister and destructor.
   *
   * @param item The data item to deliver to subscribers.
   */
  virtual void publishInternal(const T &item);

private:
  /**
   * Configuration set at construction time.
   */
  std::string m_name{};
  size_t m_capacity{};
  Dmn_Pub_Filter_Task m_filter_fn{};

  std::deque<T> m_buffer{}; // bounded historical buffer for replay
  std::vector<std::shared_ptr<Dmn_Sub>> m_subscribers{};
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
                               Dmn_Pub_Filter_Task filter_fn)
    : Dmn_Async<QueueType>(name), m_name{name}, m_capacity{capacity},
      m_filter_fn{filter_fn} {}

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
void Dmn_Pub<T, QueueType>::publishInternal(const T &item) {
  /* Though through Dmn_Async (parent class), we have a mean to
   * guarantee that only one thread is executing the core logic
   * and manipulate the m_subscribers state in a singleton asynchronous
   * thread context and without the use of mutex to protect the m_subscribers,
   * but that also requires both registerSubscriber() and
   * unregisterSubscriber() API methods to have it side effects executed
   * in the singleton asynchronous thread context, and which means that upon
   * returning from both API methods, the client is not guaranteed that
   * it has registered or unregistered successfully.
   */

  /* Keep the published item in circular ring buffer for
   * efficient access to playback to new subscribers whose misses the
   * data.
   */

  m_buffer.push_back(item);

  std::size_t numOfElementToBeRemoved = 0;
  if (m_buffer.size() > m_capacity) {
    numOfElementToBeRemoved = m_buffer.size() - m_capacity;
  }

  while (numOfElementToBeRemoved > 0 && !m_buffer.empty()) {
    m_buffer.erase(m_buffer.begin());
    numOfElementToBeRemoved--;
  }

  for (auto &sub : m_subscribers) {
    if (!m_filter_fn || m_filter_fn(sub.get(), item)) {
      sub->notify(item, this);
    }
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
    if (sub->m_replayQuantity > 0 &&
        m_buffer.size() > static_cast<size_t>(sub->m_replayQuantity)) {
      numberOfItemsToBeSkipped =
          m_buffer.size() - static_cast<size_t>(sub->m_replayQuantity);
    }

    auto startIt = std::next(m_buffer.begin(), numberOfItemsToBeSkipped);
    for (auto it = startIt; it != m_buffer.end(); it++) {
      sub->notify(*it, this);
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
