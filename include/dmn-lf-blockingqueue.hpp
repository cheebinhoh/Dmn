/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-lf-blockingqueue.hpp
 * @brief Thread-safe mutex lock free FIFO blocking queue for passing items
 *        between threads, the Michael-Scott lock free queue algorithm but
 *        adapted for empty queue pop blocking semantics.
 */

#ifndef DMN_LF_BLOCKINGQUEUE_HPP_
#define DMN_LF_BLOCKINGQUEUE_HPP_

#include <algorithm>
#include <atomic>
#include <cassert>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>

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
    std::atomic<Node *> m_next{};
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
   * @brief Remove and return the front item from the queue, blocking if empty,
   *        it throws exception if the queue is destroyed while the caller
   *        pop calls block waiting for item.
   *
   * @return The front item.
   */
  virtual auto pop() -> T;

  /**
   * @brief Attempt a non-blocking pop. Return std::nullopt if empty.
   *
   * @return optional item, or std::nullopt if the queue was empty.
   */
  virtual auto popNoWait() -> std::optional<T>;

  /**
   * @brief Push an rvalue into the queue (attempts move, with
   *        move_if_noexcept).
   *
   * Non-blocking and signals waiting consumers.
   *
   * @param item The value to push (rvalue reference).
   */
  virtual void push(T &item, bool move = true);

  /**
   * @brief Wait until the queue becomes empty and return the total number of
   *        items that have passed through the queue.
   *
   * This blocks until the queue is empty and asserts that all pushed items
   * have been popped (m_pop_count == m_push_count) before returning the
   * inbound count.
   *
   * @return The total number of items that have been passed through the queue.
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
  std::atomic<std::size_t> m_total_push_count{};
}; // class Dmn_Lf_BlockingQueue

template <typename T> Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue() {
  auto dummy = new Node;

  m_head.store(dummy);
  m_tail.store(dummy);
}

template <typename T>
Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue(std::initializer_list<T> list)
    : Dmn_Lf_BlockingQueue{} {}

template <typename T>
Dmn_Lf_BlockingQueue<T>::~Dmn_Lf_BlockingQueue() noexcept try {
  m_tail.store(nullptr);
  m_tail.notify_all();

  Node *ptr = m_head;

  while (nullptr != ptr) {
    Node *nextPtr = ptr->m_next;

    delete ptr;

    ptr = nextPtr;
  }

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
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T> auto Dmn_Lf_BlockingQueue<T>::pop() -> T {
  m_popcall_count.fetch_add(1, std::memory_order_acquire);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_popcall_count.fetch_sub(1, std::memory_order_release);
    m_popcall_count.notify_all();
  });

  auto data = popOptional(true);
  if (!data) {
    throw std::runtime_error("pop is interrupted, and return without data");
  }

  return std::move(*data);
}

template <typename T>
auto Dmn_Lf_BlockingQueue<T>::popOptional(bool wait) -> std::optional<T> {
  std::optional<T> res{};

  while (true) {
    Node *last = m_tail.load();
    Node *first = m_head.load();
    Node *next = first->m_next.load();

    if (nullptr == last) {
      break;
    } else if (first == m_head.load()) {
      if (first == last) {
        if (next == nullptr) {
          if (!wait) {
            break;
          }

          while (last == m_tail.load()) {
            m_tail.wait(last, std::memory_order_acquire);
          }

          continue;
        }

        m_tail.compare_exchange_strong(last, next); // Help move tail
      } else {
        res = std::move(next->m_data);

        if (m_head.compare_exchange_weak(first, next)) {
          delete first;

          break;
        } else {
          res = {};
        }
      }
    }
  }

  return res;
}

template <typename T>
auto Dmn_Lf_BlockingQueue<T>::popNoWait() -> std::optional<T> {
  m_popcall_count.fetch_add(1, std::memory_order_acquire);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_popcall_count.fetch_sub(1, std::memory_order_release);
    m_popcall_count.notify_all();
  });

  return popOptional(false);
}

template <typename T> void Dmn_Lf_BlockingQueue<T>::push(T &item, bool move) {
  m_pushcall_count.fetch_add(1, std::memory_order_acquire);

  // Use RAII to ensure the counter is decremented even if an exception occurs
  auto cleanup = make_scope_guard([&] {
    m_pushcall_count.fetch_sub(1, std::memory_order_release);
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

  newNode->m_data = std::move(item);

  Node *t{};
  Node *next{};

  while (true) {
    t = m_tail.load();
    next = t->m_next.load();

    if (t == m_tail.load()) { // Are tail and next consistent?
      if (next == nullptr) {
        if (t->m_next.compare_exchange_strong(next, newNode)) {
          break;
        }
      } else {
        m_tail.compare_exchange_strong(t, next);
      }
    }
  }

  m_tail.compare_exchange_strong(t, newNode);
  m_tail.notify_all();

  m_total_push_count.fetch_add(1, std::memory_order_release);
}

template <typename T> auto Dmn_Lf_BlockingQueue<T>::waitForEmpty() -> size_t {
  size_t res{};

  while (true) {
    Node *last = m_tail.load();
    Node *first = m_head.load();
    Node *next = first->m_next.load();

    if (first == m_head.load()) {
      if (first == last) {
        if (next == nullptr) {
          res = m_total_push_count.load(std::memory_order_acquire);

          break;
        }
      }
    }
  }

  return res;
}

} // namespace dmn

#endif // DMN_LF_BLOCKINGQUEUE_HPP_
