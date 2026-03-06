/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-blockingqueue-lf.hpp
 * @brief Thread-safe mutex lock and condition variable free FIFO blocking queue
 * for passing items between threads, the Michael-Scott lock free queue
 * algorithm but adapted for empty queue pop blocking semantics.
 */

#ifndef DMN_BLOCKINGQUEUE_LF_HPP_
#define DMN_BLOCKINGQUEUE_LF_HPP_

#include <iostream>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmn-proc.hpp"
#include "dmn-util.hpp"

#include "dmn-blockingqueue-interface.hpp"

namespace dmn {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string>
class Dmn_BlockingQueue_Lf : Dmn_BlockingQueue_Interface<T> {
  struct EpochData {
    std::chrono::microseconds::rep m_timestamp{};
    uint32_t m_id{};
  };

  static_assert(
      std::atomic<EpochData>::is_always_lock_free,
      "Type EpochIdentifierData does not support hardware-level CAS!");

  struct Node {
    T m_data{};
    std::atomic<Node *> m_next{nullptr};
  };

  struct InFlightGuard {
    Dmn_BlockingQueue_Lf *m_q{};
    bool m_entered{false};
    uint64_t m_epochId{};

    explicit InFlightGuard(Dmn_BlockingQueue_Lf *q) : m_q(q) {
      // fast reject
      if (m_q->m_shutdown_flag.test(std::memory_order_acquire)) {
        return;
      }

      m_q->m_in_flight.fetch_add(1, std::memory_order_acq_rel);
      m_entered = true;

      // close race: if destructor set shutdown concurrently, back out
      if (q->m_shutdown_flag.test(std::memory_order_acquire)) {
        m_q->m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        m_q->m_in_flight.notify_all();
        m_entered = false;
      } else {
        auto ep = m_q->m_epochData.load();

        TimePoint tpNow = std::chrono::steady_clock::now();
        auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(
            tpNow.time_since_epoch());

        if ((usecs.count() - ep.m_timestamp) >= 1000) {
          EpochData epNew{.m_timestamp = usecs.count(), .m_id = ep.m_id + 1};

          m_q->m_epochData.compare_exchange_strong(ep, epNew,
                                                   std::memory_order_release);
          m_epochId = epNew.m_id;
        } else {
          m_epochId = ep.m_id;
        }

        m_q->m_epochIdCount[m_epochId / 3].fetch_add(1,
                                                     std::memory_order_seq_cst);
      }
    }

    ~InFlightGuard() {
      if (!m_entered) {
        return;
      }

      m_q->m_epochIdCount[m_epochId / 3].fetch_sub(1,
                                                   std::memory_order_seq_cst);
      m_q->m_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      m_q->m_in_flight.notify_all();
    }

    explicit operator bool() const noexcept { return m_entered; }
  };

public:
  Dmn_BlockingQueue_Lf();
  Dmn_BlockingQueue_Lf(std::initializer_list<T> list);
  virtual ~Dmn_BlockingQueue_Lf() noexcept;

  Dmn_BlockingQueue_Lf(const Dmn_BlockingQueue_Lf<T> &obj) = delete;
  const Dmn_BlockingQueue_Lf<T> &
  operator=(const Dmn_BlockingQueue_Lf<T> &obj) = delete;
  Dmn_BlockingQueue_Lf(const Dmn_BlockingQueue_Lf<T> &&obj) = delete;
  Dmn_BlockingQueue_Lf<T> &operator=(Dmn_BlockingQueue_Lf<T> &&obj) = delete;

  /**
   * @brief Remove and return the front item from the queue, blocking if
   * empty, it throws exception if the queue is destroyed while the caller pop
   * calls block waiting for item.
   *
   * @return The front item.
   */
  virtual auto pop() -> T;

  /**
   * @brief Pop multiple items from the queue with optional timeout semantics.
   *
   * @warning The method does not guarantee that returned items are
   * consecutive and next to each other, but ordering of the returning items
   * in case that multiple threads are doing pop at the same time.
   *
   * Detailed semantics:
   * - count > 0 is required (asserted).
   * - If the queue has >= count items, this returns exactly count items.
   * - If the queue is empty or less than count items:
   *   - timeout == 0: wait indefinitely for count items (return exactly
   *     count).
   *   - timeout > 0: wait up to timeout microseconds for items.
   *     * If timeout expires and there is at least one item, return 1..count
   *       items (the current queue size).
   *     * If timeout expires and the queue is still empty, the function
   *       returns no item.
   *
   * The returned vector contains moved items removed from the queue.
   *
   * @param count   Number of desired items (must be > 0).
   * @param timeout Timeout in microseconds for waiting for the full count.
   *                A value of 0 means wait forever for the count items.
   * @return Vector of items (size == count on success without timeout, or
   *         between 1 and count if a timeout occurred after at least one item
   *         was produced).
   */
  virtual auto pop(size_t count, long timeout = 0) -> std::vector<T>;

  /**
   * @brief Attempt a non-blocking pop. Return std::nullopt if empty.
   *
   * @return optional item, or std::nullopt if the queue was empty.
   */
  virtual auto popNoWait() -> std::optional<T>;

  /**
   * @brief Push an rvalue into the queue (attempts move, with
   * move_if_noexcept or equivalent).
   *
   * @param item The value to push (rvalue reference).
   */
  virtual void push(T &&item);

  /**
   * @brief Push an lvalue into the queue, optionally moving it (using
   *        move_if_noexcept or equivalent).
   *
   * @param item The value to push (lvalue reference; may be moved-from if
   *             move is true).
   * @param move If true, attempt to move the value into the queue; otherwise
   *             copy it.
   */
  virtual void push(T &item, bool move = true);

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   *        items that have passed through the queue.
   *
   * @return The total number of items that have been passed through the
   * queue.
   */
  virtual auto waitForEmpty() -> size_t;

protected:
  /**
   * @brief Signal all waiting threads to wake up and return.
   *
   * Sets the m_shutdown flag and gradually exit all inflight threads.
   */
  virtual void stop();

  virtual auto popOptional(bool wait) -> std::optional<T>;

  template <class U> void pushImpl(U &&item);

private:
  static void cleanup_tunk_inflight(void *arg);

  std::atomic<Node *> m_head{};
  std::atomic<Node *> m_tail{};

  std::atomic<EpochData> m_epochData{};
  std::array<std::atomic<uint64_t>, 3> m_epochIdCount{};

  std::atomic<std::size_t> m_total_push_count{};

  std::atomic<std::uint64_t> m_in_flight{0};
  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_BlockingQueue_Lf

template <typename T> Dmn_BlockingQueue_Lf<T>::Dmn_BlockingQueue_Lf() {
  auto dummy = new Node;

  m_head.store(dummy);
  m_tail.store(dummy);

  TimePoint tpNow = std::chrono::steady_clock::now();
  auto usecs = std::chrono::duration_cast<std::chrono::microseconds>(
      tpNow.time_since_epoch());

  EpochData ep{.m_timestamp = usecs.count(), .m_id = 0};
  m_epochData.store(ep);
}

template <typename T>
Dmn_BlockingQueue_Lf<T>::Dmn_BlockingQueue_Lf(std::initializer_list<T> list)
    : Dmn_BlockingQueue_Lf{} {
  for (auto data : list) {
    this->push(data);
  }
}

template <typename T>
Dmn_BlockingQueue_Lf<T>::~Dmn_BlockingQueue_Lf() noexcept try {
  stop();

  Node *ptr = m_head.load(std::memory_order_acquire);
  while (nullptr != ptr) {
    Node *nextPtr = ptr->m_next;

    delete ptr;

    ptr = nextPtr;
  }
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T>
void Dmn_BlockingQueue_Lf<T>::cleanup_tunk_inflight(void *arg) {
  auto ptr = static_cast<std::unique_ptr<InFlightGuard> *>(arg);

  (*ptr).reset();
}

template <typename T> auto Dmn_BlockingQueue_Lf<T>::pop() -> T {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  auto data = popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::pop(size_t count, long timeout)
    -> std::vector<T> {
  assert(count > 0);

  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  std::vector<T> res{};

  auto end = std::chrono::high_resolution_clock::now() +
             std::chrono::microseconds(timeout);

  do {
    auto data = popOptional(false);
    if (data) {
      res.push_back(std::move(*data));
    } else {
      dmn::Dmn_Proc::yield();
    }
  } while (false == m_shutdown_flag.test(std::memory_order_acquire) &&
           res.size() < count &&
           (0 == timeout || std::chrono::high_resolution_clock::now() < end));

  return res;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::popOptional(bool wait) -> std::optional<T> {
  auto g = std::make_unique<InFlightGuard>(this);
  if (!(*g)) {
    // choose behavior: throw, return nullopt, etc.
    return std::nullopt;
  }

  std::optional<T> res{};

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_tunk_inflight, &g);

  while (true) {
    Node *last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      break;
    }

    Node *first = m_head.load(std::memory_order_acquire);
    Node *next = first->m_next.load(std::memory_order_acquire);

    if (first == m_head.load(std::memory_order_acquire)) {
      if (first == last) {
        if (next == nullptr) {
          if (!wait) {
            break;
          }

          while (last == m_tail.load(std::memory_order_acquire)) {
            dmn::Dmn_Proc::testcancel();
            dmn::Dmn_Proc::yield();
          }

          continue;
        }

        m_tail.compare_exchange_strong(
            last, next, std::memory_order_release); // Help move tail
      } else {
        if (m_head.compare_exchange_weak(first, next, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
          res = std::move(next->m_data);
          delete first; // NOT a hazard free delete

          break;
        }
      }
    }
  }

  DMN_PROC_CLEANUP_POP(0);

  return res;
}

template <typename T>
auto Dmn_BlockingQueue_Lf<T>::popNoWait() -> std::optional<T> {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  return popOptional(false);
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::push(T &&item) {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  // Preserve the original preference for noexcept-move (otherwise copy).
  pushImpl(std::move_if_noexcept(item));
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::push(T &item, bool move) {
  if (m_shutdown_flag.test(std::memory_order_acquire)) {
    throw std::runtime_error("Dmn_BlockingQueue_Lf object is shutting down");
  }

  if (move) {
    // Preserve the original preference for noexcept-move (otherwise copy).
    pushImpl(std::move_if_noexcept(item));
  } else {
    pushImpl(item); // copy
  }
}

template <typename T>
template <class U>
void Dmn_BlockingQueue_Lf<T>::pushImpl(U &&item) {
  auto g = std::make_unique<InFlightGuard>(this);
  if (!(*g)) {
    return;
  }

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_tunk_inflight, &g);

  Node *newNode = new Node;

  newNode->m_data = std::forward<U>(item);

  Node *last{};
  Node *next{};

  while (true) {
    last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      delete newNode;
      newNode = nullptr;

      throw std::runtime_error(
          "Dmn_BlockingQueue_Lf: push attempted on shutdown queue");
    }

    next = last->m_next.load(std::memory_order_acquire);

    if (last ==
        m_tail.load(
            std::memory_order_acquire)) { // Are tail and next consistent?
      if (next == nullptr) {
        if (last->m_next.compare_exchange_strong(next, newNode,
                                                 std::memory_order_acquire)) {
          break;
        }
      } else {
        m_tail.compare_exchange_strong(last, next, std::memory_order_acquire);
      }
    }
  }

  if (newNode) {
    m_tail.compare_exchange_strong(last, newNode, std::memory_order_acquire);
    m_tail.notify_all();

    m_total_push_count.fetch_add(1, std::memory_order_seq_cst);
  }

  DMN_PROC_CLEANUP_POP(0);
}

template <typename T> auto Dmn_BlockingQueue_Lf<T>::waitForEmpty() -> size_t {
  auto g = std::make_unique<InFlightGuard>(this);
  if (!(*g)) {
    return m_total_push_count.load(std::memory_order_acquire);
  }

  DMN_PROC_CLEANUP_PUSH(&Dmn_BlockingQueue_Lf<T>::cleanup_tunk_inflight, &g);

  while (true) {
    Node *last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      break;
    }

    Node *first = m_head.load(std::memory_order_acquire);
    Node *next = first->m_next.load(std::memory_order_acquire);

    if (first == m_head.load(std::memory_order_acquire)) {
      if (first == last) {
        if (next == nullptr) {
          break;
        }
      }
    }

    dmn::Dmn_Proc::yield();
  }

  DMN_PROC_CLEANUP_POP(0);

  return m_total_push_count.load(std::memory_order_acquire);
}

template <typename T> void Dmn_BlockingQueue_Lf<T>::stop() {
  // 1. set shutdown flag
  m_shutdown_flag.test_and_set(std::memory_order_release);

  // 2. detach m_tail and mark data as empty.
  m_tail.store(nullptr);
  m_tail.notify_all();

  // 3) wait for all threads that already "entered the epoch" to leave
  std::uint64_t n = m_in_flight.load(std::memory_order_acquire);
  while (n != 0) {
    m_in_flight.wait(n, std::memory_order_acquire);
    n = m_in_flight.load(std::memory_order_acquire);
  }
}

} // namespace dmn

#endif // DMN_BLOCKINGQUEUE_LF_HPP_
