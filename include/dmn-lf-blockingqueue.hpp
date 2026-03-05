/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-lf-blockingqueue.hpp
 * @brief Thread-safe mutex lock and condition variable free FIFO blocking queue
 * for passing items between threads, the Michael-Scott lock free queue
 * algorithm but adapted for empty queue pop blocking semantics.
 */

#ifndef DMN_LF_BLOCKINGQUEUE_HPP_
#define DMN_LF_BLOCKINGQUEUE_HPP_

#include <algorithm>
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

namespace dmn {

/**
 * @brief Thread-safe FIFO buffer.
 *
 * Template parameter T is the stored item type.
 */
template <typename T = std::string> class Dmn_Lf_BlockingQueue {
  struct Node {
    T m_data{};
    std::atomic<Node *> m_next{nullptr};
  };

public:
  Dmn_Lf_BlockingQueue();
  Dmn_Lf_BlockingQueue(std::initializer_list<T> list);
  virtual ~Dmn_Lf_BlockingQueue() noexcept;

  Dmn_Lf_BlockingQueue(const Dmn_Lf_BlockingQueue<T> &obj) = delete;
  const Dmn_Lf_BlockingQueue<T> &
  operator=(const Dmn_Lf_BlockingQueue<T> &obj) = delete;
  Dmn_Lf_BlockingQueue(const Dmn_Lf_BlockingQueue<T> &&obj) = delete;
  Dmn_Lf_BlockingQueue<T> &operator=(Dmn_Lf_BlockingQueue<T> &&obj) = delete;

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
  virtual auto popOptional(bool wait) -> std::optional<T>;

  template <class U> void pushImpl(U &&item);

private:
  std::atomic<Node *> m_head{};
  std::atomic<Node *> m_tail{};

  std::atomic<std::size_t> m_popcall_count{};
  std::atomic<std::size_t> m_pushcall_count{};
  std::atomic<std::size_t> m_waitforemptycall_count{};

  std::atomic<std::size_t> m_total_push_count{};

  std::atomic_flag m_shutdown_flag{};
}; // class Dmn_Lf_BlockingQueue

template <typename T> Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue() {
  auto dummy = new Node;

  m_head.store(dummy);
  m_tail.store(dummy);
}

template <typename T>
Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue(std::initializer_list<T> list)
    : Dmn_Lf_BlockingQueue{} {
  for (auto data : list) {
    this->push(data);
  }
}

template <typename T>
Dmn_Lf_BlockingQueue<T>::~Dmn_Lf_BlockingQueue() noexcept try {
  m_shutdown_flag.test_and_set(std::memory_order_release);

  m_tail.store(nullptr);
  m_tail.notify_all();

  size_t pushcall_count{};
  while ((pushcall_count = m_pushcall_count.load(std::memory_order_acquire)) >
         0) {
    m_pushcall_count.wait(pushcall_count, std::memory_order_acquire);
  }

  size_t popcall_count{};
  while ((popcall_count = m_popcall_count.load(std::memory_order_acquire)) >
         0) {
    m_popcall_count.wait(popcall_count, std::memory_order_acquire);
  }

  size_t waitforemptycall_count{};
  while ((waitforemptycall_count =
              m_waitforemptycall_count.load(std::memory_order_acquire)) > 0) {
    m_waitforemptycall_count.wait(waitforemptycall_count,
                                  std::memory_order_acquire);
  }

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

template <typename T> auto Dmn_Lf_BlockingQueue<T>::pop() -> T {
  m_popcall_count.fetch_add(1, std::memory_order_relaxed);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_popcall_count.fetch_sub(1, std::memory_order_seq_cst);
    m_popcall_count.notify_all();
  });

  auto data = popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename T>
auto Dmn_Lf_BlockingQueue<T>::pop(size_t count, long timeout)
    -> std::vector<T> {
  assert(count > 0);

  m_popcall_count.fetch_add(1, std::memory_order_relaxed);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_popcall_count.fetch_sub(1, std::memory_order_seq_cst);
    m_popcall_count.notify_all();
  });

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
  } while (m_shutdown_flag.test(std::memory_order_acquire) &&
           res.size() < count &&
           (0 == timeout || std::chrono::high_resolution_clock::now() < end));

  return res;
}

template <typename T>
auto Dmn_Lf_BlockingQueue<T>::popOptional(bool wait) -> std::optional<T> {
  std::optional<T> res{};

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
            m_tail.wait(last, std::memory_order_acquire);
          }

          continue;
        }

        m_tail.compare_exchange_strong(
            last, next, std::memory_order_release); // Help move tail
      } else {
        if (m_head.compare_exchange_weak(first, next,
                                         std::memory_order_release)) {
          res = std::move(next->m_data);
          delete first; // NOT a hazard free delete

          break;
        }
      }
    }
  }

  return res;
}

template <typename T>
auto Dmn_Lf_BlockingQueue<T>::popNoWait() -> std::optional<T> {
  m_popcall_count.fetch_add(1, std::memory_order_relaxed);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_popcall_count.fetch_sub(1, std::memory_order_seq_cst);
    m_popcall_count.notify_all();
  });

  return popOptional(false);
}

template <typename T> void Dmn_Lf_BlockingQueue<T>::push(T &&item) {
  m_pushcall_count.fetch_add(1, std::memory_order_relaxed);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_pushcall_count.fetch_sub(1, std::memory_order_seq_cst);
    m_pushcall_count.notify_all();
  });

  // Preserve the original preference for noexcept-move (otherwise copy).
  pushImpl(std::move_if_noexcept(item));
}

template <typename T> void Dmn_Lf_BlockingQueue<T>::push(T &item, bool move) {
  m_pushcall_count.fetch_add(1, std::memory_order_relaxed);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_pushcall_count.fetch_sub(1, std::memory_order_seq_cst);
    m_pushcall_count.notify_all();
  });

  if (move) {
    // Preserve the original preference for noexcept-move (otherwise copy).
    pushImpl(std::move_if_noexcept(item));
  } else {
    pushImpl(item); // copy
  }
}

template <typename T>
template <class U>
void Dmn_Lf_BlockingQueue<T>::pushImpl(U &&item) {
  Node *newNode = new Node;

  Node *last{};
  Node *next{};

  while (true) {
    last = m_tail.load(std::memory_order_acquire);
    if (nullptr == last) {
      delete newNode;
      newNode = nullptr;

      break;
    }

    next = last->m_next.load(std::memory_order_acquire);

    if (last ==
        m_tail.load(
            std::memory_order_acquire)) { // Are tail and next consistent?
      if (next == nullptr) {

        newNode->m_data = std::forward<U>(item);
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
}

template <typename T> auto Dmn_Lf_BlockingQueue<T>::waitForEmpty() -> size_t {
  m_waitforemptycall_count.fetch_add(1, std::memory_order_relaxed);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_waitforemptycall_count.fetch_sub(1, std::memory_order_seq_cst);
    m_waitforemptycall_count.notify_all();
  });

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

  return m_total_push_count.load(std::memory_order_acquire);
}

} // namespace dmn

#endif // DMN_LF_BLOCKINGQUEUE_HPP_
