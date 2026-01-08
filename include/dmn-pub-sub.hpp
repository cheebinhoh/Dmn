/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-pub-sub.hpp
 * @brief Simple publish/subscribe utilities.
 *
 * This header provides a lightweight publisher/subscriber pattern where:
 * - Dmn_Pub<T> is a publisher that publishes items of type T.
 * - Dmn_Pub<T>::Dmn_Sub is a subscriber interface that receives items of type
 * T.
 *
 * Design notes:
 * - Each Dmn_Pub and each Dmn_Sub inherit from Dmn_Async and therefore execute
 *   their internal work (such as delivering messages or running subscriber
 *   callbacks) in their own singleton asynchronous thread context as provided
 *   by Dmn_Async.
 *
 * - publish(const T&) enqueues a delivery task into the publisher's async
 *   context. Delivery to subscribers happens from that context (via
 *   publishInternal). Delivery to each subscriber is dispatched into the
 *   subscriber's async context using notifyInternal so subscriber handling
 *   runs in the subscriber's thread.
 *
 * - Thread-safety:
 *   * A pthread mutex (m_mutex) protects the publisher's internal state that
 *     must be consistent for callers executing synchronously (e.g., register
 *     and unregister). publishInternal also locks the mutex while updating the
 *     buffer and iterating subscribers. This mutex ensures that register/un-
 *     register operations are synchronized with publish operations and with
 *     destructor cleanup.
 *   * The Dmn_Async base class provides a guarantee that tasks scheduled via
 *     the async mechanism execute in a single asynchronous thread for that
 *     object. However, to provide synchronous semantics for register/unregister
 *     (so callers can observe registration state immediately on return), the
 *     mutex is still used.
 *
 * - Buffering and replay:
 *   * The publisher keeps a bounded historical buffer (m_buffer) of published
 *     items of size up to m_capacity. When a subscriber registers, the buffer
 *     items are replayed to the new subscriber so it does not miss recent
 *     history.
 *
 * - Lifetime and cleanup:
 *   * A Dmn_Sub tracks its publisher via m_pub. The Dmn_Sub destructor
 *     automatically unregisters itself from the publisher (if still present)
 *     and waits for pending async tasks to complete to avoid delivering to a
 *     destroyed subscriber.
 *   * The Dmn_Pub destructor clears subscriber references and waits for its
 *     own async work to finish. It sets each subscriber's m_pub to nullptr to
 *     avoid dangling back-references.
 *
 * - Filtering:
 *   * A publisher may be constructed with an optional filter function that
 *     decides whether a given subscriber should receive a particular item.
 *
 * Usage summary:
 * - Create a Dmn_Pub<T> with a name and optional capacity and filter.
 * - Derive from Dmn_Pub<T>::Dmn_Sub and implement notify(const T&).
 * - Call registerSubscriber() to register. Items already in the buffer are
 *   replayed to the subscriber on registration.
 * - Call publish(item) to publish asynchronously to all matching subscribers.
 *
 * Notes:
 * - This header documents intent and threading model; consult Dmn_Async for
 *   details of the asynchronous execution model and cleanup semantics.
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

template <typename T> class Dmn_Pub : public Dmn_Async {
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
    Dmn_Sub() = default;
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
  Dmn_Pub(std::string_view name, ssize_t capacity = 10,
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
   * non-blocking with respect to subscriber handling: it schedules the
   * delivery in the publisher's asynchronous thread context (so the caller
   * returns quickly) while delivery to each subscriber is dispatched into the
   * corresponding subscriber's asynchronous context.
   *
   * @param item The data item to publish.
   */
  void publish(const T &item);

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
  ssize_t m_capacity{};
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
Dmn_Pub<T>::Dmn_Pub(std::string_view name, ssize_t capacity,
                    Dmn_Pub_Filter_Task filter_fn)
    : Dmn_Async(name), m_name{name}, m_capacity{capacity},
      m_filter_fn{filter_fn} {

  int err = pthread_mutex_init(&m_mutex, NULL);
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

template <typename T> void Dmn_Pub<T>::publish(const T &item) {
  DMN_ASYNC_CALL_WITH_CAPTURE({ this->publishInternal(item); }, this, item);
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
  std::size_t numOfElementToBeRemoved =
      std::max(0ul, m_buffer.size() - m_capacity);
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
  for (auto &item : m_buffer) {
    sub->notifyInternal(item);
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
