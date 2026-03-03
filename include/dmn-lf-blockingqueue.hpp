/**
 * Copyright © 2025 Chee Bin HOH. All rights reserved.
 *
 * @file dmn-lf-blockingqueue.hpp
 * @brief Thread-safe mutex lock free FIFO blocking queue for passing items
 * between threads.
 *
 * <<<< NOTE: work in progress >>>>
 */

#ifndef DMN_LF_BLOCKINGQUEUE_HPP_
#define DMN_LF_BLOCKINGQUEUE_HPP_

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

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

  virtual void push(T &item, bool move = true);

  auto debugPrint() -> size_t;

protected:
  template <class U> void pushImpl(U &&item);

private:
  std::atomic<Node *> m_head{};
  std::atomic<Node *> m_tail{};
}; // class Dmn_Lf_BlockingQueue

template <typename T> Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue() {
  Node *dummy = new Node;

  m_head.store(dummy);
  m_tail.store(dummy);
}

template <typename T>
Dmn_Lf_BlockingQueue<T>::Dmn_Lf_BlockingQueue(std::initializer_list<T> list)
    : Dmn_Lf_BlockingQueue{} {}

template <typename T>
Dmn_Lf_BlockingQueue<T>::~Dmn_Lf_BlockingQueue() noexcept try {
  Node *ptr = m_head;

  while (nullptr != ptr) {
    Node *nextPtr = ptr->m_next;

    delete ptr;

    ptr = nextPtr;
  }
} catch (...) {
  // Destructors must be noexcept: swallow exceptions.
  return;
}

template <typename T> void Dmn_Lf_BlockingQueue<T>::push(T &item, bool move) {
  if (move) {
    // Preserve the original preference for noexcept-move (otherwise copy).
    pushImpl(std::move_if_noexcept(item));
  } else {
    pushImpl(item); // copy
  }
}

template <typename T> auto Dmn_Lf_BlockingQueue<T>::debugPrint() -> size_t {
  size_t count{};

  Node *first = m_head.load();

  Node *cur = first;
  while (nullptr != cur) {
    count++;

    cur = cur->m_next;
  }

  return count - 1; // remove dummy count;
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
        // Step 1: Try to link the new node to the end
        if (t->m_next.compare_exchange_strong(next, newNode)) {
          break; // Success!
        }
      } else {
        // Step 2: Tail is falling behind; try to help advance it
        m_tail.compare_exchange_strong(t, next);
      }
    }
  }

  // Step 3: Try to swing the tail to the new node
  m_tail.compare_exchange_strong(t, newNode);
}

} // namespace dmn

#endif // DMN_LF_BLOCKINGQUEUE_HPP_
