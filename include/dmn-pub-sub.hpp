/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-pub-sub.hpp
 * @brief Lightweight publish/subscribe helpers built on top of Dmn_Async.
 *
 * Overview
 * - This header provides a small, efficient publish/subscribe utility:
 *   * Dmn_Pub<T> publishes items of type T.
 *   * Dmn_Pub<T>::Dmn_Sub is the subscriber interface that receives items.
 *
 * Key design goals
 * - Simplicity: minimal API to publish, register and unregister subscribers.
 * - Correctness: clear ownership and lifetime semantics, safe cleanup on
 *   destruction.
 * - Concurrency: deliveries occur asynchronously and subscribers process
 *   notifications inside their own Dmn_Async context.
 *
 * Threading and execution model
 * - Both Dmn_Pub and Dmn_Sub derive from Dmn_Async. Each object has its own
 *   singleton asynchronous execution context as provided by Dmn_Async.
 * - publish(const T&) schedules a delivery task in the publisher's async
 *   context. That task (publishInternal) performs buffering and schedules per-
 *   subscriber deliveries into each subscriber's async context via
 *   Dmn_Sub::notifyInternal.
 * - notify(const T&) is a pure virtual callback implemented by subscriber
 *   subclasses and is always invoked inside the subscriber's async context.
 *
 * Synchronization
 * - A pthread mutex (m_mutex) protects the publisher's internal state:
 *   - the subscriber list (m_subscribers) and the replay buffer (m_buffer).
 * - The mutex provides synchronous semantics for registerSubscriber() and
 *   unregisterSubscriber(): callers return only after registration state is
 *   updated and (for register) buffer replay has been enqueued to the
 *   subscriber.
 * - publishInternal locks the same mutex while updating the buffer and while
 *   iterating the subscriber list to schedule deliveries.
 *
 * Buffering and replay
 * - The publisher keeps a bounded history (m_buffer) of up to m_capacity
 *   recently-published items.
 * - When a subscriber registers, missed items from the buffer are replayed to
 *   the subscriber according to the subscriber's replay setting:
 *   - m_replayQuantity == -1 : replay all buffered items
 *   - m_replayQuantity == 0  : replay none
 *   - m_replayQuantity  > 0  : replay up to the last N items
 *
 * Filtering
 * - An optional filter function (Dmn_Pub_Filter_Task) can be supplied at
 *   construction. If provided, the filter is invoked for each (subscriber,
 *   item) pair to decide whether that subscriber should receive the item.
 *
 * Lifetime and cleanup
 * - Dmn_Sub stores a back-pointer (m_pub) to its publisher while registered.
 * - Dmn_Sub destructor automatically unregisters from the publisher (if still
 *   registered) and waits for any pending asynchronous tasks to finish so a
 *   destroyed subscriber will not receive further notifications.
 * - Dmn_Pub destructor:
 *   - acquires the internal mutex, nils subscribers' m_pub to avoid
 *     dangling back-references, waits for its async queue to drain, and then
 *     releases the mutex. This ensures no further deliveries will be
 *     scheduled to subscribers from this publisher after destruction starts.
 *
 * Error handling and exception safety
 * - Constructors will throw on pthread mutex initialization failure.
 * - Destructors are noexcept; exceptions thrown during cleanup are swallowed to
 *   guarantee noexcept finalization.
 *
 * Usage summary
 * - Create a Dmn_Pub<T> with a name and optional capacity and filter.
 * - Derive from Dmn_Pub<T>::Dmn_Sub and implement notify(const T&).
 * - Call registerSubscriber() to register a subscriber (buffer replay occurs
 *   synchronously as part of registration).
 * - Call publish(item) to enqueue an asynchronous publish. Optionally pass
 *   block=true to wait until the publish task completes.
 *
 * Limitations and notes
 * - Capacity <= 0 is not validated beyond construction; callers should pass a
 *   sensible positive capacity.
 * - The implementation uses pthread mutexes and expects the Dmn_Async
 *   primitives/macros (e.g., DMN_ASYNC_CALL_WITH_CAPTURE and
 *   DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP) to integrate with thread/task
 *   cleanup correctly.
 *
 * See also
 * - dmn-async.hpp : asynchronous task execution and synchronization helpers.
 * - dmn-proc.hpp  : process-level helper macros used for mutex cleanup.
 */

#ifndef DMN_PUB_SUB_HPP_
#define DMN_PUB_SUB_HPP_

#include "pthread.h"

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "dmn-async.hpp"
#include "dmn-proc.hpp"

namespace dmn {

template <typename T = std::string> class Dmn_Pub : public Dmn_Async {
public:
  /**
   * Subscriber interface for receiving items published by Dmn_Pub<T>.
   *
   * Implementors should derive from Dmn_Pub<T>::Dmn_Sub and override
   * notify(const T&) to handle delivered items. The notify callback will be
   * executed inside the subscriber's own singleton asynchronous thread
   * context (i.e., the Dmn_Sub's Dmn_Async context).
   *
   * Lifetime notes:
   * - A Dmn_Sub holds a back-pointer m_pub to the publisher while it is
   *   registered. The Dmn_Sub destructor will automatically unregister from
   *   its publisher (if still registered) and wait for pending asynchronous
   *   tasks to complete before returning.
   */
  class Dmn_Sub : public Dmn_Async {
  public:
    explicit Dmn_Sub(ssize_t replayQuantity = -1)
        : m_replayQuantity{replayQuantity} {}
    virtual ~Dmn_Sub() noexcept;

    Dmn_Sub(const Dmn_Sub &obj) = delete;
    const Dmn_Sub &operator=(const Dmn_Sub &obj) = delete;
    Dmn_Sub(Dmn_Sub &&obj) = delete;
    Dmn_Sub &operator=(Dmn_Sub &&obj) = delete;

    /**
     * notify
     *
     * Called to deliver a published item to this subscriber. This method is
     * invoked inside the subscriber's asynchronous thread context. Subclasses
     * must implement this method to process received items.
     *
     * @param item The data item delivered by the publisher.
     */
    virtual void notify(const T &item) = 0;

    friend class Dmn_Pub;

  private:
    /**
     * notifyInternal
     *
     * Internal helper called by Dmn_Pub to schedule delivery into the
     * subscriber's asynchronous context. Do not call from user code; implement
     * notify() instead.
     *
     * @param item The data item to deliver.
     */
    void notifyInternal(const T &item);

    ssize_t m_replayQuantity{
        -1}; // -1 resend all, 0 no resend, resend up to number
    Dmn_Pub *m_pub{};
  }; // class Dmn_Sub

  using Dmn_Pub_Filter_Task =
      std::function<bool(const Dmn_Sub *const, const T &t)>;

  /**
   * Constructor
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
   * publish
   *
   * Publish an item to all registered subscribers. The publish() call is
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
   * registerSubscriber
   *
   * Register a subscriber with this publisher. This method acquires an
   * internal mutex and returns only after the subscriber has been added to
   * the publisher's subscriber list. After registration, items in the
   * publisher's buffer are replayed to the subscriber (via notifyInternal).
   *
   * The immediate semantics (synchronous registration) allow callers to rely
   * on the subscriber being registered when the call returns.
   *
   * @param sub A pointer to a Dmn_Sub instance to register.
   */
  void registerSubscriber(Dmn_Sub *sub);

  /**
   * unregisterSubscriber
   *
   * Deregister a previously registered subscriber. This method acquires the
   * internal mutex and returns only after the subscriber has been removed.
   * If the subscriber was not registered, this is a no-op.
   *
   * @param sub A pointer to a Dmn_Sub instance to deregister.
   */
  void unregisterSubscriber(Dmn_Sub *sub);

protected:
  /**
   * publishInternal
   *
   * Core implementation that performs buffering and iterates subscribers to
   * dispatch notifications. This runs inside the publisher's asynchronous
   * context (scheduled by publish()) and holds the internal mutex while
   * manipulating state.
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

  /**
   * Internal state protected by m_mutex.
   */
  std::deque<T> m_buffer{};  // bounded historical buffer for replay
  pthread_mutex_t m_mutex{}; // protects subscriber list & buffer
  std::vector<Dmn_Sub *> m_subscribers{};
}; // class Dmn_Pub

// class Dmn_Pub::Dmn_Sub
template <typename T> Dmn_Pub<T>::Dmn_Sub::~Dmn_Sub() noexcept try {
  if (m_pub) {
    m_pub->unregisterSubscriber(this);
  }

  this->waitForEmpty(); // make sure that no asynchronoous task pending.
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T> void Dmn_Pub<T>::Dmn_Sub::notifyInternal(const T &item) {
  DMN_ASYNC_CALL_WITH_CAPTURE({ this->notify(item); }, this, item);
}

// class Dmn_Pub
template <typename T>
Dmn_Pub<T>::Dmn_Pub(std::string_view name, size_t capacity,
                    Dmn_Pub_Filter_Task filter_fn)
    : Dmn_Async(name), m_name{name}, m_capacity{capacity},
      m_filter_fn{filter_fn} {

  int err = pthread_mutex_init(&m_mutex, nullptr);
  if (err) {
    throw std::runtime_error(strerror(err));
  }
}

template <typename T> Dmn_Pub<T>::~Dmn_Pub() noexcept try {
  pthread_mutex_lock(&m_mutex);

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  for (auto &sub : m_subscribers) {
    sub->m_pub = nullptr;
  }

  this->waitForEmpty();

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  pthread_mutex_unlock(&m_mutex);
} catch (...) {
  // explicit return to resolve exception as destructor must be noexcept
  return;
}

template <typename T> void Dmn_Pub<T>::publish(const T &item, bool block) {
  if (block) {
    auto waitHandler = this->addExecTaskWithWait(
        [this, &item]() -> void { this->publishInternal(item); });

    waitHandler->wait();
  } else {
    DMN_ASYNC_CALL_WITH_CAPTURE({ this->publishInternal(item); }, this, item);
  }
}

template <typename T> void Dmn_Pub<T>::publishInternal(const T &item) {
  /* Though through Dmn_Async (parent class), we have a mean to
   * guarantee that only one thread is executing the core logic
   * and manipulate the m_subscribers state in a singleton asynchronous
   * thread context and without the use of mutex to protect the m_subscribers,
   * but that also requires both registerSubscriber() and
   * unregisterSubscriber() API methods to have it side effects executed
   * in the singleton asynchronous thread context, and which means that upon
   * returning from both API methods, the client is not guaranteed that
   * it has registered or unregistered successfully.
   *
   * A simple mutex will guarantee that and the penalty of mutex is small
   * as most clients are registered upon application brought up, and only
   * deregister upon application shutdown.
   */
  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

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
    if (!m_filter_fn || m_filter_fn(sub, item)) {
      sub->notifyInternal(item);
    }
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }
} // method publishInternal()

template <typename T> void Dmn_Pub<T>::registerSubscriber(Dmn_Sub *sub) {
  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  if (this == sub->m_pub) {
    return;
  }

  assert(nullptr == sub->m_pub ||
         "The subscriber has been registered with another publisher");

  sub->m_pub = this;
  m_subscribers.push_back(sub);

  // resend the data items that the registered subscriber
  // miss.
  size_t numberOfItemsToBeSkipped = 0;
  if (sub->m_replayQuantity > 0 &&
      m_buffer.size() > static_cast<size_t>(sub->m_replayQuantity)) {
    numberOfItemsToBeSkipped =
        m_buffer.size() - static_cast<size_t>(sub->m_replayQuantity);
  }

  for (auto &item : m_buffer) {
    if (numberOfItemsToBeSkipped > 0) {
      numberOfItemsToBeSkipped--;
    } else {
      sub->notifyInternal(item);
    }
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }
}

template <typename T> void Dmn_Pub<T>::unregisterSubscriber(Dmn_Sub *sub) {
  int err = pthread_mutex_lock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }

  DMN_PROC_ENTER_PTHREAD_MUTEX_CLEANUP(&m_mutex);

  if (nullptr != sub->m_pub) {
    assert(this == sub->m_pub ||
           "The subscriber' registered publisher is NOT this" == nullptr);

    sub->m_pub = nullptr;

    m_subscribers.erase(
        std::remove(m_subscribers.begin(), m_subscribers.end(), sub),
        m_subscribers.end());
  }

  DMN_PROC_EXIT_PTHREAD_MUTEX_CLEANUP();

  err = pthread_mutex_unlock(&m_mutex);
  if (err) {
    throw std::runtime_error(strerror(err));
  }
}

} // namespace dmn

#endif // DMN_PUB_SUB_HPP_
